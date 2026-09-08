#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>
#ifdef __linux__
#include <sys/resource.h>
#endif

#include "data_durability.hpp"
#include "owned_test_root.hpp"
#include "raft_core.hpp"
#include "database_engine.hpp"
#include "lsm.hpp"

namespace fs = std::filesystem;

int main() {
#ifdef __linux__
    // The payload exceeds this ceiling; a full JSON/string materialization must fail.
    rlimit memoryLimit{};
    if (getrlimit(RLIMIT_AS, &memoryLimit) != 0) return 8;
    memoryLimit.rlim_cur = std::min(memoryLimit.rlim_cur, rlim_t{128U * 1024U * 1024U});
    if (setrlimit(RLIMIT_AS, &memoryLimit) != 0) return 8;
#endif
    const OwnedTestRoot ownedRoot("raft-snapshot-metadata");
    const fs::path snapshot = ownedRoot.dataRoot() / ".snapshot";
    {
        std::ofstream output(snapshot, std::ios::binary | std::ios::trunc);
        output << R"({"commitIndex" : 42 ,"complete" : true ,"currentTerm":7,"lastIncludedIndex" : 42 ,"lastIncludedTerm" : 6 ,"lsm_payload":{"files":[{"content":")";
        const std::string chunk(1024U * 1024U, 'x');
        for (int i = 0; i < 256; ++i) output.write(chunk.data(), chunk.size());
        output << R"("}]},"version":1})";
    }
    const std::string checksum =
        pacificdb::durability::ChecksumCalculator::sha256File(snapshot.string());
    std::ofstream(snapshot.string() + ".sha256") << checksum << "\n";

    uint64_t index = 0;
    uint64_t term = 0;
    std::string reason;
    if (!RaftCore::readPersistedSnapshotMetadata(
            snapshot.string(), index, term, reason) || index != 42 || term != 6) {
        std::cerr << "bounded snapshot metadata read failed: " << reason << "\n";
        return 1;
    }

    std::ofstream(snapshot, std::ios::binary | std::ios::app) << 'x';
    if (RaftCore::readPersistedSnapshotMetadata(
            snapshot.string(), index, term, reason) ||
        reason != "snapshot_file_checksum_mismatch") {
        std::cerr << "corrupt snapshot was accepted\n";
        return 2;
    }

    const auto writeFixture = [&](const std::string& bytes) {
        std::ofstream(snapshot, std::ios::binary | std::ios::trunc) << bytes;
        std::ofstream(snapshot.string() + ".sha256") <<
            pacificdb::durability::ChecksumCalculator::sha256File(snapshot.string()) << "\n";
    };
    const std::string header = R"({"commitIndex":42,"complete":true,"lastIncludedIndex":42,"lastIncludedTerm":6,"lsm_payload":{},"version":)";
    for (const auto& invalid : std::vector<std::string>{
            header + "2}", header + "-1}", header + "1.5}",
            header + "1,\"version\":2}",
            header + "1,\"\\u0076ersion\":1}", header + "1} trailing",
            header + "1", header + "1,}",
            header + "1,\"padding\":\"" + std::string(65536, 'x') + "\"}",
            R"({"nested":{"commitIndex":42,"complete":true,"lastIncludedIndex":42,"lastIncludedTerm":6},"lsm_payload":{},"version":1})",
            R"({"commitIndex":42,"complete":1,"lastIncludedIndex":42,"lastIncludedTerm":6,"lsm_payload":{},"version":1})",
            R"({"commitIndex":43,"complete":true,"lastIncludedIndex":42,"lastIncludedTerm":6,"lsm_payload":{},"version":1})",
            R"({"commitIndex":42,"complete":true,"lastIncludedIndex":42.0,"lastIncludedTerm":6,"lsm_payload":{},"version":1})",
            R"({"commitIndex":42,"complete":true,"lastIncludedIndex":42,"lastIncludedTerm":-1,"lsm_payload":{},"version":1})",
            R"({"commitIndex":42,"complete":true,"lastIncludedIndex":42,"lastIncludedTerm":18446744073709551616,"lsm_payload":{},"version":1})",
            R"({"commitIndex":42,"complete":true,"lastIncludedIndex":42,"lastIncludedTerm":6,"version":1})",
            R"({"commitIndex":42,"complete":true,"lastIncludedIndex":42,"lastIncludedTerm":6,"lsm_payload":[],"version":1})",
            R"({"lsm_payload":{"files":[}},"version":1})"}) {
        writeFixture(invalid);
        if (RaftCore::readPersistedSnapshotMetadata(snapshot.string(), index, term, reason) ||
            index != 0 || term != 0) {
            std::cerr << "invalid snapshot envelope accepted: " << invalid.substr(0, 160) << "\n";
            return 3;
        }
    }
    writeFixture(R"({"lsm_payload":{"files":[{"content":"escaped\"}{[]\\"}]},"version":1,"complete":true,"commitIndex":42,"lastIncludedTerm":6,"lastIncludedIndex":42})");
    if (!RaftCore::readPersistedSnapshotMetadata(snapshot.string(), index, term, reason) ||
        index != 42 || term != 6) {
        std::cerr << "valid reordered envelope rejected: " << reason << "\n";
        return 4;
    }
    fs::remove(snapshot.string() + ".sha256");
    if (RaftCore::readPersistedSnapshotMetadata(snapshot.string(), index, term, reason) ||
        index != 0 || term != 0) return 6;
    std::ofstream(snapshot.string() + ".sha256") << std::string(1024 * 1024, 'a');
    if (RaftCore::readPersistedSnapshotMetadata(snapshot.string(), index, term, reason) ||
        index != 0 || term != 0) return 7;

    // Even an empty Raft log cannot make a corrupt snapshot recoverable.
    writeFixture(header + "2}");
#ifdef _WIN32
    _putenv_s("DATA_ROOT", ownedRoot.dataRoot().string().c_str());
#else
    setenv("DATA_ROOT", ownedRoot.dataRoot().c_str(), 1);
#endif
    DatabaseEngine::configureStorageRoot(ownedRoot.dataRoot().string());
    LSM::init(ownedRoot.dataRoot().string());
    auto& raft = RaftCore::instance();
    raft.init({}, "snapshot-metadata-test", 0, false);
    if (raft.isRecoveryComplete()) {
        std::cerr << "Raft declared recovery complete after snapshot refusal\n";
        return 5;
    }

    std::cout << "RAFT_SNAPSHOT_METADATA_BOUNDED_PASS\n";
    return 0;
}
