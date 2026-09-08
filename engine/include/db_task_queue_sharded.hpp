#pragma once

#include "db_task_queue.hpp"
#include <functional>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <memory>

/**
 * Sharded task queue to reduce lock contention and improve throughput.
 * Distributes tasks across multiple shards using consistent hashing on collection name.
 * Each shard has its own lock and condition variable, allowing parallel task processing.
 * Target improvement: 5-10x throughput increase from reduced lock contention.
 */
class DBTaskQueueSharded {
public:
    using Task = std::function<void()>;
    enum class Priority : uint8_t {
        HIGH = 0,
        MEDIUM = 1,
        LOW = 2
    };

    DBTaskQueueSharded(size_t shards = 16, size_t workersPerShard = 2, size_t maxQueuePerShard = 1000);
    ~DBTaskQueueSharded();

    bool enqueue(Task task,
                 Priority priority = Priority::MEDIUM,
                 const std::string& collection = "",
                 size_t costHint = 1);

    size_t getQueued() const;
    size_t getTotalWorkers() const { return totalWorkers_; }
    size_t getShardCount() const { return shardCount_; }

    static DBTaskQueueSharded& instance();
    static void configure(size_t shards, size_t workersPerShard, size_t maxQueuePerShard);

    // non-copyable
    DBTaskQueueSharded(const DBTaskQueueSharded&) = delete;
    DBTaskQueueSharded& operator=(const DBTaskQueueSharded&) = delete;

    // Metrics
    size_t getProcessedTasks() const;
    size_t getActiveWorkers() const;
    uint64_t getEnqueueLockAvgUs() const;
    uint64_t getDequeueLockAvgUs() const;
    uint64_t getMaxQueueDepth() const;
    uint64_t getWorkStealSuccesses() const;
    uint64_t getWorkStealAttempts() const;

private:
    struct Shard {
        struct TaskEnvelope {
            Task task;
            Priority priority;
            std::string collection;
            size_t costHint;
            uint64_t sequence;
            uint64_t enqueuedAtUs;
        };

        using CollectionQueue = std::deque<TaskEnvelope>;
        std::array<std::unordered_map<std::string, CollectionQueue>, 3> queuesByPriority;
        std::array<std::vector<std::string>, 3> collectionOrder;
        std::array<size_t, 3> rrIndex{{0, 0, 0}};
        size_t priorityCursor{0};
        std::array<size_t, 3> priorityBudgetRemaining{{8, 4, 2}};

        Shard() {
            // Pre-allocate bucket arrays so _M_bucket_count is never 0 when the
            // inlined unordered_map hash lookup executes `div _M_bucket_count`.
            // A default-constructed map starts with 1 bucket, but under compiler
            // optimisation the initial-value proof is lost; reserve() forces a
            // real allocation (≥64 buckets) before any concurrent access begins.
            for (auto& pq : queuesByPriority) pq.reserve(64);
        }

        mutable std::mutex mu;
        std::condition_variable cv;
        size_t totalQueued{0};
        uint64_t sequence{0};

        std::atomic<uint64_t> processed{0};
        std::atomic<uint64_t> enqueueLockTotalUs{0};
        std::atomic<uint64_t> enqueueLockCount{0};
        std::atomic<uint64_t> dequeueLockTotalUs{0};
        std::atomic<uint64_t> dequeueLockCount{0};
        std::atomic<uint64_t> maxQueueDepth{0};
        std::atomic<size_t> activeWorkers{0};
        std::atomic<bool> shutdown{false};
        std::atomic<uint64_t> stealAttempts{0};
        std::atomic<uint64_t> stealSuccesses{0};
    };

    void workerThread(size_t shardId);
    size_t hashCollection(const std::string& collection) const;
    static size_t priorityIndex(Priority p);

    // Work stealing: try to steal tasks from other shards
    bool stealFromNeighbor(size_t currentShardId, std::vector<Shard::TaskEnvelope>& outTasks, size_t maxTasks = 8);
    Shard::TaskEnvelope* tryStealFromShard(size_t shardId, bool lockIfHaveLock = true);

    size_t shardCount_;
    size_t workersPerShard_;
    size_t maxQueuePerShard_;
    size_t totalWorkers_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<std::thread> workers_;
    std::atomic<bool> shutdown_{false};
};
