#include "data_durability.hpp"
#include "owned_test_root.hpp"
#include "raft_core.hpp"
#include "lsm.hpp"
#include "snapshot_bundle.hpp"
#include "wal_integrity.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifdef __linux__
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

int main() {
#ifdef __linux__
    rlimit limit{};
    if (getrlimit(RLIMIT_AS, &limit) != 0) return 8;
    limit.rlim_cur = std::min(limit.rlim_cur, rlim_t{128U * 1024U * 1024U});
    if (setrlimit(RLIMIT_AS, &limit) != 0) return 8;
#endif
    const OwnedTestRoot root("snapshot-bundle");
    WALIntegrity::markCleanShutdown(root.dataRoot().string());
    if (!WALIntegrity::wasCleanShutdown(root.dataRoot().string()) ||
        !fs::exists(root.dataRoot() / WALIntegrity::CLEAN_SHUTDOWN_MARKER)) {
        std::cerr << "startup consumed the clean marker before migration\n";
        return 5;
    }
    WALIntegrity::clearCleanShutdownMarker(root.dataRoot().string());
    if (fs::exists(root.dataRoot() / WALIntegrity::CLEAN_SHUTDOWN_MARKER)) {
        std::cerr << "clean marker was not explicitly consumed\n";
        return 6;
    }
    const fs::path source = root.dataRoot() / "large.sst";
    const fs::path bundlePath = root.dataRoot() / ".snapshot";
    const fs::path restored = root.dataRoot() / "restored.sst";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output.seekp(256U * 1024U * 1024U - 1);
        output.put('x');
    }
    json manifest = {
        {"snapshot_format", "pdb-snapshot-bundle-v3"},
        {"snapshot_checksum_format", "bundle-v3"},
        {"version", 1},
        {"complete", true},
        {"commitIndex", 42},
        {"lastIncludedIndex", 42},
        {"lastIncludedTerm", 6},
        {"currentTerm", 7},
        {"lsm_payload", {
            {"version", 3},
            {"lastIncludedIndex", 42},
            {"files", json::array({{{"path", "user/db/db.meta"}}})},
        }},
    };
    pacificdb::snapshot_bundle::write(
        bundlePath, manifest, {{{source}}});
    const auto bundle = pacificdb::snapshot_bundle::read(bundlePath);
    pacificdb::snapshot_bundle::extract(bundlePath, bundle, 0, restored);
    if (bundle.manifest["lastIncludedIndex"] != 42 ||
        fs::file_size(restored) != fs::file_size(source) ||
        pacificdb::durability::ChecksumCalculator::sha256File(restored.string()) !=
            pacificdb::durability::ChecksumCalculator::sha256File(source.string())) {
        std::cerr << "snapshot bundle round trip failed\n";
        return 1;
    }
    std::ofstream(bundlePath.string() + ".sha256") <<
        pacificdb::durability::ChecksumCalculator::sha256File(bundlePath.string()) << "\n";
    uint64_t index = 0;
    uint64_t term = 0;
    std::string reason;
    if (!RaftCore::readPersistedSnapshotMetadata(
            bundlePath.string(), index, term, reason) || index != 42 || term != 6) {
        std::cerr << "bounded bundle metadata read failed: " << reason << "\n";
        return 3;
    }
    json alteredManifest = bundle.manifest;
    alteredManifest["currentTerm"] = 8;
    if (RaftCore::validatePersistedSnapshot(alteredManifest, reason)) {
        std::cerr << "bundle manifest without matching checksum was accepted\n";
        return 7;
    }

    const fs::path applyRoot = root.dataRoot() / "apply";
    fs::create_directories(applyRoot);
    LSM::init(applyRoot.string());
    if (!LSM::applySnapshot(bundle.manifest["lsm_payload"], bundlePath) ||
        fs::file_size(applyRoot / "user/db/db.meta") != fs::file_size(source)) {
        std::cerr << "bounded LSM bundle apply failed\n";
        return 4;
    }

    fs::resize_file(bundlePath, fs::file_size(bundlePath) - 1);
    try {
        (void)pacificdb::snapshot_bundle::read(bundlePath);
        std::cerr << "truncated snapshot bundle was accepted\n";
        return 2;
    } catch (const std::exception&) {}

    std::cout << "SNAPSHOT_BUNDLE_BOUNDED_PASS\n";
    return 0;
}
