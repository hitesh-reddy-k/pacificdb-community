#pragma once

#include "db_task_queue_sharded.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>

/**
 * DBTaskQueuePartitioned: partitioned task queue with:
 * - Adaptive shard distribution
 * - Per-shard concurrency control
 * - Distributed load balancing
 * - Comprehensive metrics instrumentation
 * - Jitter-based backpressure
 * - Work stealing for load balancing
 *
 */
class DBTaskQueuePartitioned {
public:
    using Task = std::function<void()>;
    enum class Priority : uint8_t {
        CRITICAL = 0,
        HIGH = 1,
        MEDIUM = 2,
        LOW = 3
    };

    struct ShardMetrics {
        uint64_t tasksProcessed{0};
        uint64_t tasksRejected{0};
        uint64_t totalWaitUs{0};
        uint64_t totalExecUs{0};
        uint64_t lockContentionUs{0};
        double avgP99LatencyMs{0.0};
        size_t currentDepth{0};
        double utilizationPct{0.0};
    };

    struct SystemMetrics {
        uint64_t globalTasksProcessed{0};
        uint64_t globalTasksRejected{0};
        double globalThroughputRps{0.0};
        double globalP99LatencyMs{0.0};
        double globalP95LatencyMs{0.0};
        double globalP50LatencyMs{0.0};
        uint64_t totalQueueDepth{0};
        std::vector<ShardMetrics> shardMetrics;
    };

    DBTaskQueuePartitioned(
        size_t shards = 32,                           // More shards for better distribution
        size_t workersPerShard = 4,
        size_t maxQueuePerShard = 5000,
        bool enableWorkStealing = true,
        bool enableAdaptiveBalancing = true
    );

    ~DBTaskQueuePartitioned();

    // Core interface
    bool enqueue(
        Task task,
        Priority priority = Priority::MEDIUM,
        const std::string& collection = "",
        size_t costHint = 1,
        const std::string& shardHint = ""  // Hint for shard selection
    );

    // Metrics and diagnostics
    SystemMetrics getMetrics() const;
    ShardMetrics getShardMetrics(size_t shardId) const;

    // Configuration
    void setAdaptiveBalancing(bool enabled, double targetUtilization = 0.75);
    void tuneShardCapacity(size_t shardId, size_t newMaxQueue);

    // Lifecycle
    size_t getTotalQueued() const;
    size_t getTotalProcessed() const;
    size_t getTotalRejected() const;
    size_t getTotalWorkers() const;
    size_t getActiveWorkers() const;
    uint64_t getEnqueueLockAvgUs() const;
    uint64_t getDequeueLockAvgUs() const;
    uint64_t getAvgTaskServiceUs() const;
    uint64_t getQueueEnqueueContentionAvgUs() const;
    uint64_t getQueueDequeueWaitAvgUs() const;
    uint64_t getWorkStealAttempts() const;
    uint64_t getWorkStealSuccesses() const;

    // Singleton
    static DBTaskQueuePartitioned& instance();
    static void configure(size_t shards, size_t workersPerShard, size_t maxQueuePerShard);
    static void shutdownInstance();

    // Non-copyable
    DBTaskQueuePartitioned(const DBTaskQueuePartitioned&) = delete;
    DBTaskQueuePartitioned& operator=(const DBTaskQueuePartitioned&) = delete;

private:
    struct ShardState {
        std::unique_ptr<DBTaskQueueSharded> queue;
        std::atomic<uint64_t> tasksProcessed{0};
        std::atomic<uint64_t> tasksRejected{0};
        std::atomic<uint64_t> totalWaitUs{0};
        std::atomic<uint64_t> totalExecUs{0};
        std::atomic<size_t> currentDepth{0};
        std::atomic<bool> isHot{false};
        std::atomic<double> utilizationPct{0.0};
        std::vector<uint64_t> latencyHistogram;  // Track p50/p95/p99
        mutable std::mutex mu;
    };

    size_t shardCount_;
    size_t workersPerShard_;
    size_t maxQueuePerShard_;
    bool enableWorkStealing_;
    bool enableAdaptiveBalancing_;

    std::vector<std::unique_ptr<ShardState>> shards_;
    std::thread metricsThread_;
    std::thread balanceThread_;
    std::atomic<bool> shutdown_{false};
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCv_;
    mutable std::mutex metricsMu_;
    mutable std::chrono::steady_clock::time_point lastMetricsSampleTime_{};
    mutable uint64_t lastMetricsProcessed_{0};

    // Work stealing and load balancing
    std::atomic<size_t> distributionRoundRobin_{0};
    std::atomic<uint64_t> globalSelector_{0};
    std::atomic<double> targetUtilization_{0.75};

    // Metrics collection
    void metricsLoop();
    void balanceLoop();
    void shutdown();

    size_t selectShardAdaptive(const std::string& collection, const std::string& hint);
    size_t selectShardRoundRobin(const std::string& collection);

    void updateShardMetrics(size_t shardId);
    void updateLoadBalance();
};
