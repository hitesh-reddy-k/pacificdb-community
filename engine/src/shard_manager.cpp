#include "shard_manager.hpp"
#include "storage_path.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <future>
#include <thread>
#include <limits>
#include <cstdlib>
#include <cstdint>
#include <string_view>

namespace {
constexpr std::uint64_t stableShardHash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static_assert(stableShardHash("hello") == 0xa430d84680aabd0bULL);
}


json ShardInfo::toJson() const {
    return {{"shard_id", shardId}, {"start_key", startKey}, {"end_key", endKey},
            {"primary", primaryHost}, {"replicas", replicas},
            {"status", static_cast<int>(status)}, {"doc_count", documentCount},
            {"size_bytes", sizeBytes}, {"load_factor", loadFactor},
            {"avg_latency_ms", avgLatencyMs}, {"total_operations", totalOperations},
            {"total_errors", totalErrors}, {"is_healthy", isHealthy}};
}

ShardInfo ShardInfo::fromJson(const json& value) {
    ShardInfo info;
    info.shardId = value.value("shard_id", "");
    info.startKey = value.value("start_key", "");
    info.endKey = value.value("end_key", "");
    info.primaryHost = value.value("primary", "");
    info.replicas = value.value("replicas", std::vector<std::string>{});
    info.status = static_cast<Status>(value.value("status", 0));
    info.documentCount = value.value("doc_count", size_t{0});
    info.sizeBytes = value.value("size_bytes", size_t{0});
    info.loadFactor = value.value("load_factor", 0.0);
    return info;
}

// ============================================================================
// ShardManager Implementation
// ============================================================================

ShardManager& ShardManager::instance() {
    static ShardManager instance;
    return instance;
}

void ShardManager::init(const std::string& configPath) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    const char* dataRootEnv = std::getenv("DATA_ROOT");
    if (!dataRootEnv || !*dataRootEnv) {
        throw std::runtime_error(
            "ShardManager requires an explicit absolute DATA_ROOT");
    }
    const std::filesystem::path dataRoot(dataRootEnv);
    if (!dataRoot.is_absolute()) {
        throw std::runtime_error(
            "ShardManager refuses cwd-relative DATA_ROOT");
    }
    const std::filesystem::path requestedMap = configPath.empty()
        ? dataRoot / "shard_map.json"
        : std::filesystem::path(configPath);
    if (!requestedMap.is_absolute()) {
        throw std::runtime_error(
            "ShardManager config path must be absolute");
    }
    shardMapPath_ = validateContainedStoragePath(
        dataRoot, requestedMap).string();

    // Dynamic configuration via environment (optional overrides)
    if (const char* envBytes = std::getenv("SHARD_SPLIT_SIZE_BYTES")) {
        try { shardSizeThreshold_ = std::stoull(envBytes); } catch (...) {}
    }
    if (const char* envDocs = std::getenv("SHARD_SPLIT_DOCS")) {
        try { shardDocCountThreshold_ = std::stoull(envDocs); } catch (...) {}
    }
    if (const char* envLoad = std::getenv("SHARD_SPLIT_LOAD")) {
        try { shardLoadSplitThreshold_ = std::stod(envLoad); } catch (...) {}
    }
    if (const char* envLat = std::getenv("SHARD_QUERY_LATENCY_MS")) {
        try { shardQueryLatencyThresholdMs_ = std::stod(envLat); } catch (...) {}
    }
    if (const char* envRps = std::getenv("SHARD_QUERY_RPS")) {
        try { shardQueryRpsThreshold_ = std::stod(envRps); } catch (...) {}
    }

    if (std::filesystem::exists(shardMapPath_)) {
        loadShardMap(shardMapPath_);
    }

     // If no shards configured, bootstrap from environment or fallback defaults
    if (shardMap_.empty()) {
        int shardCount = 1;
        if (const char* envShardCount = std::getenv("SHARD_COUNT")) {
            try { shardCount = std::max(1, std::stoi(envShardCount)); } catch (...) { shardCount = 1; }
        }

        int replicaCount = 3; // 1 leader + 2 replicas
        if (const char* envReplicaCount = std::getenv("SHARD_REPLICA_COUNT")) {
            try { replicaCount = std::max(1, std::stoi(envReplicaCount)); } catch (...) { replicaCount = 3; }
        }

        std::vector<std::string> engineHosts;
        if (const char* envHosts = std::getenv("SHARD_NODE_HOSTS")) {
            std::stringstream ss(envHosts);
            std::string host;
            while (std::getline(ss, host, ',')) {
                if (!host.empty()) engineHosts.push_back(host);
            }
        }
        if (engineHosts.empty()) {
            std::string host = "127.0.0.1";
            int port = 9000;
            if (const char* envHost = std::getenv("ENGINE_HOST")) {
                std::string h(envHost);
                if (!h.empty() && h != "0.0.0.0") host = h;
            }
            if (const char* envPort = std::getenv("ENGINE_PORT")) {
                try { port = std::stoi(envPort); } catch (...) { port = 9000; }
            }
            engineHosts.push_back(host + ":" + std::to_string(port));
        }

        const std::string hexBounds = "0123456789abcdef";
        for (int i = 0; i < shardCount; ++i) {
            ShardInfo shard;
            shard.shardId = "shard-" + std::to_string(i);
            shard.startKey = (i == 0) ? "" : std::string(1, hexBounds[(i * 16) / shardCount]);
            shard.endKey = (i == shardCount - 1) ? "" : std::string(1, hexBounds[((i + 1) * 16) / shardCount]);
            shard.primaryHost = engineHosts[static_cast<size_t>(i) % engineHosts.size()];
            shard.status = ShardInfo::Status::ACTIVE;

            for (int r = 1; r < replicaCount && r < static_cast<int>(engineHosts.size()); ++r) {
                size_t idx = (static_cast<size_t>(i) + static_cast<size_t>(r)) % engineHosts.size();
                if (engineHosts[idx] != shard.primaryHost) {
                    shard.replicas.push_back(engineHosts[idx]);
                }
            }

            shardMap_[shard.startKey] = shard;
        }

        std::cout << "[ShardManager] Initialized " << shardMap_.size()
                  << " shard(s), replica target=" << replicaCount
                  << ", nodes=" << engineHosts.size() << std::endl;
    }
}

void ShardManager::start() {
    if (running_.exchange(true)) return;

    monitorThread_ = std::thread(&ShardManager::monitorLoop, this);
    std::cout << "[ShardManager] Started with " << shardMap_.size() << " shards" << std::endl;
}

void ShardManager::stop() {
    running_ = false;
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
    std::cout << "[ShardManager] Stopped" << std::endl;
}

std::string ShardManager::getShardForKey(const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    // Find the shard whose range contains this key
    for (const auto& [startKey, info] : shardMap_) {
        if (info.containsKey(key)) {
            return info.shardId;
        }
    }

    // Default to first shard if no match
    if (!shardMap_.empty()) {
        return shardMap_.begin()->second.shardId;
    }

    return "";
}

std::vector<std::string> ShardManager::getShardsForRange(const std::string& startKey,
                                                          const std::string& endKey) const {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    std::vector<std::string> result;

    for (const auto& [key, info] : shardMap_) {
        // Check if shard overlaps with query range
        bool overlaps = (info.endKey.empty() || info.endKey > startKey) &&
                        (endKey.empty() || info.startKey < endKey);

        if (overlaps) {
            result.push_back(info.shardId);
        }
    }

    return result;
}

std::vector<std::string> ShardManager::getAllShards() const {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    std::vector<std::string> result;

    for (const auto& [key, info] : shardMap_) {
        result.push_back(info.shardId);
    }

    return result;
}

bool ShardManager::createShard(const ShardInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    // Check for overlapping ranges
    for (const auto& [key, existing] : shardMap_) {
        bool overlaps = (existing.endKey.empty() || existing.endKey > info.startKey) &&
                        (info.endKey.empty() || existing.startKey < info.endKey);
        if (overlaps) {
            std::cerr << "[ShardManager] Cannot create shard: range overlaps with "
                      << existing.shardId << std::endl;
            return false;
        }
    }

    shardMap_[info.startKey] = info;
    persistShardMap();

    std::cout << "[ShardManager] Created shard " << info.shardId
              << " [" << info.startKey << " - " << info.endKey << "]" << std::endl;
    return true;
}

bool ShardManager::removeShard(const std::string& shardId) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    for (auto it = shardMap_.begin(); it != shardMap_.end(); ++it) {
        if (it->second.shardId == shardId) {
            shardMap_.erase(it);
            persistShardMap();
            std::cout << "[ShardManager] Removed shard " << shardId << std::endl;
            return true;
        }
    }

    return false;
}

bool ShardManager::updateShard(const ShardInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    for (auto& [key, existing] : shardMap_) {
        if (existing.shardId == info.shardId) {
            existing = info;
            persistShardMap();
            return true;
        }
    }

    return false;
}

ShardInfo ShardManager::getShardInfo(const std::string& shardId) const {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    for (const auto& [key, info] : shardMap_) {
        if (info.shardId == shardId) {
            return info;
        }
    }

    return ShardInfo{};
}

std::vector<ShardInfo> ShardManager::getAllShardsInfo() const {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    std::vector<ShardInfo> result;

    for (const auto& [key, info] : shardMap_) {
        result.push_back(info);
    }

    return result;
}

bool ShardManager::splitShard(const std::string& shardId, const std::string& splitKey) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    ShardInfo* original = nullptr;
    std::string originalKey;

    for (auto& [key, info] : shardMap_) {
        if (info.shardId == shardId) {
            original = &info;
            originalKey = key;
            break;
        }
    }

    if (!original) {
        std::cerr << "[ShardManager] Shard not found: " << shardId << std::endl;
        return false;
    }

    if (!original->containsKey(splitKey)) {
        std::cerr << "[ShardManager] Split key not in shard range" << std::endl;
        return false;
    }

    // Create new shard for upper half
    ShardInfo newShard;
    newShard.shardId = shardId + "-split";
    newShard.startKey = splitKey;
    newShard.endKey = original->endKey;
    newShard.primaryHost = findBestNodeForShard();
    newShard.status = ShardInfo::Status::ACTIVE;

    // Update original shard to cover lower half
    original->endKey = splitKey;
    original->status = ShardInfo::Status::ACTIVE;

    // Add new shard
    shardMap_[splitKey] = newShard;

    persistShardMap();

    std::cout << "[ShardManager] Split shard " << shardId << " at key " << splitKey << std::endl;
    return true;
}

bool ShardManager::mergeShard(const std::string& shardId1, const std::string& shardId2) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);

    ShardInfo *shard1 = nullptr, *shard2 = nullptr;
    std::string key1, key2;

    for (auto& [key, info] : shardMap_) {
        if (info.shardId == shardId1) { shard1 = &info; key1 = key; }
        if (info.shardId == shardId2) { shard2 = &info; key2 = key; }
    }

    if (!shard1 || !shard2) {
        std::cerr << "[ShardManager] One or both shards not found" << std::endl;
        return false;
    }

    // Ensure shards are adjacent
    if (shard1->endKey != shard2->startKey && shard2->endKey != shard1->startKey) {
        std::cerr << "[ShardManager] Shards are not adjacent" << std::endl;
        return false;
    }

    // Merge: extend shard1 to cover shard2's range
    if (shard1->endKey == shard2->startKey) {
        shard1->endKey = shard2->endKey;
    } else {
        shard1->startKey = shard2->startKey;
    }

    // Remove shard2
    shardMap_.erase(key2);

    persistShardMap();

    std::cout << "[ShardManager] Merged shards " << shardId1 << " and " << shardId2 << std::endl;
    return true;
}

bool ShardManager::rebalanceShards() {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    std::lock_guard<std::recursive_mutex> nodeLock(nodeMutex_);

    if (nodes_.empty() || shardMap_.empty()) return false;

    // Calculate target shards per node
    size_t totalShards = shardMap_.size();
    size_t activeNodes = 0;
    for (const auto& [id, node] : nodes_) {
        if (node.isAlive) activeNodes++;
    }

    if (activeNodes == 0) return false;

    size_t targetPerNode = (totalShards + activeNodes - 1) / activeNodes;

    std::cout << "[ShardManager] Rebalancing: " << totalShards
              << " shards across " << activeNodes << " nodes (target: "
              << targetPerNode << " per node)" << std::endl;

    // Simple rebalancing: redistribute shards evenly
    std::vector<std::string> nodeIds;
    for (const auto& [id, node] : nodes_) {
        if (node.isAlive) nodeIds.push_back(id);
    }

    size_t nodeIdx = 0;
    for (auto& [key, shard] : shardMap_) {
        if (nodeIdx < nodeIds.size()) {
            const auto& node = nodes_[nodeIds[nodeIdx]];
            shard.primaryHost = node.host + ":" + std::to_string(node.port);
            nodeIdx = (nodeIdx + 1) % nodeIds.size();
        }
    }

    persistShardMap();
    return true;
}

bool ShardManager::migrateShard(const std::string& shardId, const std::string& targetNode) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    std::lock_guard<std::recursive_mutex> nodeLock(nodeMutex_);

    auto nodeIt = nodes_.find(targetNode);
    if (nodeIt == nodes_.end() || !nodeIt->second.isAlive) {
        std::cerr << "[ShardManager] Target node not found or offline: " << targetNode << std::endl;
        return false;
    }

    for (auto& [key, info] : shardMap_) {
        if (info.shardId == shardId) {
            info.status = ShardInfo::Status::MIGRATING;
            info.primaryHost = nodeIt->second.host + ":" + std::to_string(nodeIt->second.port);
            info.status = ShardInfo::Status::ACTIVE;

            persistShardMap();
            std::cout << "[ShardManager] Migrated shard " << shardId
                      << " to " << targetNode << std::endl;
            return true;
        }
    }

    return false;
}

void ShardManager::registerNode(const ClusterNode& node) {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);
    nodes_[node.nodeId] = node;
    nodes_[node.nodeId].lastHeartbeat = std::chrono::steady_clock::now();
    std::cout << "[ShardManager] Registered node " << node.nodeId
              << " at " << node.host << ":" << node.port << std::endl;
}

void ShardManager::unregisterNode(const std::string& nodeId) {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);
    nodes_.erase(nodeId);
    std::cout << "[ShardManager] Unregistered node " << nodeId << std::endl;
}

void ShardManager::updateNodeHealth(const std::string& nodeId, double cpu, double memory,
                                    double disk, double network) {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);
    auto it = nodes_.find(nodeId);
    if (it != nodes_.end()) {
        it->second.cpuUsage = cpu;
        it->second.memoryUsage = memory;
        it->second.diskUsage = disk;
        it->second.networkUsage = network;
        it->second.lastHeartbeat = std::chrono::steady_clock::now();
        it->second.isAlive = true;
    }
}

std::vector<ClusterNode> ShardManager::getNodes() const {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);
    std::vector<ClusterNode> result;
    for (const auto& [id, node] : nodes_) {
        result.push_back(node);
    }
    return result;
}

ClusterNode ShardManager::getNode(const std::string& nodeId) const {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);
    auto it = nodes_.find(nodeId);
    if (it != nodes_.end()) {
        return it->second;
    }
    return ClusterNode{};
}

json ShardManager::getClusterStatus() const {
    std::lock_guard<std::recursive_mutex> shardLock(shardMutex_);
    std::lock_guard<std::recursive_mutex> nodeLock(nodeMutex_);

    json status;
    status["total_shards"] = shardMap_.size();
    status["active_nodes"] = getActiveNodes();
    status["healthy"] = isHealthy();

    json shards = json::array();
    for (const auto& [key, info] : shardMap_) {
        shards.push_back(info.toJson());
    }
    status["shards"] = shards;

    json nodeList = json::array();
    for (const auto& [id, node] : nodes_) {
        nodeList.push_back({
            {"node_id", node.nodeId},
            {"host", node.host},
            {"port", node.port},
            {"alive", node.isAlive},
            {"cpu", node.cpuUsage},
            {"memory", node.memoryUsage}
        });
    }
    status["nodes"] = nodeList;

    return status;
}

bool ShardManager::isHealthy() const {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);

    size_t aliveNodes = 0;
    for (const auto& [id, node] : nodes_) {
        if (node.isAlive) aliveNodes++;
    }

    // Healthy if at least one node is alive and we have shards
    return aliveNodes > 0 && !shardMap_.empty();
}

size_t ShardManager::getActiveNodes() const {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);
    size_t count = 0;
    for (const auto& [id, node] : nodes_) {
        if (node.isAlive) count++;
    }
    return count;
}

void ShardManager::monitorLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::lock_guard<std::recursive_mutex> lock(nodeMutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto& [id, node] : nodes_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - node.lastHeartbeat).count();
            if (elapsed > 30) node.isAlive = false;
        }
    }
}

std::string ShardManager::findBestNodeForShard() const {
    std::lock_guard<std::recursive_mutex> lock(nodeMutex_);

    std::string bestNode;
    double minLoad = std::numeric_limits<double>::max();

    for (const auto& [id, node] : nodes_) {
        if (node.isAlive) {
            double load = node.cpuUsage * 0.5 + node.memoryUsage * 0.5;
            if (load < minLoad) {
                minLoad = load;
                bestNode = node.host + ":" + std::to_string(node.port);
            }
        }
    }

    return bestNode.empty() ? "127.0.0.1:9000" : bestNode;
}

std::string ShardManager::suggestSplitKey(const std::string& shardId) const {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    for (const auto& [key, info] : shardMap_) {
        if (info.shardId == shardId) {
            return computeSplitKey(info);
        }
    }
    return "";
}

void ShardManager::updateShardQueryLoad(const std::string& shardId, double latencyMs) {
    if (shardId.empty()) return;

    // Update EWMA stats
    QueryStats stats;
    {
        std::lock_guard<std::mutex> lock(shardQueryMutex_);
        stats = shardQueryStats_[shardId];
        auto now = std::chrono::steady_clock::now();
        if (stats.hasLast) {
            auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - stats.lastQuery).count();
            if (deltaMs > 0) {
                double instantRps = 1000.0 / static_cast<double>(deltaMs);
                double alphaRps = 0.2;
                stats.rpsEwma = (stats.rpsEwma == 0.0)
                    ? instantRps
                    : (alphaRps * instantRps + (1.0 - alphaRps) * stats.rpsEwma);
            }
        }
        double alphaLat = 0.2;
        stats.latencyEwmaMs = (stats.latencyEwmaMs == 0.0)
            ? latencyMs
            : (alphaLat * latencyMs + (1.0 - alphaLat) * stats.latencyEwmaMs);

        stats.lastQuery = now;
        stats.hasLast = true;
        shardQueryStats_[shardId] = stats;
    }

    // Compute normalized load ratios
    double latencyRatio = 0.0;
    double rpsRatio = 0.0;
    if (shardQueryLatencyThresholdMs_ > 0.0) {
        latencyRatio = stats.latencyEwmaMs / shardQueryLatencyThresholdMs_;
    }
    if (shardQueryRpsThreshold_ > 0.0) {
        rpsRatio = stats.rpsEwma / shardQueryRpsThreshold_;
    }

    double queryLoad = std::max(latencyRatio, rpsRatio);
    if (queryLoad > 1.0) queryLoad = 1.0;

    // Merge with shard metrics
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    for (auto& [key, info] : shardMap_) {
        if (info.shardId == shardId) {
            // Decay previous loadFactor slightly, then apply query load
            info.loadFactor = std::max(queryLoad, info.loadFactor * 0.9);
            break;
        }
    }
}

std::string ShardManager::computeSplitKey(const ShardInfo& info) const {
    // Strategy:
    // 1) If both bounds exist and differ, pick a midpoint by first differing char.
    // 2) Otherwise, try a safe suffix on startKey.
    // 3) Otherwise, derive from endKey by decrementing last char.
    // 4) Fallback to "m" for unbounded ranges.

    const std::string& startKey = info.startKey;
    const std::string& endKey = info.endKey;

    // Attempt midpoint by first differing char
    if (!startKey.empty() && !endKey.empty()) {
        size_t i = 0;
        while (i < startKey.size() && i < endKey.size() && startKey[i] == endKey[i]) {
            i++;
        }
        if (i < startKey.size() && i < endKey.size()) {
            unsigned char a = static_cast<unsigned char>(startKey[i]);
            unsigned char b = static_cast<unsigned char>(endKey[i]);
            if (a + 1 < b) {
                unsigned char mid = static_cast<unsigned char>((a + b) / 2);
                std::string candidate = startKey.substr(0, i);
                candidate.push_back(static_cast<char>(mid));
                return candidate;
            }
        }
    }

    // Try a safe suffix on startKey
    if (!startKey.empty()) {
        std::string candidate = startKey;
        candidate.push_back(static_cast<char>(0x7f));
        if (info.containsKey(candidate)) {
            return candidate;
        }
    }

    // Try deriving from endKey by decrementing last char
    if (!endKey.empty()) {
        std::string candidate = endKey;
        for (int i = static_cast<int>(candidate.size()) - 1; i >= 0; --i) {
            unsigned char c = static_cast<unsigned char>(candidate[i]);
            if (c > 0) {
                candidate[i] = static_cast<char>(c - 1);
                candidate.resize(static_cast<size_t>(i + 1));
                if (info.containsKey(candidate) && candidate != startKey) {
                    return candidate;
                }
                break;
            }
        }
    }

    // Fallback for unbounded ranges
    if (info.containsKey("m")) {
        return "m";
    }
    if (info.containsKey("M")) {
        return "M";
    }

    return "";
}

void ShardManager::persistShardMap() {
    // Save shard map to disk
    json mapJson;
    mapJson["shards"] = json::array();

    for (const auto& [key, info] : shardMap_) {
        mapJson["shards"].push_back(info.toJson());
    }

    try {
        std::ofstream file(shardMapPath_);
        if (!file) {
            throw std::runtime_error("cannot open contained shard map");
        }
        file << mapJson.dump(2);
        file.flush();
        if (!file) {
            throw std::runtime_error("cannot persist contained shard map");
        }
        file.close();
    } catch (const std::exception& error) {
        std::cerr << "[ShardManager] Failed to persist shard map: "
                  << error.what() << std::endl;
    }
}

void ShardManager::loadShardMap(const std::string& configPath) {
    try {
        std::ifstream file(configPath);
        if (!file.is_open()) return;

        json mapJson;
        file >> mapJson;

        for (const auto& shardJson : mapJson["shards"]) {
            ShardInfo info = ShardInfo::fromJson(shardJson);
            shardMap_[info.startKey] = info;
        }

        std::cout << "[ShardManager] Loaded " << shardMap_.size()
                  << " shards from " << configPath << std::endl;
    } catch (...) {
        std::cerr << "[ShardManager] Failed to load shard map" << std::endl;
    }
}


size_t ShardManager::getTotalShards() const {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    return shardMap_.size();
}

void ShardManager::recordOperation(const std::string& shardId,
                                   const std::string& opType,
                                   double latencyMs, size_t bytes, bool success) {
    std::lock_guard<std::recursive_mutex> lock(shardMutex_);
    for (auto& [key, shard] : shardMap_) {
        if (shard.shardId != shardId) continue;
        ++shard.totalOperations;
        if (opType == "read") ++shard.totalReads; else ++shard.totalWrites;
        if (!success) ++shard.totalErrors;
        shard.avgLatencyMs += (latencyMs - shard.avgLatencyMs) /
                              static_cast<double>(shard.totalOperations);
        shard.sizeBytes += bytes;
        shard.lastActivity = std::chrono::steady_clock::now();
        break;
    }
}
