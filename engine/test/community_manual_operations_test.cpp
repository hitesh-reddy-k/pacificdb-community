#include "backup_manager.hpp"
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

    auto& backups = BackupManager::instance();
    backups.init((root / "data").string(), (root / "backups").string(),
                 (root / "restores").string());
    const std::string id = backups.createFullBackup("community test", 1);
    if (id.empty() || !backups.verifyBackup(id)) return 1;
    const fs::path restored = root / "restores" / "restored";
    if (!backups.restoreFromBackup(id, restored.string()) ||
        !fs::exists(restored / "record.txt")) return 2;

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
