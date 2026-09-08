#include "db_task_queue_sharded.hpp"
#include "metrics_exporter.hpp"
#include <iostream>
#include <functional>
#include <algorithm>
// immintrin.h provides AVX/SSE intrinsics — x86_64 only.
// On ARM64/aarch64 the CMake build sets -DNO_IMMINTRIN so we skip this.
#ifndef NO_IMMINTRIN
#  include <immintrin.h>
#endif
#include <cstdlib>

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
}

// Static configuration
static std::atomic<size_t> g_cfgShards{16};
static std::atomic<size_t> g_cfgWorkersPerShard{2};
static std::atomic<size_t> g_cfgMaxQueuePerShard{1000};
static std::once_flag g_cfgOnce;

DBTaskQueueSharded::DBTaskQueueSharded(size_t shards, size_t workersPerShard, size_t maxQueuePerShard)
    : shardCount_(std::max<size_t>(1, shards)),
      workersPerShard_(std::max<size_t>(1, workersPerShard)),
      maxQueuePerShard_(std::max<size_t>(1, maxQueuePerShard)),
      totalWorkers_(std::max<size_t>(1, shards) * std::max<size_t>(1, workersPerShard)) {

    // Create shards
    for (size_t i = 0; i < shardCount_; ++i) {
        shards_.emplace_back(std::make_unique<Shard>());
    }

    // Start workers
    for (size_t i = 0; i < totalWorkers_; ++i) {
        size_t shardId = i % shardCount_;
        workers_.emplace_back(&DBTaskQueueSharded::workerThread, this, shardId);
    }

    std::cout << "[DB-TASK-QUEUE-SHARDED] Started " << totalWorkers_ << " workers across "
              << shardCount_ << " shards (" << workersPerShard_ << " workers per shard), "
              << "max queue per shard: " << maxQueuePerShard_ << "\n";
}

DBTaskQueueSharded::~DBTaskQueueSharded() {
    shutdown_.store(true);
    for (auto& shard : shards_) {
        shard->shutdown.store(true);
        shard->cv.notify_all();
    }
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    uint64_t totalProcessed = 0;
    for (auto& shard : shards_) {
        totalProcessed += shard->processed.load();
    }
    std::cout << "[DB-TASK-QUEUE-SHARDED] Shutdown complete, processed=" << totalProcessed << "\n";
}

size_t DBTaskQueueSharded::hashCollection(const std::string& collection) const {
    const size_t sc = shardCount_;
    if (sc == 0 || shards_.empty()) return 0;
    if (collection.empty()) return 0;
    std::hash<std::string> hasher;
    size_t h = hasher(collection);
    if (sc == 0) return 0;  // re-check after hash: guards compiler-reordered path
    return h % sc;
}

size_t DBTaskQueueSharded::priorityIndex(Priority p) {
    // queuesByPriority/collectionOrder are size-3 arrays (HIGH=0, MEDIUM=1, LOW=2).
    // Clamp defensively: a caller passing an out-of-range value (e.g. via a raw cast
    // from a wider priority enum) must never index past the array — that is undefined
    // behaviour and was the cause of a SIGFPE (reading garbage memory as an
    // unordered_map → hash % bucket_count where bucket_count==0).
    size_t idx = static_cast<size_t>(p);
    return idx > 2 ? 2 : idx;
}

bool DBTaskQueueSharded::enqueue(Task task, Priority priority, const std::string& collection, size_t costHint) {
    size_t shardId = hashCollection(collection);
    if (shards_.empty()) return false;
    if (shardId >= shards_.size()) shardId = 0;
    auto& shard = *shards_[shardId];

    auto lockStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(shard.mu);
    auto lockAcquired = std::chrono::steady_clock::now();

    uint64_t waitUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(lockAcquired - lockStart).count()
    );
    shard.enqueueLockTotalUs.fetch_add(waitUs, std::memory_order_relaxed);
    shard.enqueueLockCount.fetch_add(1, std::memory_order_relaxed);

    // Check queue depth
    if (shard.totalQueued >= maxQueuePerShard_) {
        std::cerr << "[DB-TASK-QUEUE-SHARDED] Shard " << shardId << " queue full ("
                  << shard.totalQueued << " >= " << maxQueuePerShard_ << ")\n";
        return false;
    }

    // Create task envelope
    Shard::TaskEnvelope envelope{};
    envelope.task = std::move(task);
    envelope.priority = priority;
    envelope.collection = collection.empty() ? "__default__" : collection;
    envelope.costHint = std::max<size_t>(1, costHint);
    envelope.sequence = ++shard.sequence;
    envelope.enqueuedAtUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    // Insert into priority queue
    const size_t pIdx = priorityIndex(priority);
    auto& buckets = shard.queuesByPriority[pIdx];
    auto inserted = buckets.emplace(envelope.collection, Shard::CollectionQueue{});
    auto& q = inserted.first->second;

    if (inserted.second) {
        shard.collectionOrder[pIdx].push_back(envelope.collection);
    }

    // Insert maintaining cost ordering
    auto pos = std::upper_bound(
        q.begin(), q.end(), envelope,
        [](const Shard::TaskEnvelope& lhs, const Shard::TaskEnvelope& rhs) {
            if (lhs.costHint != rhs.costHint) return lhs.costHint < rhs.costHint;
            return lhs.sequence < rhs.sequence;
        }
    );
    q.insert(pos, std::move(envelope));
    ++shard.totalQueued;

    shard.maxQueueDepth.store(
        std::max<uint64_t>(shard.maxQueueDepth.load(std::memory_order_relaxed), shard.totalQueued),
        std::memory_order_relaxed
    );

    MetricsExporter::incrementCounter("pacificdb_pipeline_dbq_sharded_enqueued_total", 1.0);

    shard.cv.notify_one();
    return true;
}

size_t DBTaskQueueSharded::getQueued() const {
    size_t total = 0;
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lk(shard->mu);
        total += shard->totalQueued;
    }
    return total;
}

size_t DBTaskQueueSharded::getProcessedTasks() const {
    uint64_t total = 0;
    for (auto& shard : shards_) {
        total += shard->processed.load();
    }
    return total;
}

size_t DBTaskQueueSharded::getActiveWorkers() const {
    size_t total = 0;
    for (auto& shard : shards_) {
        total += shard->activeWorkers.load(std::memory_order_relaxed);
    }
    return total;
}

uint64_t DBTaskQueueSharded::getEnqueueLockAvgUs() const {
    uint64_t totalUs = 0;
    uint64_t totalCount = 0;
    for (auto& shard : shards_) {
        totalUs += shard->enqueueLockTotalUs.load();
        totalCount += shard->enqueueLockCount.load();
    }
    return totalCount == 0 ? 0 : totalUs / totalCount;
}

uint64_t DBTaskQueueSharded::getDequeueLockAvgUs() const {
    uint64_t totalUs = 0;
    uint64_t totalCount = 0;
    for (auto& shard : shards_) {
        totalUs += shard->dequeueLockTotalUs.load();
        totalCount += shard->dequeueLockCount.load();
    }
    return totalCount == 0 ? 0 : totalUs / totalCount;
}

uint64_t DBTaskQueueSharded::getMaxQueueDepth() const {
    uint64_t maxDepth = 0;
    for (auto& shard : shards_) {
        maxDepth = std::max(maxDepth, shard->maxQueueDepth.load());
    }
    return maxDepth;
}

uint64_t DBTaskQueueSharded::getWorkStealAttempts() const {
    uint64_t total = 0;
    for (auto& shard : shards_) {
        total += shard->stealAttempts.load();
    }
    return total;
}

uint64_t DBTaskQueueSharded::getWorkStealSuccesses() const {
    uint64_t total = 0;
    for (auto& shard : shards_) {
        total += shard->stealSuccesses.load();
    }
    return total;
}

bool DBTaskQueueSharded::stealFromNeighbor(size_t currentShardId, std::vector<Shard::TaskEnvelope>& outTasks, size_t maxTasks) {
    if (shardCount_ <= 1 || shards_.empty()) return false;  // no neighbours to steal from
    // Try to steal from neighboring shards in round-robin fashion
    // Start from next shard and work around the ring
    const size_t MAX_ATTEMPTS = 3; // Don't try all shards, just a few neighbors
    size_t stealedCount = 0;

    for (size_t attempt = 0; attempt < MAX_ATTEMPTS && stealedCount < maxTasks; ++attempt) {
        size_t targetShard = (currentShardId + attempt + 1) % shardCount_;
        auto& targetShard_ref = *shards_[targetShard];

        targetShard_ref.stealAttempts.fetch_add(1, std::memory_order_relaxed);

        // Non-blocking attempt to acquire lock
        std::unique_lock<std::mutex> lk(targetShard_ref.mu, std::try_to_lock);
        if (!lk.owns_lock()) {
            // Can't acquire lock - target shard is busy, skip
            continue;
        }

        // Try to steal one task from this shard
        for (size_t p = 0; p < 3 && stealedCount < maxTasks; ++p) {
            auto& buckets = targetShard_ref.queuesByPriority[p];
            if (buckets.empty()) continue;

            auto& colls = targetShard_ref.collectionOrder[p];
            if (colls.empty()) continue;

            // Steal from first available collection
            for (auto& coll : colls) {
                auto it = buckets.find(coll);
                if (it != buckets.end() && !it->second.empty()) {
                    outTasks.push_back(std::move(it->second.front()));
                    it->second.pop_front();
                    --targetShard_ref.totalQueued;
                    targetShard_ref.stealSuccesses.fetch_add(1, std::memory_order_relaxed);
                    ++stealedCount;

                    if (it->second.empty()) {
                        buckets.erase(it);
                    }
                    break;
                }
            }
            if (stealedCount >= maxTasks) break;
        }

        // Notify target shard in case it's waiting on empty queue after steal
        if (stealedCount > 0) {
            targetShard_ref.cv.notify_one();
        }
    }

    return stealedCount > 0;
}


void DBTaskQueueSharded::workerThread(size_t shardId) {
    if (shards_.empty()) return;
    if (shardId >= shards_.size()) shardId = 0;
    auto& shard = *shards_[shardId];
    const std::chrono::milliseconds CV_TIMEOUT{50};

    while (!shutdown_.load()) {
        std::vector<Shard::TaskEnvelope> batch;
        batch.reserve(32);

        // Acquire lock and dequeue
        {
            auto lockStart = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lk(shard.mu);
            auto lockAcquired = std::chrono::steady_clock::now();

            uint64_t waitUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(lockAcquired - lockStart).count()
            );
            shard.dequeueLockTotalUs.fetch_add(waitUs, std::memory_order_relaxed);
            shard.dequeueLockCount.fetch_add(1, std::memory_order_relaxed);

            // Wait for work (blocks thread, no CPU spin)
            if (shard.totalQueued == 0) {
                shard.cv.wait_for(lk, CV_TIMEOUT, [&]() {
                    return shard.totalQueued > 0 || shutdown_.load();
                });
                if (shutdown_.load()) break;
                if (shard.totalQueued == 0) continue;
            }

            // Dequeue batch (up to 32 tasks)
            size_t dequeueCount = 0;
            while (shard.totalQueued > 0 && batch.size() < 32 && dequeueCount < 8) {
                // Round-robin through priorities with quantum allocation
                const size_t batchSizeBefore = batch.size();
                for (size_t p = 0; p < 3 && batch.size() < 32; ++p) {
                    size_t idx = (shard.priorityCursor + p) % 3;

                    if (shard.priorityBudgetRemaining[idx] == 0) continue;

                    auto& buckets = shard.queuesByPriority[idx];
                    if (buckets.empty()) continue;

                    // Round-robin through collections at this priority
                    auto& colls = shard.collectionOrder[idx];
                    if (colls.empty()) continue;

                    size_t& rrIdx = shard.rrIndex[idx];
                    rrIdx = rrIdx % colls.size();

                    const size_t collIdx = rrIdx;
                    auto coll = colls[collIdx];
                    auto it = buckets.find(coll);
                    if (it != buckets.end() && !it->second.empty()) {
                        batch.push_back(std::move(it->second.front()));
                        it->second.pop_front();
                        --shard.totalQueued;
                        --shard.priorityBudgetRemaining[idx];

                        if (it->second.empty()) {
                            buckets.erase(it);
                            colls.erase(colls.begin() + static_cast<std::ptrdiff_t>(collIdx));
                            rrIdx = colls.empty() ? 0 : (collIdx % colls.size());
                        } else {
                            rrIdx = (collIdx + 1) % colls.size();
                        }
                        break;
                    }
                    ++rrIdx;
                }

                if (batch.size() == batchSizeBefore && shard.totalQueued > 0) {
                    bool queuedBehindDepletedBudget = false;
                    for (size_t p = 0; p < 3; ++p) {
                        if (shard.priorityBudgetRemaining[p] == 0 && !shard.queuesByPriority[p].empty()) {
                            queuedBehindDepletedBudget = true;
                            break;
                        }
                    }
                    if (queuedBehindDepletedBudget) {
                        shard.priorityBudgetRemaining = {{8, 4, 2}};
                        continue;
                    }
                }

                shard.priorityCursor = (shard.priorityCursor + 1) % 3;
                ++dequeueCount;
            }

            // Reset priority budgets if all depleted
            if (shard.priorityBudgetRemaining[0] == 0 &&
                shard.priorityBudgetRemaining[1] == 0 &&
                shard.priorityBudgetRemaining[2] == 0) {
                shard.priorityBudgetRemaining = {{8, 4, 2}};
            }

            // Wait if no work found - but first try work stealing
            if (batch.empty() && !shutdown_.load()) {
                // Release lock and try to steal from other shards
                lk.unlock();

                // Attempt work stealing
                stealFromNeighbor(shardId, batch, 4);

                // Re-acquire lock if we didn't find work via stealing
                if (batch.empty()) {
                    lk.lock();
                    // Double-check our own queue now that we've retried other shards
                    if (shard.totalQueued > 0) {
                        // Retry dequeue from own shard (batch should remain empty to re-enter dequeue logic)
                        // Actually, just let the loop continue naturally
                    } else {
                        // Still nothing - wait on condition variable
                        shard.cv.wait_for(lk, CV_TIMEOUT);
                    }
                }
            }
        } // Release lock before executing batch

        // Execute batch without holding lock
        shard.activeWorkers.fetch_add(1, std::memory_order_relaxed);
        for (auto& envelope : batch) {
            try {
                envelope.task();
            } catch (const std::exception& e) {
                std::cerr << "[DB-TASK-QUEUE-SHARDED] Task exception: " << e.what() << "\n";
            }
            shard.processed.fetch_add(1, std::memory_order_relaxed);
        }
        shard.activeWorkers.fetch_sub(1, std::memory_order_relaxed);
    }
}

static std::atomic<DBTaskQueueSharded*> g_shardedInstance{nullptr};
static std::once_flag g_shardedOnce;

DBTaskQueueSharded& DBTaskQueueSharded::instance() {
    DBTaskQueueSharded* inst = g_shardedInstance.load();
    if (inst == nullptr) {
        std::call_once(g_shardedOnce, []() {
            auto newInst = new DBTaskQueueSharded(
                g_cfgShards.load(),
                g_cfgWorkersPerShard.load(),
                g_cfgMaxQueuePerShard.load()
            );
            DBTaskQueueSharded* expected = nullptr;
            if (!g_shardedInstance.compare_exchange_strong(expected, newInst)) {
                delete newInst; // Another thread already initialized
            }
        });
        inst = g_shardedInstance.load();
    }
    return *inst;
}

void DBTaskQueueSharded::configure(size_t shards, size_t workersPerShard, size_t maxQueuePerShard) {
    std::call_once(g_cfgOnce, [shards, workersPerShard, maxQueuePerShard]() {
        g_cfgShards.store(std::max<size_t>(1, shards));
        g_cfgWorkersPerShard.store(std::max<size_t>(1, workersPerShard));
        g_cfgMaxQueuePerShard.store(std::max<size_t>(1, maxQueuePerShard));
        std::cout << "[DB-TASK-QUEUE-SHARDED] Configured shards=" << g_cfgShards.load()
                  << " workers_per_shard=" << g_cfgWorkersPerShard.load()
                  << " max_queue_per_shard=" << g_cfgMaxQueuePerShard.load() << "\n";
    });
}
