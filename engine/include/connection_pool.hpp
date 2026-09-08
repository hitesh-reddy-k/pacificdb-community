#pragma once

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <deque>
#include <atomic>
#include <chrono>
#include <string>
#include <cstdint>

/**
 * ConnectionPool - Thread pool for handling database connections efficiently
 *
 * Before: 1 thread per connection (spawned and destroyed for each client)
 * After: Fixed thread pool that reuses worker threads for multiple connections
 *
 * Benefits:
 * - Reduced thread creation/destruction overhead
 * - Controlled concurrency (prevents thread explosion)
 * - Better CPU cache utilization
 * - Predictable resource usage
 */
class ConnectionPool {
public:
    using Task = std::function<void()>;
    enum class Priority {
        HIGH,
        NORMAL,
        LOW
    };

    /**
     * Initialize connection pool with worker threads
     * @param minThreads Minimum number of worker threads (always alive)
     * @param maxThreads Maximum number of worker threads
     */
    // Production defaults: higher thread count and queue size for throughput
    // maxQueueSize controls how many pending connections can be queued before rejecting
    ConnectionPool(size_t minThreads = 32, size_t maxThreads = 512, size_t maxQueueSize = 16384);

    ~ConnectionPool();

    /**
     * Submit a client connection handler to the pool
     * @param task Function to execute (typically handleClient)
     * @return true if task was queued, false if pool is full
     */
    // Submit a task with an optional per-task timeout. If the task runs longer than
    // 'timeout', the pool's watchdog will mark the task's cancellation token and
    // long-running operations should check QueryCancel::isCancelled() cooperatively.
    bool submit(Task task,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                Priority priority = Priority::NORMAL);

    // Submit multiple tasks in one lock/notify cycle.
    // Returns number of tasks accepted (may be less than tasks.size() if queue is near full).
    size_t submitBatch(std::vector<Task> tasks,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                       Priority priority = Priority::NORMAL);

    /**
     * Get current statistics
     */
    size_t getActiveThreads() const { return activeThreads_.load(); }
    size_t getTotalThreads() const { return totalThreads_.load(); }
    size_t getQueuedTasks() const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
    }
    size_t getProcessedTasks() const { return processedTasks_.load(); }
    uint64_t getQueueMutexWaitAvgUs() const {
        uint64_t count = queueMutexWaitCount_.load();
        return count == 0 ? 0 : queueMutexWaitTotalUs_.load() / count;
    }
    uint64_t getQueueCvWaitAvgUs() const {
        uint64_t count = queueCvWaitCount_.load();
        return count == 0 ? 0 : queueCvWaitTotalUs_.load() / count;
    }
    uint64_t getQueueSpuriousWakeups() const { return queueSpuriousWakeups_.load(); }
    uint64_t getQueueDepthMax() const { return queueDepthMax_.load(); }
    uint64_t getQueueDepthAvg() const {
        uint64_t count = queueDepthSamples_.load();
        return count == 0 ? 0 : queueDepthTotal_.load() / count;
    }
    uint64_t getWorkerActiveExecAvgUs() const {
        uint64_t count = workerExecCount_.load();
        return count == 0 ? 0 : workerExecTotalUs_.load() / count;
    }
    uint64_t getWorkerBlockedMutexAvgUs() const {
        uint64_t count = workerBlockedMutexCount_.load();
        return count == 0 ? 0 : workerBlockedMutexTotalUs_.load() / count;
    }
    uint64_t getWorkerBlockedCvAvgUs() const {
        uint64_t count = workerBlockedCvCount_.load();
        return count == 0 ? 0 : workerBlockedCvTotalUs_.load() / count;
    }
    uint64_t getWorkerIdleAvgUs() const {
        uint64_t count = workerIdleCount_.load();
        return count == 0 ? 0 : workerIdleTotalUs_.load() / count;
    }

    /**
     * Graceful shutdown - wait for all tasks to complete
     */
    void shutdown();

private:
    Task wrapTask(Task task, std::chrono::milliseconds timeout);
    Task wrapTask(Task task, std::chrono::milliseconds timeout, uint64_t enqueuedAtUs);
    std::deque<Task>& queueForPriority(Priority priority);
    bool hasPendingTasksUnsafe() const;
    Task popNextTaskUnsafe();
    void workerThread();
    void scaleUp();
    void scaleDown();
    void monitorThread();

    std::vector<std::thread> workers_;
    std::deque<Task> highPriorityQueue_;
    std::deque<Task> normalPriorityQueue_;
    std::deque<Task> lowPriorityQueue_;

    mutable std::mutex queueMutex_;
    std::condition_variable condition_;

    std::atomic<bool> shutdown_{false};
    std::atomic<size_t> activeThreads_{0};
    std::atomic<size_t> totalThreads_{0};
    std::atomic<size_t> processedTasks_{0};
    std::atomic<uint64_t> queueMutexWaitTotalUs_{0};
    std::atomic<uint64_t> queueMutexWaitCount_{0};
    std::atomic<uint64_t> queueCvWaitTotalUs_{0};
    std::atomic<uint64_t> queueCvWaitCount_{0};
    std::atomic<uint64_t> queueSpuriousWakeups_{0};
    std::atomic<uint64_t> queueDepthSamples_{0};
    std::atomic<uint64_t> queueDepthTotal_{0};
    std::atomic<uint64_t> queueDepthMax_{0};
    std::atomic<uint64_t> workerExecTotalUs_{0};
    std::atomic<uint64_t> workerExecCount_{0};
    std::atomic<uint64_t> workerBlockedMutexTotalUs_{0};
    std::atomic<uint64_t> workerBlockedMutexCount_{0};
    std::atomic<uint64_t> workerBlockedCvTotalUs_{0};
    std::atomic<uint64_t> workerBlockedCvCount_{0};
    std::atomic<uint64_t> workerIdleTotalUs_{0};
    std::atomic<uint64_t> workerIdleCount_{0};

    // Optional short wait budget before rejecting submissions when queue is full.
    // Controlled via CONN_POOL_ENQUEUE_WAIT_MS (default: 25ms).
    std::chrono::milliseconds enqueueWaitBudget_{25};

    const size_t minThreads_;
    const size_t maxThreads_;
    size_t maxQueueSize_ = 16384;  // Production: high capacity queue

    std::thread monitor_;
};

// Global instance
extern ConnectionPool* g_connectionPool;

// Initialize global pool - production defaults
// maxQueueSize is optional and defaults to 16384 for high throughput
void initConnectionPool(size_t minThreads = 32, size_t maxThreads = 512, size_t maxQueueSize = 16384);

// Cleanup global pool
void destroyConnectionPool();
