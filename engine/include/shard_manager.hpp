#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct ShardInfo {
    enum class Status { ACTIVE, MIGRATING, SPLITTING, DRAINING, OFFLINE, INITIALIZING };
    std::string shardId;
    std::string startKey;
    std::string endKey;
    std::string primaryHost;
    std::vector<std::string> replicas;
    Status status{Status::ACTIVE};
    size_t documentCount{0};
    size_t sizeBytes{0};
    double loadFactor{0};
    double weight{1};
    double avgLatencyMs{0};
    double p95LatencyMs{0};
    double p99LatencyMs{0};
    size_t totalOperations{0};
    size_t totalReads{0};
    size_t totalWrites{0};
    size_t totalErrors{0};
    double opsPerSecond{0};
    std::chrono::steady_clock::time_point createdAt{};
    std::chrono::steady_clock::time_point lastActivity{};
    bool isHealthy{true};

    bool containsKey(const std::string& key) const {
        return key >= startKey && (endKey.empty() || key < endKey);
    }
    json toJson() const;
    static ShardInfo fromJson(const json& value);
};

struct ClusterNode {
    std::string nodeId;
    std::string host;
    int port{9000};
    bool isAlive{true};
    std::chrono::steady_clock::time_point lastHeartbeat{};
    std::vector<std::string> hostedShards;
    double cpuUsage{0};
    double memoryUsage{0};
    double diskUsage{0};
    double networkUsage{0};
};

class ShardManager {
public:
    static ShardManager& instance();
    void init(const std::string& configPath = "");
    void start();
    void stop();
    std::string getShardForKey(const std::string& key) const;
    std::vector<std::string> getShardsForRange(const std::string&, const std::string&) const;
    std::vector<std::string> getAllShards() const;
    bool createShard(const ShardInfo&);
    bool removeShard(const std::string&);
    bool updateShard(const ShardInfo&);
    ShardInfo getShardInfo(const std::string&) const;
    std::vector<ShardInfo> getAllShardsInfo() const;
    bool splitShard(const std::string&, const std::string&);
    bool mergeShard(const std::string&, const std::string&);
    bool rebalanceShards();
    bool migrateShard(const std::string&, const std::string&);
    void registerNode(const ClusterNode&);
    void unregisterNode(const std::string&);
    void updateNodeHealth(const std::string&, double, double, double = 0, double = 0);
    std::vector<ClusterNode> getNodes() const;
    ClusterNode getNode(const std::string&) const;
    json getClusterStatus() const;
    bool isHealthy() const;
    size_t getTotalShards() const;
    size_t getActiveNodes() const;
    void recordOperation(const std::string&, const std::string&, double, size_t = 0, bool = true);
    void updateShardQueryLoad(const std::string&, double);
    std::string suggestSplitKey(const std::string&) const;
    size_t getShardSizeThreshold() const { return shardSizeThreshold_; }
    size_t getShardDocCountThreshold() const { return shardDocCountThreshold_; }
    double getShardLoadSplitThreshold() const { return shardLoadSplitThreshold_; }
    double getShardQueryLatencyThresholdMs() const { return shardQueryLatencyThresholdMs_; }
    double getShardQueryRpsThreshold() const { return shardQueryRpsThreshold_; }

private:
    ShardManager() = default;
    std::map<std::string, ShardInfo> shardMap_;
    mutable std::recursive_mutex shardMutex_;
    std::map<std::string, ClusterNode> nodes_;
    mutable std::recursive_mutex nodeMutex_;
    std::atomic<bool> running_{false};
    std::thread monitorThread_;
    std::string shardMapPath_;
    size_t shardSizeThreshold_{20 * 1024 * 1024};
    size_t shardDocCountThreshold_{200};
    double loadBalanceThreshold_{0.15};
    double shardLoadSplitThreshold_{0.40};
    double shardQueryLatencyThresholdMs_{50};
    double shardQueryRpsThreshold_{100};
    struct QueryStats {
        double latencyEwmaMs{0};
        double rpsEwma{0};
        bool hasLast{false};
        std::chrono::steady_clock::time_point lastQuery{};
    };
    std::unordered_map<std::string, QueryStats> shardQueryStats_;
    mutable std::mutex shardQueryMutex_;
    void monitorLoop();
    std::string findBestNodeForShard() const;
    std::string computeSplitKey(const ShardInfo&) const;
    void persistShardMap();
    void loadShardMap(const std::string&);
};
