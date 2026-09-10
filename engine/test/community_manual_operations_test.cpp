#include "backup_manager.hpp"
#include "community_catalog.hpp"
#include "database_engine.hpp"
#include "shard_manager.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "pacificdb-community-manual-test";
    fs::remove_all(root);
    fs::create_directories(root / "data");
    fs::create_directories(root / "backups");
    fs::create_directories(root / "restores");
    std::ofstream(root / "data" / "record.txt") << "durable-record";

#ifdef _WIN32
    _putenv_s("DATA_ROOT", (root / "data").string().c_str());
#else
    setenv("DATA_ROOT", (root / "data").string().c_str(), 1);
#endif

    DatabaseEngine::init((root / "data").string(), false);

    auto& backups = BackupManager::instance();
    backups.init((root / "data").string(), (root / "backups").string(),
                 (root / "restores").string());
    const std::string id = backups.createFullBackup("community test", 1);
    if (id.empty() || !backups.verifyBackup(id)) return 1;
    const auto exported = backups.exportBackupManifest(id);
    bool foundRecord = false;
    for (const auto& file : exported.at("files"))
        foundRecord = foundRecord || file.at("path") == "data/record.txt";
    const auto firstChunk = backups.readBackupFileChunk(id, "data/record.txt", 0, 4);
    const auto rest = backups.readBackupFileChunk(
        id, "data/record.txt", firstChunk.size(), 64);
    std::string exportedRecord(firstChunk.begin(), firstChunk.end());
    exportedRecord.append(rest.begin(), rest.end());
    if (exported.value("format", "") != "pacificdb-full-backup-v1" ||
        !foundRecord || exportedRecord != "durable-record") return 7;
    try {
        backups.readBackupFileChunk(id, "../record.txt", 0, 4);
        return 8;
    } catch (const std::invalid_argument&) {}
    const fs::path restored = root / "restores" / "restored";
    if (!backups.restoreFromBackup(id, restored.string()) ||
        !fs::exists(restored / "record.txt")) return 2;
    auto& catalog = pacificdb::community::CommunityCatalog::instance();
    const auto restore = catalog.recordRestore("system", id, restored.string(), true, "");
    if (restore.value("backup_id", "") != id ||
        catalog.listRestores("system").empty()) return 6;

    auto& shards = ShardManager::instance();
    shards.init();
    ClusterNode first{"node-1", "127.0.0.1", 9000};
    ClusterNode second{"node-2", "127.0.0.1", 9010};
    shards.registerNode(first);
    shards.registerNode(second);
    const auto initial = shards.getAllShardsInfo();
    if (initial.empty() || !shards.splitShard(initial.front().shardId, "m")) return 3;
    const auto split = shards.getAllShardsInfo();
    if (split.size() != 2 || !shards.migrateShard(split.back().shardId, "node-2"))
        return 4;
    if (!shards.rebalanceShards()) return 5;

    fs::remove_all(root);
    std::cout << "manual backup/restore and sharding passed\n";
    return 0;
}
