#pragma once

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

class DBTaskQueue {
public:
    using Task = std::function<void()>;
    enum class Priority : uint8_t {
        HIGH = 0,
        MEDIUM = 1,
        LOW = 2
    };

    DBTaskQueue(size_t workers = 16, size_t maxQueue = 10000);
    ~DBTaskQueue();

    bool enqueue(Task task,
                 Priority priority = Priority::MEDIUM,
                 const std::string& collection = "",
                 size_t costHint = 1);

    size_t getQueued() const;

    static DBTaskQueue& instance();

    // Configure before first use (applies to the singleton on creation)
    static void configure(size_t workers, size_t maxQueue);

    // non-copyable
    DBTaskQueue(const DBTaskQueue&) = delete;
    DBTaskQueue& operator=(const DBTaskQueue&) = delete;

    size_t getActiveWorkers() const { return activeWorkers_.load(); }
    size_t getProcessedTasks() const { return processed_.load(); }
    size_t getTotalWorkers() const { return workers_.size(); }
    uint64_t getEnqueueLockAvgUs() const {
        uint64_t c = enqueueLockCount_.load();
        return c == 0 ? 0 : enqueueLockTotalUs_.load() / c;
    }
    uint64_t getDequeueLockAvgUs() const {
        uint64_t c = dequeueLockCount_.load();
        return c == 0 ? 0 : dequeueLockTotalUs_.load() / c;
    }
    uint64_t getCvWaitAvgUs() const {
        uint64_t c = cvWaitCount_.load();
        return c == 0 ? 0 : cvWaitTotalUs_.load() / c;
    }
    uint64_t getSpuriousWakeups() const { return spuriousWakeups_.load(); }
    uint64_t getMaxQueueDepth() const { return maxObservedQueueDepth_.load(); }
    uint64_t getQueueDepthSamples() const { return queueDepthSamples_.load(); }
    uint64_t getQueueDepthSampleTotal() const { return queueDepthSampleTotal_.load(); }
    uint64_t getQueueDepthBucket0() const { return queueDepthBuckets_[0].load(); }
    uint64_t getQueueDepthBucket1() const { return queueDepthBuckets_[1].load(); }
    uint64_t getQueueDepthBucket2() const { return queueDepthBuckets_[2].load(); }
    uint64_t getQueueDepthBucket3() const { return queueDepthBuckets_[3].load(); }
    uint64_t getQueueDepthBucket4() const { return queueDepthBuckets_[4].load(); }
    uint64_t getQueueDepthBucket5() const { return queueDepthBuckets_[5].load(); }
    uint64_t getAvgTaskServiceUs() const {
        uint64_t c = serviceCount_.load();
        return c == 0 ? 0 : serviceTotalUs_.load() / c;
    }
    uint64_t getQueueEnqueueContentionAvgUs() const {
        uint64_t c = enqueueContentionCount_.load();
        return c == 0 ? 0 : enqueueContentionTotalUs_.load() / c;
    }
    uint64_t getQueueDequeueWaitAvgUs() const {
        uint64_t c = dequeueWaitCount_.load();
        return c == 0 ? 0 : dequeueWaitTotalUs_.load() / c;
    }

private:
    struct TaskEnvelope {
        Task task;
        Priority priority;
        std::string collection;
        size_t costHint;
        uint64_t sequence;
        uint64_t enqueuedAtUs;
    };

    void workerThread();
    bool hasQueuedTasksLocked() const;
    bool popNextTaskLocked(TaskEnvelope& out);
    static size_t priorityIndex(Priority p);

    std::vector<std::thread> workers_;
    using CollectionQueue = std::deque<TaskEnvelope>;
    std::array<std::unordered_map<std::string, CollectionQueue>, 3> queuesByPriority_;
    std::array<std::vector<std::string>, 3> collectionOrder_;
    std::array<size_t, 3> rrIndex_{};
    size_t priorityCursor_{0};
    std::array<size_t, 3> priorityBudgetRemaining_{{8, 4, 2}};
    const std::array<size_t, 3> priorityQuantum_{{8, 4, 2}};
    mutable std::mutex mu_;
    std::condition_variable cv_;
    size_t totalQueued_{0};
    uint64_t sequence_{0};

    const size_t maxQueueSize_;
    std::chrono::milliseconds enqueueWaitBudget_{40};
    std::atomic<bool> shutdown_{false};
    std::atomic<size_t> processed_{0};
    std::atomic<size_t> activeWorkers_{0};
    // Service timing and contention stats
    std::atomic<uint64_t> serviceTotalUs_{0};
    std::atomic<uint64_t> serviceCount_{0};
    std::atomic<uint64_t> enqueueContentionTotalUs_{0};
    std::atomic<uint64_t> enqueueContentionCount_{0};
    std::atomic<uint64_t> dequeueWaitTotalUs_{0};
    std::atomic<uint64_t> dequeueWaitCount_{0};
    std::atomic<uint64_t> enqueueLockTotalUs_{0};
    std::atomic<uint64_t> enqueueLockCount_{0};
    std::atomic<uint64_t> dequeueLockTotalUs_{0};
    std::atomic<uint64_t> dequeueLockCount_{0};
    std::atomic<uint64_t> cvWaitTotalUs_{0};
    std::atomic<uint64_t> cvWaitCount_{0};
    std::atomic<uint64_t> spuriousWakeups_{0};
    std::atomic<uint64_t> maxObservedQueueDepth_{0};
    std::atomic<uint64_t> queueDepthSamples_{0};
    std::atomic<uint64_t> queueDepthSampleTotal_{0};
    std::array<std::atomic<uint64_t>, 6> queueDepthBuckets_{{0, 0, 0, 0, 0, 0}};
};
