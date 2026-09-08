#include "db_task_queue.hpp"
#include "request_timing.hpp"
#include "metrics_exporter.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdlib>
// immintrin.h provides AVX/SSE intrinsics (_mm_pause etc.) — x86_64 only.
// On ARM64/aarch64 the CMake build sets -DNO_IMMINTRIN so we use the ARM yield.
#ifndef NO_IMMINTRIN
#  include <immintrin.h>
#  define CPU_PAUSE() _mm_pause()
#else
#  define CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#endif

namespace {
std::chrono::milliseconds resolveDbqEnqueueWaitBudget() {
    long long waitMs = 40;
    if (const char* env = std::getenv("DBQ_ENQUEUE_WAIT_MS")) {
        try {
            waitMs = std::stoll(env);
        } catch (...) {
            waitMs = 40;
        }
    }
    if (waitMs < 0) waitMs = 0;
    return std::chrono::milliseconds(waitMs);
}
} // namespace

// Static configuration
static std::atomic<size_t> g_cfgWorkers{16};
static std::atomic<size_t> g_cfgMaxQueue{10000};
static std::atomic<size_t> g_cfgBatchSize{32};
static std::once_flag g_cfgOnce;

DBTaskQueue::DBTaskQueue(size_t workers, size_t maxQueue)
    : maxQueueSize_(maxQueue), enqueueWaitBudget_(resolveDbqEnqueueWaitBudget()) {
    for (size_t i = 0; i < workers; ++i) {
        workers_.emplace_back(&DBTaskQueue::workerThread, this);
    }
    std::cout << "[DB-TASK-QUEUE] Started " << workers_.size() << " workers, max queue " << maxQueueSize_ << "\n";
}

DBTaskQueue::~DBTaskQueue() {
    shutdown_.store(true);
    cv_.notify_all();
    for (auto &t : workers_) {
        if (t.joinable()) t.join();
    }
    std::cout << "[DB-TASK-QUEUE] Shutdown complete, processed=" << processed_.load() << "\n";
}

bool DBTaskQueue::enqueue(Task task, Priority priority, const std::string& collection, size_t costHint) {
    auto lockStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(mu_);
    auto lockAcquired = std::chrono::steady_clock::now();
    uint64_t waitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(lockAcquired - lockStart).count());
    enqueueContentionTotalUs_.fetch_add(waitUs, std::memory_order_relaxed);
    enqueueContentionCount_.fetch_add(1, std::memory_order_relaxed);
    enqueueLockTotalUs_.fetch_add(waitUs, std::memory_order_relaxed);
    enqueueLockCount_.fetch_add(1, std::memory_order_relaxed);
    if (totalQueued_ >= maxQueueSize_) {
        if (enqueueWaitBudget_.count() > 0) {
            const auto deadline = std::chrono::steady_clock::now() + enqueueWaitBudget_;
            while (!shutdown_.load() && totalQueued_ >= maxQueueSize_) {
                if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
                    break;
                }
            }
        }
        if (totalQueued_ >= maxQueueSize_) {
            std::cerr << "[DB-TASK-QUEUE] WARNING: Queue full (" << totalQueued_ << ") >= max (" << maxQueueSize_ << ")\n";
            return false;
        }
    }

    TaskEnvelope envelope;
    envelope.task = std::move(task);
    envelope.priority = priority;
    envelope.collection = collection.empty() ? "__default__" : collection;
    envelope.costHint = std::max<size_t>(1, costHint);
    envelope.sequence = ++sequence_;
    envelope.enqueuedAtUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    const size_t pIdx = priorityIndex(priority);
    auto& buckets = queuesByPriority_[pIdx];
    auto inserted = buckets.emplace(envelope.collection, CollectionQueue{});
    auto& q = inserted.first->second;

    if (inserted.second) {
        collectionOrder_[pIdx].push_back(envelope.collection);
    }

    auto pos = std::upper_bound(
        q.begin(), q.end(), envelope,
        [](const TaskEnvelope& lhs, const TaskEnvelope& rhs) {
            if (lhs.costHint != rhs.costHint) return lhs.costHint < rhs.costHint;
            return lhs.sequence < rhs.sequence;
        }
    );
    q.insert(pos, std::move(envelope));
    ++totalQueued_;
    queueDepthSamples_.fetch_add(1, std::memory_order_relaxed);
    queueDepthSampleTotal_.fetch_add(totalQueued_, std::memory_order_relaxed);
    maxObservedQueueDepth_.store(std::max<uint64_t>(maxObservedQueueDepth_.load(std::memory_order_relaxed), totalQueued_), std::memory_order_relaxed);
    const size_t depthBucket = totalQueued_ == 0 ? 0 : (totalQueued_ < 10 ? 1 : totalQueued_ < 25 ? 2 : totalQueued_ < 50 ? 3 : totalQueued_ < 100 ? 4 : 5);
    queueDepthBuckets_[depthBucket].fetch_add(1, std::memory_order_relaxed);
    MetricsExporter::incrementCounter("pacificdb_pipeline_dbq_enqueued_total", 1.0);

    cv_.notify_one();
    return true;
}

size_t DBTaskQueue::getQueued() const {
    std::lock_guard<std::mutex> lk(mu_);
    return totalQueued_;
}

DBTaskQueue& DBTaskQueue::instance() {
    static DBTaskQueue inst(g_cfgWorkers.load(), g_cfgMaxQueue.load());
    return inst;
}

void DBTaskQueue::configure(size_t workers, size_t maxQueue) {
    // Only effective before first instance construction
    std::call_once(g_cfgOnce, [workers, maxQueue]() {
        g_cfgWorkers.store(workers);
        g_cfgMaxQueue.store(maxQueue);
        std::cout << "[DB-TASK-QUEUE] Configured workers=" << workers << " maxQueue=" << maxQueue << "\n";
    });
}

void DBTaskQueue::workerThread() {
    // Phase D.1 optimization: spin-before-sleep to reduce CV wait overhead
    const int SPIN_ITERATIONS = 1000;           // Try ~10-50µs of CPU spinning
    const std::chrono::microseconds YIELD_TIME{10};  // Brief yield after spinning
    const std::chrono::microseconds CV_TIMEOUT{50};  // Short CV timeout, then retry spin

    while (!shutdown_.load()) {
        std::vector<TaskEnvelope> batch;
        batch.reserve(g_cfgBatchSize.load());

        // PHASE 1: Active spin (lock-free check with CPU pauses)
        bool found_work = false;
        for (int spin = 0; spin < SPIN_ITERATIONS && !found_work && !shutdown_.load(); ++spin) {
            // Check queue state without holding lock (may miss work, that's ok)
            if (totalQueued_ > 0) {
                found_work = true;
                break;
            }
            // CPU pause instruction to reduce power/heat while spinning
            // On ARM64 this maps to "yield"; on x86 to _mm_pause()
            CPU_PAUSE();
        }

        // Try to acquire lock and process batch
        bool has_batch = false;
        {
            auto waitStart = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lk(mu_);
            auto lockAcquired = std::chrono::steady_clock::now();
            uint64_t lockWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(lockAcquired - waitStart).count());
            dequeueLockTotalUs_.fetch_add(lockWaitUs, std::memory_order_relaxed);
            dequeueLockCount_.fetch_add(1, std::memory_order_relaxed);

            // Check if we have tasks now that we hold the lock
            if (hasQueuedTasksLocked()) {
                // We have work, pop it
                size_t limit = std::max<size_t>(1, g_cfgBatchSize.load());
                while (batch.size() < limit) {
                    TaskEnvelope next;
                    if (!popNextTaskLocked(next)) {
                        break;
                    }
                    batch.push_back(std::move(next));
                }
                has_batch = !batch.empty();

                // Record that spin worked (we found work during spin phase)
                uint64_t spinEffectiveUs = 0;  // Spinning didn't require CV wait
                cvWaitTotalUs_.fetch_add(spinEffectiveUs, std::memory_order_relaxed);
                cvWaitCount_.fetch_add(1, std::memory_order_relaxed);
            } else if (!shutdown_.load()) {
                // No work after spin, fall back to brief CV wait with timeout
                // PHASE 2: Yield briefly
                lk.unlock();
                std::this_thread::yield();
                lk.lock();

                // PHASE 3: CV wait with timeout (if still no work, retry spin loop)
                auto cvWaitStart = std::chrono::steady_clock::now();
                cv_.wait_for(lk, CV_TIMEOUT, [this] { return shutdown_.load() || hasQueuedTasksLocked(); });
                auto cvWaitEnd = std::chrono::steady_clock::now();

                uint64_t cvWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(cvWaitEnd - cvWaitStart).count());
                cvWaitTotalUs_.fetch_add(cvWaitUs, std::memory_order_relaxed);
                cvWaitCount_.fetch_add(1, std::memory_order_relaxed);

                // Now try to pop work if available
                if (hasQueuedTasksLocked() && !shutdown_.load()) {
                    size_t limit = std::max<size_t>(1, g_cfgBatchSize.load());
                    while (batch.size() < limit) {
                        TaskEnvelope next;
                        if (!popNextTaskLocked(next)) {
                            break;
                        }
                        batch.push_back(std::move(next));
                    }
                    has_batch = !batch.empty();
                }
            }

            if (shutdown_.load() && !hasQueuedTasksLocked()) return;
        }

        if (!batch.empty()) {
            // Wake producers that may be waiting for queue slots.
            cv_.notify_all();
        }

        for (auto& task : batch) {
            if (!task.task) continue;
            try {
                if (task.enqueuedAtUs > 0) {
                    const uint64_t nowUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (nowUs > task.enqueuedAtUs) {
                        pacificdb::timing::recordStage(pacificdb::timing::Stage::DbQueueWait, nowUs - task.enqueuedAtUs);
                    }
                }
                activeWorkers_.fetch_add(1, std::memory_order_relaxed);
                auto svcStart = std::chrono::steady_clock::now();
                task.task();
                auto svcEnd = std::chrono::steady_clock::now();
                uint64_t svcUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(svcEnd - svcStart).count());
                serviceTotalUs_.fetch_add(svcUs, std::memory_order_relaxed);
                serviceCount_.fetch_add(1, std::memory_order_relaxed);
                processed_++;
                MetricsExporter::incrementCounter("pacificdb_pipeline_dbq_completed_total", 1.0);
            } catch (const std::exception &e) {
                std::cerr << "[DB-TASK-QUEUE] Worker caught exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[DB-TASK-QUEUE] Worker caught unknown exception\n";
            }
            activeWorkers_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

bool DBTaskQueue::hasQueuedTasksLocked() const {
    return totalQueued_ > 0;
}

size_t DBTaskQueue::priorityIndex(Priority p) {
    switch (p) {
        case Priority::HIGH: return 0;
        case Priority::MEDIUM: return 1;
        case Priority::LOW: return 2;
    }
    return 1;
}

bool DBTaskQueue::popNextTaskLocked(TaskEnvelope& out) {
    if (totalQueued_ == 0) return false;

    auto popFromPriority = [&](size_t pIdx, TaskEnvelope& candidate) -> bool {
        auto& buckets = queuesByPriority_[pIdx];
        auto& order = collectionOrder_[pIdx];
        if (order.empty()) return false;

        if (rrIndex_[pIdx] >= order.size()) rrIndex_[pIdx] = 0;
        size_t attempts = order.size();

        while (attempts-- > 0 && !order.empty()) {
            if (rrIndex_[pIdx] >= order.size()) rrIndex_[pIdx] = 0;
            const std::string key = order[rrIndex_[pIdx]];
            auto it = buckets.find(key);
            if (it == buckets.end() || it->second.empty()) {
                if (it != buckets.end()) buckets.erase(it);
                order.erase(order.begin() + rrIndex_[pIdx]);
                if (order.empty()) {
                    rrIndex_[pIdx] = 0;
                    break;
                }
                if (rrIndex_[pIdx] >= order.size()) rrIndex_[pIdx] = 0;
                continue;
            }

            candidate = std::move(it->second.front());
            it->second.pop_front();
            --totalQueued_;

            if (it->second.empty()) {
                buckets.erase(it);
                order.erase(order.begin() + rrIndex_[pIdx]);
                if (!order.empty() && rrIndex_[pIdx] >= order.size()) rrIndex_[pIdx] = 0;
            } else {
                rrIndex_[pIdx] = (rrIndex_[pIdx] + 1) % order.size();
            }

            return true;
        }

        return false;
    };

    size_t checked = 0;
    while (checked < queuesByPriority_.size()) {
        size_t pIdx = priorityCursor_ % queuesByPriority_.size();

        if (priorityBudgetRemaining_[pIdx] == 0) {
            priorityBudgetRemaining_[pIdx] = priorityQuantum_[pIdx];
            priorityCursor_ = (priorityCursor_ + 1) % queuesByPriority_.size();
            ++checked;
            continue;
        }

        if (popFromPriority(pIdx, out)) {
            if (priorityBudgetRemaining_[pIdx] > 0) {
                --priorityBudgetRemaining_[pIdx];
            }
            if (priorityBudgetRemaining_[pIdx] == 0 || collectionOrder_[pIdx].empty()) {
                priorityBudgetRemaining_[pIdx] = priorityQuantum_[pIdx];
                priorityCursor_ = (priorityCursor_ + 1) % queuesByPriority_.size();
            }
            return true;
        }

        priorityBudgetRemaining_[pIdx] = priorityQuantum_[pIdx];
        priorityCursor_ = (priorityCursor_ + 1) % queuesByPriority_.size();
        ++checked;
    }

    return false;
}
