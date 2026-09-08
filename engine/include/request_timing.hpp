#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "metrics_exporter.hpp"

namespace pacificdb::timing {

enum class Stage : std::size_t {
    QueueWait = 0,
    Parse,
    Auth,
    WalAppend,
    WalFsync,
    LockWait,
    MemtableInsert,
    Replication,
    ResponseSend,
    DbQueueWait,
    ConnPoolWait,
    AcceptWait,
    Count
};

struct StageAggregate {
    std::atomic<uint64_t> totalUs{0};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> maxUs{0};
};

struct RequestTimingContext {
    std::array<std::atomic<uint64_t>, static_cast<std::size_t>(Stage::Count)> totalsUs{};
    std::array<std::atomic<uint64_t>, static_cast<std::size_t>(Stage::Count)> counts{};

    void record(Stage stage, uint64_t us) {
        const auto index = static_cast<std::size_t>(stage);
        totalsUs[index].fetch_add(us, std::memory_order_relaxed);
        counts[index].fetch_add(1, std::memory_order_relaxed);
    }
};

inline std::array<StageAggregate, static_cast<std::size_t>(Stage::Count)>& stageAggregates() {
    static std::array<StageAggregate, static_cast<std::size_t>(Stage::Count)> aggregates{};
    return aggregates;
}

inline thread_local std::shared_ptr<RequestTimingContext> g_requestTimingContext;

inline std::shared_ptr<RequestTimingContext> makeRequestTimingContext() {
    return std::make_shared<RequestTimingContext>();
}

inline void setRequestTimingContext(std::shared_ptr<RequestTimingContext> context) {
    g_requestTimingContext = std::move(context);
}

inline void clearRequestTimingContext() {
    g_requestTimingContext.reset();
}

inline bool requestTimingActive() {
    return static_cast<bool>(g_requestTimingContext);
}

inline void recordStage(Stage stage, uint64_t us) {
    if (g_requestTimingContext) {
        g_requestTimingContext->record(stage, us);
    }

    auto& aggregate = stageAggregates()[static_cast<std::size_t>(stage)];
    aggregate.totalUs.fetch_add(us, std::memory_order_relaxed);
    aggregate.count.fetch_add(1, std::memory_order_relaxed);

    uint64_t previous = aggregate.maxUs.load(std::memory_order_relaxed);
    while (us > previous && !aggregate.maxUs.compare_exchange_weak(
               previous, us, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

inline uint64_t aggregateAverageUs(Stage stage) {
    const auto& aggregate = stageAggregates()[static_cast<std::size_t>(stage)];
    const uint64_t count = aggregate.count.load(std::memory_order_relaxed);
    if (count == 0) return 0;
    return aggregate.totalUs.load(std::memory_order_relaxed) / count;
}

inline uint64_t aggregateTotalUs(Stage stage) {
    return stageAggregates()[static_cast<std::size_t>(stage)].totalUs.load(std::memory_order_relaxed);
}

inline uint64_t aggregateCount(Stage stage) {
    return stageAggregates()[static_cast<std::size_t>(stage)].count.load(std::memory_order_relaxed);
}

inline uint64_t aggregateMaxUs(Stage stage) {
    return stageAggregates()[static_cast<std::size_t>(stage)].maxUs.load(std::memory_order_relaxed);
}

inline uint64_t contextAverageUs(const std::shared_ptr<RequestTimingContext>& context, Stage stage) {
    if (!context) return 0;
    const auto index = static_cast<std::size_t>(stage);
    const uint64_t count = context->counts[index].load(std::memory_order_relaxed);
    if (count == 0) return 0;
    return context->totalsUs[index].load(std::memory_order_relaxed) / count;
}

inline uint64_t contextTotalUs(const std::shared_ptr<RequestTimingContext>& context, Stage stage) {
    if (!context) return 0;
    uint64_t total = context->totalsUs[static_cast<std::size_t>(stage)].load(std::memory_order_relaxed);
    if (stage == Stage::MemtableInsert && total == 0) {
        // Debug: check if any records were made to this stage
        uint64_t count = context->counts[static_cast<std::size_t>(stage)].load(std::memory_order_relaxed);
        if (count > 0) {
            // This shouldn't happen - count > 0 but total == 0
            std::cerr << "[TIMING DEBUG] MemtableInsert: count=" << count << " but total=0 (impossible!)\n";
        }
    }
    return total;
}

inline uint64_t contextCount(const std::shared_ptr<RequestTimingContext>& context, Stage stage) {
    if (!context) return 0;
    return context->counts[static_cast<std::size_t>(stage)].load(std::memory_order_relaxed);
}

inline void recordLockProfile(const std::string& metricBase, uint64_t waitUs, uint64_t holdUs, bool contended) {
    if (metricBase.empty()) return;
    // Record samples into histogram buckets so we can compute percentiles
    MetricsExporter::recordHistogram(metricBase + "_wait_us", static_cast<double>(waitUs));
    MetricsExporter::recordHistogram(metricBase + "_hold_us", static_cast<double>(holdUs));
    if (contended) MetricsExporter::incrementCounter(metricBase + "_contention_total", 1.0);
}

template <typename MutexT>
class TimedUniqueLock {
public:
    explicit TimedUniqueLock(MutexT& mutex, std::string metricBase = {})
        : metricBase_(std::move(metricBase)), start_(std::chrono::steady_clock::now()), lock_(mutex) {
        recordWait();
    }

    TimedUniqueLock(TimedUniqueLock&& other) noexcept
        : metricBase_(std::move(other.metricBase_)),
          start_(other.start_),
          acquiredAt_(other.acquiredAt_),
          lock_(std::move(other.lock_)),
          armed_(other.armed_),
          waitUs_(other.waitUs_),
          holdRecorded_(other.holdRecorded_) {
        other.armed_ = false;
    }

    TimedUniqueLock& operator=(TimedUniqueLock&& other) noexcept {
        if (this != &other) {
            if (armed_ && !holdRecorded_) {
                recordHold();
            }
            metricBase_ = std::move(other.metricBase_);
            start_ = other.start_;
            acquiredAt_ = other.acquiredAt_;
            lock_ = std::move(other.lock_);
            armed_ = other.armed_;
            waitUs_ = other.waitUs_;
            holdRecorded_ = other.holdRecorded_;
            other.armed_ = false;
        }
        return *this;
    }
    TimedUniqueLock(const TimedUniqueLock&) = delete;
    TimedUniqueLock& operator=(const TimedUniqueLock&) = delete;

    ~TimedUniqueLock() {
        if (armed_ && !holdRecorded_) {
            recordHold();
        }
    }

    auto* operator->() { return &lock_; }
    auto& get() { return lock_; }
    void unlock() {
        if (armed_ && !holdRecorded_) {
            recordHold();
        }
        lock_.unlock();
    }
    void lock() { lock_.lock(); }
    explicit operator bool() const { return lock_.owns_lock(); }

private:
    void recordWait() {
        const auto end = std::chrono::steady_clock::now();
        waitUs_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count());
        acquiredAt_ = end;
        recordStage(Stage::LockWait, waitUs_);
    }

    void recordHold() {
        const auto end = std::chrono::steady_clock::now();
        const uint64_t holdUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - acquiredAt_).count());
        recordLockProfile(metricBase_, waitUs_, holdUs, waitUs_ > 0);
        holdRecorded_ = true;
    }

    std::string metricBase_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point acquiredAt_;
    std::unique_lock<MutexT> lock_;
    bool armed_{true};
    uint64_t waitUs_{0};
    bool holdRecorded_{false};
};

template <typename MutexT>
class TimedSharedLock {
public:
    explicit TimedSharedLock(MutexT& mutex, std::string metricBase = {})
        : metricBase_(std::move(metricBase)), start_(std::chrono::steady_clock::now()), lock_(mutex) {
        recordWait();
    }

    TimedSharedLock(TimedSharedLock&& other) noexcept
        : metricBase_(std::move(other.metricBase_)),
          start_(other.start_),
          acquiredAt_(other.acquiredAt_),
          lock_(std::move(other.lock_)),
          armed_(other.armed_),
          waitUs_(other.waitUs_),
          holdRecorded_(other.holdRecorded_) {
        other.armed_ = false;
    }

    TimedSharedLock& operator=(TimedSharedLock&& other) noexcept {
        if (this != &other) {
            if (armed_ && !holdRecorded_) {
                recordHold();
            }
            metricBase_ = std::move(other.metricBase_);
            start_ = other.start_;
            acquiredAt_ = other.acquiredAt_;
            lock_ = std::move(other.lock_);
            armed_ = other.armed_;
            waitUs_ = other.waitUs_;
            holdRecorded_ = other.holdRecorded_;
            other.armed_ = false;
        }
        return *this;
    }
    TimedSharedLock(const TimedSharedLock&) = delete;
    TimedSharedLock& operator=(const TimedSharedLock&) = delete;

    ~TimedSharedLock() {
        if (armed_ && !holdRecorded_) {
            recordHold();
        }
    }

    auto* operator->() { return &lock_; }
    auto& get() { return lock_; }
    void unlock() {
        if (armed_ && !holdRecorded_) {
            recordHold();
        }
        lock_.unlock();
    }
    void lock() { lock_.lock(); }
    explicit operator bool() const { return lock_.owns_lock(); }

private:
    void recordWait() {
        const auto end = std::chrono::steady_clock::now();
        waitUs_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count());
        acquiredAt_ = end;
        recordStage(Stage::LockWait, waitUs_);
    }

    void recordHold() {
        const auto end = std::chrono::steady_clock::now();
        const uint64_t holdUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - acquiredAt_).count());
        recordLockProfile(metricBase_, waitUs_, holdUs, waitUs_ > 0);
        holdRecorded_ = true;
    }

    std::string metricBase_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point acquiredAt_;
    std::shared_lock<MutexT> lock_;
    bool armed_{true};
    uint64_t waitUs_{0};
    bool holdRecorded_{false};
};

template <typename MutexT>
TimedUniqueLock<MutexT> makeTimedUniqueLock(MutexT& mutex, std::string metricBase = {}) {
    return TimedUniqueLock<MutexT>(mutex, std::move(metricBase));
}

template <typename MutexT>
TimedSharedLock<MutexT> makeTimedSharedLock(MutexT& mutex, std::string metricBase = {}) {
    return TimedSharedLock<MutexT>(mutex, std::move(metricBase));
}

} // namespace pacificdb::timing
