#include "db_task_queue_partitioned.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <functional>

namespace {
std::atomic<size_t> g_cfgShards{32};
std::atomic<size_t> g_cfgWorkersPerShard{4};
std::atomic<size_t> g_cfgMaxQueuePerShard{5000};
std::once_flag g_partitionedCfgOnce;
}

DBTaskQueuePartitioned::DBTaskQueuePartitioned(
    size_t shards,
    size_t workersPerShard,
    size_t maxQueuePerShard,
    bool enableWorkStealing,
    bool enableAdaptiveBalancing)
        : shardCount_(std::max<size_t>(1, shards)),
            workersPerShard_(std::max<size_t>(1, workersPerShard)),
            maxQueuePerShard_(std::max<size_t>(1, maxQueuePerShard)),
      enableWorkStealing_(enableWorkStealing),
            enableAdaptiveBalancing_(enableAdaptiveBalancing),
            lastMetricsSampleTime_(std::chrono::steady_clock::now()) {

    // Initialize shards
    for (size_t i = 0; i < shardCount_; ++i) {
        auto shardState = std::make_unique<ShardState>();
        shardState->queue = std::make_unique<DBTaskQueueSharded>(
            1,  // Single shard per ShardState
            workersPerShard_,
            maxQueuePerShard_
        );
        shardState->latencyHistogram.resize(10000, 0);  // 10ms buckets up to 100s
        shards_.push_back(std::move(shardState));
    }

    // Start background threads for metrics and rebalancing
    metricsThread_ = std::thread(&DBTaskQueuePartitioned::metricsLoop, this);
    if (enableAdaptiveBalancing_) {
        balanceThread_ = std::thread(&DBTaskQueuePartitioned::balanceLoop, this);
    }

    std::cout << "[DB-TASK-QUEUE-PARTITIONED] Started with " << shardCount_ << " shards, "
              << workersPerShard << " workers per shard, "
              << maxQueuePerShard << " max queue per shard\n";
}

DBTaskQueuePartitioned::~DBTaskQueuePartitioned() {
    shutdown();
}

void DBTaskQueuePartitioned::shutdown() {
    if (shutdown_.exchange(true)) return;
    lifecycleCv_.notify_all();
    if (metricsThread_.joinable()) metricsThread_.join();
    if (balanceThread_.joinable()) balanceThread_.join();

    uint64_t totalProcessed = 0;
    for (auto& shard : shards_) {
        totalProcessed += shard->tasksProcessed.load();
    }
    shards_.clear();
    std::cout << "[DB-TASK-QUEUE-PARTITIONED] Shutdown complete, processed=" << totalProcessed << "\n";
}

size_t DBTaskQueuePartitioned::selectShardAdaptive(
    const std::string& collection,
    const std::string& hint) {
    const size_t effectiveShardCount = std::min(shardCount_, shards_.size());
    if (effectiveShardCount == 0) return 0;

    // If hint provided, hash it to a shard
    if (!hint.empty()) {
        std::hash<std::string> hasher;
        return hasher(hint) % effectiveShardCount;
    }

    // Use collection name for consistent routing
    if (!collection.empty()) {
        std::hash<std::string> hasher;
        size_t base = hasher(collection) % effectiveShardCount;

        // Check if shard is hot; if so, try next one
        if (shards_[base]->isHot.load()) {
            for (size_t i = 1; i < effectiveShardCount; ++i) {
                size_t candidate = (base + i) % effectiveShardCount;
                if (!shards_[candidate]->isHot.load()) {
                    return candidate;
                }
            }
        }
        return base;
    }

    // Fall back to round-robin
    return selectShardRoundRobin("");
}

size_t DBTaskQueuePartitioned::selectShardRoundRobin(const std::string&) {
    const size_t effectiveShardCount = std::min(shardCount_, shards_.size());
    if (effectiveShardCount == 0) return 0;
    return distributionRoundRobin_.fetch_add(1, std::memory_order_relaxed) % effectiveShardCount;
}

bool DBTaskQueuePartitioned::enqueue(
    Task task,
    Priority priority,
    const std::string& collection,
    size_t costHint,
    const std::string& shardHint) {
    const size_t effectiveShardCount = std::min(shardCount_, shards_.size());
    if (effectiveShardCount == 0) return false;

    size_t shardId = selectShardAdaptive(collection, shardHint);
    if (shardId >= effectiveShardCount) shardId = 0;
    auto& shard = *shards_[shardId];

    // Convert Priority enum to match DBTaskQueueSharded.
    // The partitioned queue has 4 levels (CRITICAL=0, HIGH=1, MEDIUM=2, LOW=3); the underlying sharded
    // queue has only 3 (HIGH=0, MEDIUM=1, LOW=2). A raw static_cast mapped Pro LOW(3)
    // to the invalid sharded value 3, indexing past the size-3 priority arrays
    // (undefined behaviour → SIGFPE). Map explicitly and saturate into range.
    DBTaskQueueSharded::Priority dbqPriority;
    switch (priority) {
        case Priority::CRITICAL:
        case Priority::HIGH:     dbqPriority = DBTaskQueueSharded::Priority::HIGH;   break;
        case Priority::MEDIUM:   dbqPriority = DBTaskQueueSharded::Priority::MEDIUM; break;
        case Priority::LOW:
        default:                 dbqPriority = DBTaskQueueSharded::Priority::LOW;    break;
    }

    // Try to enqueue on selected shard
    if (shard.queue->enqueue(task, dbqPriority, collection, costHint)) {
        shard.tasksProcessed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // If primary shard full and work stealing enabled, try others
    if (enableWorkStealing_) {
        for (size_t i = 1; i < effectiveShardCount; ++i) {
            size_t tryId = (shardId + i) % effectiveShardCount;
            auto& tryShard = *shards_[tryId];
            if (tryShard.queue->enqueue(task, dbqPriority, collection, costHint)) {
                shard.tasksRejected.fetch_add(1, std::memory_order_relaxed);
                tryShard.tasksProcessed.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
    }

    shard.tasksRejected.fetch_add(1, std::memory_order_relaxed);
    return false;
}

DBTaskQueuePartitioned::SystemMetrics DBTaskQueuePartitioned::getMetrics() const {
    SystemMetrics metrics;
    metrics.shardMetrics.resize(shardCount_);

    for (size_t i = 0; i < shardCount_; ++i) {
        auto sm = getShardMetrics(i);
        metrics.shardMetrics[i] = sm;
        metrics.globalTasksProcessed += sm.tasksProcessed;
        metrics.globalTasksRejected += sm.tasksRejected;
        metrics.totalQueueDepth += sm.currentDepth;
        metrics.globalP99LatencyMs = std::max(metrics.globalP99LatencyMs, sm.avgP99LatencyMs);
    }

    {
        std::lock_guard<std::mutex> lk(metricsMu_);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMetricsSampleTime_).count();
        if (elapsed > 0) {
            uint64_t delta = metrics.globalTasksProcessed - lastMetricsProcessed_;
            metrics.globalThroughputRps = (double)delta / ((double)elapsed / 1000.0);
            lastMetricsSampleTime_ = now;
            lastMetricsProcessed_ = metrics.globalTasksProcessed;
        }
    }

    return metrics;
}

DBTaskQueuePartitioned::ShardMetrics DBTaskQueuePartitioned::getShardMetrics(size_t shardId) const {
    if (shardId >= shardCount_) return ShardMetrics{};

    auto& shard = *shards_[shardId];
    std::lock_guard<std::mutex> lk(shard.mu);

    ShardMetrics m;
    m.tasksProcessed = shard.tasksProcessed.load();
    m.tasksRejected = shard.tasksRejected.load();
    m.totalWaitUs = shard.totalWaitUs.load();
    m.totalExecUs = shard.totalExecUs.load();
    m.lockContentionUs = shard.totalWaitUs.load();
    m.currentDepth = shard.queue->getQueued();
    m.utilizationPct = shard.utilizationPct.load();

    // Calculate p99 from histogram
    if (!shard.latencyHistogram.empty()) {
        uint64_t total = std::accumulate(shard.latencyHistogram.begin(), shard.latencyHistogram.end(), 0ULL);
        uint64_t target = (total * 99) / 100;
        uint64_t sum = 0;
        for (size_t i = 0; i < shard.latencyHistogram.size(); ++i) {
            sum += shard.latencyHistogram[i];
            if (sum >= target) {
                m.avgP99LatencyMs = (double)i / 100.0;  // Convert bucket index to ms
                break;
            }
        }
    }

    return m;
}

void DBTaskQueuePartitioned::setAdaptiveBalancing(bool enabled, double targetUtilization) {
    enableAdaptiveBalancing_ = enabled;
    if (targetUtilization < 0.1) targetUtilization = 0.1;
    if (targetUtilization > 0.99) targetUtilization = 0.99;
    targetUtilization_.store(targetUtilization, std::memory_order_relaxed);
}

void DBTaskQueuePartitioned::tuneShardCapacity(size_t shardId, size_t newMaxQueue) {
    if (shardId < shardCount_) {
        maxQueuePerShard_ = newMaxQueue;
        // TODO: dynamically resize shard queues
    }
}

size_t DBTaskQueuePartitioned::getTotalQueued() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->queue->getQueued();
    }
    return total;
}

size_t DBTaskQueuePartitioned::getTotalProcessed() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->tasksProcessed.load();
    }
    return total;
}

size_t DBTaskQueuePartitioned::getTotalWorkers() const {
    return shardCount_ * workersPerShard_;
}

size_t DBTaskQueuePartitioned::getActiveWorkers() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->queue->getActiveWorkers();
    }
    return total;
}

uint64_t DBTaskQueuePartitioned::getEnqueueLockAvgUs() const {
    uint64_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->queue->getEnqueueLockAvgUs();
    }
    return shardCount_ == 0 ? 0 : total / shardCount_;
}

uint64_t DBTaskQueuePartitioned::getDequeueLockAvgUs() const {
    uint64_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->queue->getDequeueLockAvgUs();
    }
    return shardCount_ == 0 ? 0 : total / shardCount_;
}

uint64_t DBTaskQueuePartitioned::getAvgTaskServiceUs() const {
    return 0;
}

uint64_t DBTaskQueuePartitioned::getQueueEnqueueContentionAvgUs() const {
    return getEnqueueLockAvgUs();
}

uint64_t DBTaskQueuePartitioned::getQueueDequeueWaitAvgUs() const {
    return getDequeueLockAvgUs();
}

uint64_t DBTaskQueuePartitioned::getWorkStealAttempts() const {
    uint64_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->queue->getWorkStealAttempts();
    }
    return total;
}

uint64_t DBTaskQueuePartitioned::getWorkStealSuccesses() const {
    uint64_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->queue->getWorkStealSuccesses();
    }
    return total;
}

size_t DBTaskQueuePartitioned::getTotalRejected() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->tasksRejected.load();
    }
    return total;
}

void DBTaskQueuePartitioned::metricsLoop() {
    while (!shutdown_.load()) {
        {
            std::unique_lock<std::mutex> lock(lifecycleMutex_);
            lifecycleCv_.wait_for(lock, std::chrono::seconds(1), [this]() {
                return shutdown_.load(std::memory_order_acquire);
            });
        }
        if (shutdown_) break;

        // Update shard metrics periodically
        for (size_t i = 0; i < shardCount_; ++i) {
            updateShardMetrics(i);
        }
    }
}

void DBTaskQueuePartitioned::balanceLoop() {
    while (!shutdown_.load()) {
        {
            std::unique_lock<std::mutex> lock(lifecycleMutex_);
            lifecycleCv_.wait_for(lock, std::chrono::seconds(5), [this]() {
                return shutdown_.load(std::memory_order_acquire);
            });
        }
        if (shutdown_) break;

        if (enableAdaptiveBalancing_) {
            updateLoadBalance();
        }
    }
}

void DBTaskQueuePartitioned::updateShardMetrics(size_t shardId) {
    if (shardId >= shardCount_) return;

    auto& shard = *shards_[shardId];
    size_t queued = shard.queue->getQueued();
    shard.currentDepth.store(queued, std::memory_order_relaxed);

    // Mark as hot if queue depth > 50% capacity
    bool isHot = queued > (maxQueuePerShard_ / 2);
    shard.isHot.store(isHot, std::memory_order_relaxed);

    // Compute utilization
    if (maxQueuePerShard_ > 0) {
        double util = (double)queued / (double)maxQueuePerShard_ * 100.0;
        shard.utilizationPct.store(util, std::memory_order_relaxed);
    }

    const double target = targetUtilization_.load(std::memory_order_relaxed) * 100.0;
    shard.isHot.store(shard.utilizationPct.load(std::memory_order_relaxed) >= target, std::memory_order_relaxed);
}

void DBTaskQueuePartitioned::updateLoadBalance() {
    // Find hot and cold shards
    std::vector<size_t> hotShards, coldShards;
    for (size_t i = 0; i < shardCount_; ++i) {
        auto& shard = *shards_[i];
        size_t depth = shard.currentDepth.load();
        if (depth > (maxQueuePerShard_ * 3 / 4)) {
            hotShards.push_back(i);
        } else if (depth < (maxQueuePerShard_ / 4)) {
            coldShards.push_back(i);
        }
    }

    // TODO: Implement work stealing between hot and cold shards
    // This would involve draining tasks from cold shards and requeuing to hot ones
    (void)hotShards;
    (void)coldShards;
}

static std::atomic<DBTaskQueuePartitioned*> g_partitionedInstance{nullptr};

DBTaskQueuePartitioned& DBTaskQueuePartitioned::instance() {
    DBTaskQueuePartitioned* inst = g_partitionedInstance.load();
    if (inst) return *inst;

    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        auto newInst = new DBTaskQueuePartitioned(
            g_cfgShards.load(),
            g_cfgWorkersPerShard.load(),
            g_cfgMaxQueuePerShard.load()
        );
        DBTaskQueuePartitioned* expected = nullptr;
        g_partitionedInstance.compare_exchange_strong(expected, newInst);
    });

    return *g_partitionedInstance.load();
}

void DBTaskQueuePartitioned::configure(size_t shards, size_t workersPerShard, size_t maxQueuePerShard) {
    // If already initialized, log warning and exit
    if (g_partitionedInstance.load()) {
        std::cerr << "[DB-TASK-QUEUE-PARTITIONED] Warning: configure called after initialization\n";
        return;
    }
    std::call_once(g_partitionedCfgOnce, [shards, workersPerShard, maxQueuePerShard]() {
        g_cfgShards.store(std::max<size_t>(1, shards));
        g_cfgWorkersPerShard.store(std::max<size_t>(1, workersPerShard));
        g_cfgMaxQueuePerShard.store(std::max<size_t>(1, maxQueuePerShard));
        std::cout << "[DB-TASK-QUEUE-PARTITIONED] Configured shards=" << g_cfgShards.load()
                  << " workers_per_shard=" << g_cfgWorkersPerShard.load()
                  << " max_queue_per_shard=" << g_cfgMaxQueuePerShard.load() << "\n";
    });
}

void DBTaskQueuePartitioned::shutdownInstance() {
    if (auto* instance = g_partitionedInstance.load(std::memory_order_acquire)) {
        instance->shutdown();
    }
}
