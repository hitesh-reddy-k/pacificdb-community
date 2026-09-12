#include "database_engine.hpp"
#include "community_catalog.hpp"
#include "lsm.hpp"
#include "raft_core.hpp"
#include "wal.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kUser = "binary-user";
constexpr const char* kDatabase = "binary-db";
constexpr const char* kCollection = "docs";

struct RawRecord {
    std::uint64_t index;
    std::uint64_t term;
    std::string payload;
};

std::vector<RawRecord> readRecords(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<RawRecord> records;
    while (in.peek() != std::char_traits<char>::eof()) {
        RawRecord record{};
        std::uint32_t size = 0;
        if (!in.read(reinterpret_cast<char*>(&record.index), sizeof(record.index)) ||
            !in.read(reinterpret_cast<char*>(&record.term), sizeof(record.term)) ||
            !in.read(reinterpret_cast<char*>(&size), sizeof(size))) {
            throw std::runtime_error("truncated Raft test record header");
        }
        record.payload.resize(size);
        if (!in.read(record.payload.data(), static_cast<std::streamsize>(size))) {
            throw std::runtime_error("truncated Raft test record payload");
        }
        records.push_back(std::move(record));
    }
    return records;
}

void writeRecords(const fs::path& path, const std::vector<RawRecord>& records) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto& record : records) {
        const auto size = static_cast<std::uint32_t>(record.payload.size());
        out.write(reinterpret_cast<const char*>(&record.index), sizeof(record.index));
        out.write(reinterpret_cast<const char*>(&record.term), sizeof(record.term));
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write(record.payload.data(), static_cast<std::streamsize>(record.payload.size()));
    }
    if (!out) throw std::runtime_error("failed to rewrite Raft test records");
}

std::string encodeV2(const nlohmann::json& value) {
    const auto packed = nlohmann::json::to_msgpack(value);
    std::string payload = "PDBR2";
    payload.append(reinterpret_cast<const char*>(packed.data()), packed.size());
    return payload;
}

void configure(const fs::path& root) {
#ifndef _WIN32
    setenv("DATA_ROOT", root.c_str(), 1);
    setenv("DATA_DIR", root.c_str(), 1);
    setenv("RAFT_STANDALONE_BYPASS", "1", 1);
#endif
}

fs::path collectionWal(const fs::path& root) {
    return root / kUser / kDatabase / "wal" / "docs.wal";
}

void requireNoCollectionWal(const fs::path& root) {
    const fs::path dbWal = root / kUser / kDatabase / "wal" / "db.wal";
    if (fs::exists(dbWal) || fs::exists(collectionWal(root))) {
        throw std::runtime_error("replicated mutation was written to a duplicate collection WAL");
    }
}

void initialize(const fs::path& root, const std::string& nodeId) {
    configure(root);
    fs::create_directories(root / "raft");
    DatabaseEngine::init(root.string());
    LSM::init(root.string());
    WAL::init();
    auto& raft = RaftCore::instance();
    raft.init({}, nodeId, 0, true);
    raft.start();
    if (!raft.isLeaseValid()) {
        raft.stop();
        WAL::shutdown();
        throw std::runtime_error("single-node leader lease is not immediately valid");
    }
}

int writePhase(const fs::path& root) {
    initialize(root, "binary-log-writer");
    auto& raft = RaftCore::instance();
    const bool databaseApplied = raft.replicateAndApply({
        {"action", "createDatabase"},
        {"op", "CREATE_DB"},
        {"userId", kUser},
        {"db", kDatabase},
    });
    const bool collectionApplied = raft.replicateAndApply({
        {"action", "createCollection"},
        {"op", "CREATE_COLLECTION"},
        {"userId", kUser},
        {"db", kDatabase},
        {"collection", kCollection},
    });
    const bool firstInsertApplied = raft.replicateAndApply({
        {"action", "insert"},
        {"op", "INSERT"},
        {"userId", kUser},
        {"db", kDatabase},
        {"collection", kCollection},
        {"data", {{"id", "one"}, {"value", 1}, {"padding", std::string(2048, 'a')}}},
    });
    const bool updateApplied = raft.replicateAndApply({
        {"action", "update"},
        {"op", "UPDATE"},
        {"userId", kUser},
        {"db", kDatabase},
        {"collection", kCollection},
        {"data", {{"id", "one"}, {"value", 2}, {"padding", std::string(2048, 'a')}}},
    });
    const bool secondInsertApplied = raft.replicateAndApply({
        {"action", "insert"},
        {"op", "INSERT"},
        {"userId", kUser},
        {"db", kDatabase},
        {"collection", kCollection},
        {"data", {{"id", "two"}, {"value", 3}}},
    });
    const bool deleteApplied = raft.replicateAndApply({
        {"action", "delete"},
        {"op", "DELETE"},
        {"userId", kUser},
        {"db", kDatabase},
        {"collection", kCollection},
        {"data", {{"id", "two"}, {"_deleted", true}}},
    });
    if (!databaseApplied || !collectionApplied || !firstInsertApplied ||
        !updateApplied || !secondInsertApplied || !deleteApplied) {
        throw std::runtime_error("standalone Raft entries were not applied");
    }
    requireNoCollectionWal(root);

    const auto records = readRecords(root / "raft/log.bin");
    bool sawRaw = false;
    bool sawCompressed = false;
    for (const auto& record : records) {
        if (record.payload.size() < 14 || record.payload.rfind("PDBR3", 0) != 0) {
            throw std::runtime_error("Raft log entry is not a PDBR3 envelope");
        }
        sawRaw |= record.payload[5] == 0;
        sawCompressed |= record.payload[5] == 1;
    }
    if (records.size() != 6 || records.front().index != 1 || !sawRaw || !sawCompressed) {
        throw std::runtime_error("PDBR3 did not exercise raw and compressed records");
    }
    if (fs::exists(root / "raft/raft_log.jsonl")) {
        throw std::runtime_error("legacy Raft JSON log was written");
    }

    // Simulate a crash: the durable Raft log must be sufficient even though no
    // graceful LSM flush or applied-progress checkpoint runs.
    std::_Exit(0);
}

int recoverPhase(const fs::path& root) {
    initialize(root, "binary-log-recoverer");
    struct WalShutdownGuard {
        ~WalShutdownGuard() { WAL::shutdown(); }
    } walShutdownGuard;
    auto& raft = RaftCore::instance();
    const auto recoveredMetrics = raft.getWriteReplicationMetrics();
    if (recoveredMetrics.value("raftBatchWindowMs", -1) != 2) {
        throw std::runtime_error("Raft metrics do not expose the active batch window");
    }
    if (recoveredMetrics.value("raftLogOffsetIndexEntries", 0ULL) != 6) {
        throw std::runtime_error("restart did not rebuild the Raft log offset index");
    }
    const auto one = LSM::findByField(kUser, kDatabase, kCollection, "id", "one");
    const auto two = LSM::findByField(kUser, kDatabase, kCollection, "id", "two");
    if (one.size() != 1 || one.front().value("value", 0) != 2 || !two.empty()) {
        throw std::runtime_error("restart did not recover the final replicated state from Raft");
    }
    requireNoCollectionWal(root);

    if (!raft.replicateAndApply({{"type", "raft_noop"}})) {
        throw std::runtime_error("post-recovery Raft append failed");
    }
    const auto appendedMetrics = raft.getWriteReplicationMetrics();
    if (appendedMetrics.value("raftLogOffsetIndexEntries", 0ULL) != 7) {
        throw std::runtime_error("Raft append did not extend the log offset index");
    }

    LSM::put(kUser, kDatabase, kCollection,
             {{"id", "local"}, {"value", 4}});
    WAL::shutdown();
    size_t localRecords = 0;
    const auto localWal = WAL::scan(collectionWal(root), [&](const WalReplayRecord& record) {
        if (record.entry.value("op", std::string()) == "PUT" &&
            record.entry.contains("data") &&
            record.entry["data"].value("id", std::string()) == "local") {
            ++localRecords;
        }
        return true;
    });
    if (localWal.status != WalScanStatus::OK || localWal.records != 1 ||
        localRecords != 1) {
        throw std::runtime_error("non-replicated local mutation did not retain collection WAL durability");
    }
    raft.stop();
    return 0;
}

int writeProjectAndCrash(const fs::path& root) {
    initialize(root, "project-writer");
    auto& catalog = pacificdb::community::CommunityCatalog::instance();
    catalog.initialize("system");
    const auto project = catalog.createProject("system", "crash-durable-project");
    if (project.value("name", std::string()) != "crash-durable-project" ||
        catalog.getProject("system", project.value("id", std::string())).is_null()) {
        throw std::runtime_error("project write was not acknowledged and visible");
    }
    std::_Exit(0);
}

int recoverProject(const fs::path& root) {
    initialize(root, "project-recoverer");
    auto& catalog = pacificdb::community::CommunityCatalog::instance();
    catalog.initialize("system");
    const auto projects = catalog.listProjects("system");
    const bool found = std::any_of(projects.begin(), projects.end(), [](const auto& project) {
        return project.value("name", std::string()) == "crash-durable-project";
    });
    RaftCore::instance().stop();
    WAL::shutdown();
    if (!found) {
        throw std::runtime_error(
            "acknowledged project disappeared after a second-process crash");
    }
    return 0;
}

std::string quote(const fs::path& value) {
    return "\"" + value.string() + "\"";
}

void makeMixedVersionLog(const fs::path& root) {
    auto records = readRecords(root / "raft/log.bin");
    if (records.size() < 2) throw std::runtime_error("not enough records for compatibility test");
    records[0].payload = encodeV2({
        {"action", "createDatabase"}, {"op", "CREATE_DB"},
        {"userId", kUser}, {"db", kDatabase},
    });
    records[1].payload = nlohmann::json({
        {"action", "createCollection"}, {"op", "CREATE_COLLECTION"},
        {"userId", kUser}, {"db", kDatabase}, {"collection", kCollection},
    }).dump();
    writeRecords(root / "raft/log.bin", records);
}

void corruptPdbR3(const fs::path& root) {
    auto records = readRecords(root / "raft/log.bin");
    if (records.empty() || records.front().payload.size() < 15) {
        throw std::runtime_error("not enough PDBR3 data to corrupt");
    }
    records.front().payload.back() ^= 1;
    writeRecords(root / "raft/log.bin", records);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write") {
        try {
            return writePhase(fs::path(argv[2]));
        } catch (const std::exception& error) {
            std::cerr << "RAFT_BINARY_LOG_WRITE_FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::string(argv[1]) == "--recover") {
        try {
            return recoverPhase(fs::path(argv[2]));
        } catch (const std::exception& error) {
            std::cerr << "RAFT_BINARY_LOG_RECOVER_FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::string(argv[1]) == "--write-project-crash") {
        try {
            return writeProjectAndCrash(fs::path(argv[2]));
        } catch (const std::exception& error) {
            std::cerr << "RAFT_PROJECT_WRITE_FAIL: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 3 && std::string(argv[1]) == "--recover-project") {
        try {
            return recoverProject(fs::path(argv[2]));
        } catch (const std::exception& error) {
            std::cerr << "RAFT_PROJECT_RECOVER_FAIL: " << error.what() << '\n';
            return 1;
        }
    }

    const fs::path root = fs::temp_directory_path() /
        ("pacificdb-raft-binary-log-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path corruptRoot = root.string() + "-corrupt";
    try {
        const fs::path executable = fs::canonical(argv[0]);
        fs::create_directories(root);
        const std::string writeCommand = quote(executable) + " --write " + quote(root);
        if (std::system(writeCommand.c_str()) != 0) {
            throw std::runtime_error("writer subprocess failed");
        }
        makeMixedVersionLog(root);
        const std::string recoverCommand = quote(executable) + " --recover " + quote(root);
        if (std::system(recoverCommand.c_str()) != 0) {
            throw std::runtime_error("recovery subprocess failed");
        }
        const std::string projectWriteCommand = quote(executable) +
            " --write-project-crash " + quote(root);
        if (std::system(projectWriteCommand.c_str()) != 0) {
            throw std::runtime_error("project writer subprocess failed");
        }
        const std::string projectRecoverCommand = quote(executable) +
            " --recover-project " + quote(root);
        if (std::system(projectRecoverCommand.c_str()) != 0) {
            throw std::runtime_error("acknowledged project crash recovery failed");
        }

        fs::create_directories(corruptRoot);
        const std::string corruptWriteCommand = quote(executable) + " --write " + quote(corruptRoot);
        if (std::system(corruptWriteCommand.c_str()) != 0) {
            throw std::runtime_error("corruption writer subprocess failed");
        }
        const auto validRecords = readRecords(corruptRoot / "raft/log.bin");
        corruptPdbR3(corruptRoot);
        const std::string corruptRecoverCommand = quote(executable) + " --recover " + quote(corruptRoot);
        if (std::system(corruptRecoverCommand.c_str()) == 0) {
            throw std::runtime_error("corrupt PDBR3 record was accepted");
        }
        auto unknownCodecRecords = validRecords;
        unknownCodecRecords.front().payload[5] = 127;
        writeRecords(corruptRoot / "raft/log.bin", unknownCodecRecords);
        if (std::system(corruptRecoverCommand.c_str()) == 0) {
            throw std::runtime_error("unknown PDBR3 codec was accepted");
        }
        writeRecords(corruptRoot / "raft/log.bin", validRecords);
        fs::resize_file(corruptRoot / "raft/log.bin",
                        fs::file_size(corruptRoot / "raft/log.bin") - 1);
        if (std::system(corruptRecoverCommand.c_str()) == 0) {
            throw std::runtime_error("truncated PDBR3 log was accepted");
        }
        fs::remove_all(root);
        fs::remove_all(corruptRoot);
        std::cout << "RAFT_BINARY_LOG_V3_PASS\n";
        return 0;
    } catch (const std::exception& error) {
        fs::remove_all(root);
        fs::remove_all(corruptRoot);
        std::cerr << "RAFT_BINARY_LOG_V3_FAIL: " << error.what() << '\n';
        return 1;
    }
}
