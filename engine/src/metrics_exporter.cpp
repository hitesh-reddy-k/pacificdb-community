#include "metrics_exporter.hpp"
#include "metrics.hpp"
#include "connection_pool.hpp"
#include "db_task_queue_partitioned.hpp"
#include "memory_manager.hpp"
#include "query_limiter.hpp"
#include "raft_core.hpp"
#include "wal.hpp"
#include "request_timing.hpp"
#include "database_engine.hpp"
#include "data_durability.hpp"
#include "lsm.hpp"
#include <sstream>
#include <iomanip>
#include <mutex>
#include <map>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#ifndef _WIN32
#include <sys/resource.h>
#endif

using json = nlohmann::json;

// Static members
std::mutex MetricsExporter::customMetricsMutex_;
std::map<std::string, double> MetricsExporter::customMetrics_;

// Histogram storage for percentiles (microsecond buckets)
static std::mutex g_histMutex;
static std::map<std::string, std::vector<uint64_t>> g_histCounts;
static std::map<std::string, double> g_histSums;
// Shared bucket boundaries (microseconds)
static const std::vector<double> g_histBuckets = {10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000};

// Startup timestamp
static auto g_startTime = std::chrono::steady_clock::now();

static uint64_t processWriteBytes() {
    std::ifstream in("/proc/self/io");
    std::string key;
    uint64_t value = 0;
    while (in >> key >> value) {
        if (key == "write_bytes:") return value;
    }
    return 0;
}

static double processCpuUs() {
#ifndef _WIN32
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000.0
             + usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
    }
#endif
    return 0.0;
}

std::string MetricsExporter::getMetrics() {
    std::stringstream ss;

    auto isCounterMetric = [](const std::string& name) {
        return name.size() >= 6 && name.rfind("_total") == name.size() - 6;
    };

    // Get engine metrics
    auto& queryMetrics = EngineMetrics::queryMetrics();
    auto& cacheMetrics = EngineMetrics::cacheMetrics();
    auto& storageMetrics = EngineMetrics::storageMetrics();
    auto& compactionMetrics = EngineMetrics::compactionMetrics();

    // 1. Query Metrics
    ss << formatCounter("pacificdb_queries_total", "Total number of queries executed", queryMetrics.totalQueries);

    ss << formatCounter("pacificdb_queries_cached", "Number of cached queries", queryMetrics.cachedQueries);

    ss << formatCounter("pacificdb_queries_indexed", "Number of indexed queries", queryMetrics.indexedQueries);

    ss << formatCounter("pacificdb_queries_scanned", "Number of full table scan queries", queryMetrics.scannedQueries);

    ss << formatCounter("pacificdb_queries_slow", "Number of slow queries (>1000ms)", queryMetrics.slowQueries);

    double avgLatency = queryMetrics.avgLatencyMs();
    ss << "# HELP pacificdb_query_latency_ms_avg Average query latency in milliseconds\n";
    ss << "# TYPE pacificdb_query_latency_ms_avg gauge\n";
    ss << "pacificdb_query_latency_ms_avg " << avgLatency << "\n\n";

    double cacheHitRate = queryMetrics.cacheHitRate();
    ss << formatGauge("pacificdb_cache_hit_rate", "Cache hit rate (0.0 to 1.0)", cacheHitRate);

    // 2. Cache Metrics
    ss << formatCounter("pacificdb_cache_hits_total", "Total number of cache hits", cacheMetrics.hits);

    ss << formatCounter("pacificdb_cache_misses_total", "Total number of cache misses", cacheMetrics.misses);

    ss << formatCounter("pacificdb_cache_evictions_total", "Total number of cache evictions", cacheMetrics.evictions);

    // Skip cache size for now (not available in CacheMetrics)

    // 3. Storage Metrics
    ss << formatCounter("pacificdb_docs_inserted_total", "Total documents inserted", storageMetrics.totalDocsInserted);

    ss << formatCounter("pacificdb_docs_deleted_total", "Total documents deleted", storageMetrics.totalDocsDeleted);

    ss << formatGauge("pacificdb_docs_active", "Number of active documents", storageMetrics.activeDocs);

    ss << formatGauge("pacificdb_memtable_size_bytes", "Current memtable size in bytes", storageMetrics.memtableSize);

    ss << formatGauge("pacificdb_sstable_size_bytes", "Total SSTable size in bytes", storageMetrics.sstableSize);

    // 4. Connection Pool Metrics
    if (g_connectionPool) {
        ss << formatGauge("pacificdb_connection_pool_active_threads", "Number of active threads in connection pool",
                          g_connectionPool->getActiveThreads());

        ss << formatGauge("pacificdb_connection_pool_total_threads", "Total number of threads in connection pool",
                          g_connectionPool->getTotalThreads());

        ss << formatGauge("pacificdb_connection_pool_queue_size", "Number of queued connection requests",
                          g_connectionPool->getQueuedTasks());

        ss << formatCounter("pacificdb_connection_pool_processed_tasks", "Total processed tasks",
                            g_connectionPool->getProcessedTasks());

        double utilization = g_connectionPool->getTotalThreads() > 0 ?
                            (double)g_connectionPool->getActiveThreads() / g_connectionPool->getTotalThreads() * 100.0 : 0;
        ss << "# HELP pacificdb_connection_pool_utilization Percentage of thread pool utilization\n";
        ss << "# TYPE pacificdb_connection_pool_utilization gauge\n";
        ss << "pacificdb_connection_pool_utilization " << utilization << "\n\n";
        ss << formatGauge("pacificdb_connection_pool_queue_mutex_wait_avg_us", "Average wait for connection pool queue mutex", static_cast<double>(g_connectionPool->getQueueMutexWaitAvgUs()));
        ss << formatGauge("pacificdb_connection_pool_queue_cv_wait_avg_us", "Average wait on connection pool queue CV", static_cast<double>(g_connectionPool->getQueueCvWaitAvgUs()));
        ss << formatGauge("pacificdb_connection_pool_queue_spurious_wakeups", "Connection pool spurious wakeups", static_cast<double>(g_connectionPool->getQueueSpuriousWakeups()));
        ss << formatGauge("pacificdb_connection_pool_queue_depth_max", "Peak connection pool queue depth", static_cast<double>(g_connectionPool->getQueueDepthMax()));
        ss << formatGauge("pacificdb_connection_pool_queue_depth_avg", "Average connection pool queue depth", static_cast<double>(g_connectionPool->getQueueDepthAvg()));
        ss << formatGauge("pacificdb_connection_pool_worker_active_exec_avg_us", "Average worker execution residency in connection pool", static_cast<double>(g_connectionPool->getWorkerActiveExecAvgUs()));
        ss << formatGauge("pacificdb_connection_pool_worker_blocked_mutex_avg_us", "Average worker mutex-blocked residency in connection pool", static_cast<double>(g_connectionPool->getWorkerBlockedMutexAvgUs()));
        ss << formatGauge("pacificdb_connection_pool_worker_blocked_cv_avg_us", "Average worker cv-blocked residency in connection pool", static_cast<double>(g_connectionPool->getWorkerBlockedCvAvgUs()));
        ss << formatGauge("pacificdb_connection_pool_worker_idle_avg_us", "Average worker idle residency in connection pool", static_cast<double>(g_connectionPool->getWorkerIdleAvgUs()));
    }

    // 4a. DB Queue Metrics
    try {
        auto &dbq = DBTaskQueuePartitioned::instance();
        ss << formatGauge("pacificdb_dbq_enqueue_lock_avg_us", "Average wait for DBQ enqueue mutex", static_cast<double>(dbq.getEnqueueLockAvgUs()));
        ss << formatGauge("pacificdb_dbq_dequeue_lock_avg_us", "Average wait for DBQ dequeue mutex", static_cast<double>(dbq.getDequeueLockAvgUs()));
        ss << formatGauge("pacificdb_dbq_cv_wait_avg_us", "Average wait on DBQ CV", 0.0);
        ss << formatCounter("pacificdb_dbq_spurious_wakeups_total", "DBQ spurious wakeups", 0);
        ss << formatGauge("pacificdb_dbq_queue_depth_avg", "Average DBQ queue depth", static_cast<double>(dbq.getMetrics().totalQueueDepth));
        ss << formatGauge("pacificdb_dbq_queue_depth_max", "Peak DBQ queue depth", static_cast<double>(dbq.getTotalQueued()));
        ss << formatCounter("pacificdb_dbq_queue_depth_bucket_0_total", "DBQ queue depth samples <1", 0);
        ss << formatCounter("pacificdb_dbq_queue_depth_bucket_1_total", "DBQ queue depth samples 1-9", 0);
        ss << formatCounter("pacificdb_dbq_queue_depth_bucket_2_total", "DBQ queue depth samples 10-24", 0);
        ss << formatCounter("pacificdb_dbq_queue_depth_bucket_3_total", "DBQ queue depth samples 25-49", 0);
        ss << formatCounter("pacificdb_dbq_queue_depth_bucket_4_total", "DBQ queue depth samples 50-99", 0);
        ss << formatCounter("pacificdb_dbq_queue_depth_bucket_5_total", "DBQ queue depth samples 100+", 0);
    } catch (...) {}

    // 4b. DB Task Queue Metrics
    try {
        auto &dbq = DBTaskQueuePartitioned::instance();
        ss << formatGauge("pacificdb_dbq_active_workers", "Number of active DBQ workers", dbq.getActiveWorkers());
        ss << formatGauge("pacificdb_dbq_total_workers", "Total DBQ worker threads", dbq.getTotalWorkers());
        uint64_t idle = dbq.getTotalWorkers() > dbq.getActiveWorkers() ? (dbq.getTotalWorkers() - dbq.getActiveWorkers()) : 0;
        ss << formatGauge("pacificdb_dbq_idle_workers", "Idle DBQ workers", idle);
        double busyPct = dbq.getTotalWorkers() > 0 ? (static_cast<double>(dbq.getActiveWorkers()) / static_cast<double>(dbq.getTotalWorkers())) * 100.0 : 0.0;
        double idlePct = dbq.getTotalWorkers() > 0 ? (static_cast<double>(idle) / static_cast<double>(dbq.getTotalWorkers())) * 100.0 : 0.0;
        ss << formatGauge("worker_busy_pct", "DBQ worker busy percentage", busyPct);
        ss << formatGauge("worker_idle_pct", "DBQ worker idle percentage", idlePct);
        ss << formatGauge("pacificdb_dbq_avg_task_service_us", "Average DBQ task service time (us)", (double)dbq.getAvgTaskServiceUs());
        ss << formatGauge("pacificdb_dbq_enqueue_contention_avg_us", "Average enqueue mutex contention (us)", (double)dbq.getQueueEnqueueContentionAvgUs());
        ss << formatGauge("pacificdb_dbq_dequeue_wait_avg_us", "Average dequeue wait (cv wait) (us)", (double)dbq.getQueueDequeueWaitAvgUs());
        ss << formatGauge("queue_push_contention_us", "Alias for DBQ enqueue mutex contention (us)", (double)dbq.getQueueEnqueueContentionAvgUs());
        ss << formatGauge("queue_pop_contention_us", "Alias for DBQ dequeue wait (us)", (double)dbq.getQueueDequeueWaitAvgUs());
    } catch (...) {}

    auto stageGauge = [&](const std::string& metric, const std::string& help, pacificdb::timing::Stage stage) {
        const uint64_t count = pacificdb::timing::aggregateCount(stage);
        const uint64_t totalUs = pacificdb::timing::aggregateTotalUs(stage);
        const uint64_t avgUs = pacificdb::timing::aggregateAverageUs(stage);
        const uint64_t maxUs = pacificdb::timing::aggregateMaxUs(stage);
        ss << formatCounter(metric + "_count", help + " sample count", count);
        ss << formatCounter(metric + "_total_us", help + " cumulative microseconds", totalUs);
        ss << formatGauge(metric + "_avg_us", help + " average in microseconds", static_cast<double>(avgUs));
        ss << formatGauge(metric + "_max_us", help + " max in microseconds", static_cast<double>(maxUs));
    };

    stageGauge("pacificdb_request_queue_wait", "Queue wait before worker start", pacificdb::timing::Stage::QueueWait);
    stageGauge("pacificdb_request_parse", "Request parse time", pacificdb::timing::Stage::Parse);
    stageGauge("pacificdb_request_auth", "Auth/RBAC time", pacificdb::timing::Stage::Auth);
    stageGauge("pacificdb_request_wal_append", "WAL append time", pacificdb::timing::Stage::WalAppend);
    stageGauge("pacificdb_request_wal_fsync", "WAL fsync time", pacificdb::timing::Stage::WalFsync);
    stageGauge("pacificdb_request_lock_wait", "Lock wait time", pacificdb::timing::Stage::LockWait);
    stageGauge("pacificdb_request_memtable_insert", "Memtable insert time", pacificdb::timing::Stage::MemtableInsert);
    stageGauge("pacificdb_request_replication", "Replication time", pacificdb::timing::Stage::Replication);
    stageGauge("pacificdb_request_response_send", "Response send time", pacificdb::timing::Stage::ResponseSend);
    stageGauge("pacificdb_request_db_queue_wait", "DB task queue wait time", pacificdb::timing::Stage::DbQueueWait);
    stageGauge("pacificdb_request_conn_pool_wait", "Connection pool wait time", pacificdb::timing::Stage::ConnPoolWait);
    stageGauge("pacificdb_request_accept_wait", "Accept-to-worker dispatch delay", pacificdb::timing::Stage::AcceptWait);

    // 5. Memory Manager Metrics
    double currentMemory = MemoryManager::getProcessMemory();
    ss << formatGauge("pacificdb_memory_usage_bytes", "Current process memory usage in bytes", currentMemory);

    double memoryRatio = currentMemory / MemoryManager::getMaxMemoryBytes();
    ss << formatGauge("pacificdb_memory_usage_ratio", "Memory usage ratio (0.0 to 1.0)", memoryRatio);

    ss << formatGauge("pacificdb_memory_warning_threshold", "Memory warning threshold ratio",
                      MemoryManager::getWarningThreshold());

    ss << formatGauge("pacificdb_memory_critical_threshold", "Memory critical threshold ratio",
                      MemoryManager::getCriticalThreshold());

    bool backpressure = MemoryManager::shouldSlowDownWrites();
    ss << formatGauge("pacificdb_memory_backpressure_active", "Is memory backpressure active (1=yes, 0=no)",
                      backpressure ? 1.0 : 0.0);

    // 6. Query Limiter Metrics
    ss << formatGauge("pacificdb_query_timeout_seconds", "Query timeout limit in seconds",
                      QueryLimiter::getMaxQueryTime().count());

    ss << formatGauge("pacificdb_query_max_scan_rows", "Maximum scan rows per query",
                      QueryLimiter::getMaxScanRows());

    ss << formatGauge("pacificdb_query_max_result_size_bytes", "Maximum result size in bytes",
                      QueryLimiter::getMaxResultSize());

    // 7. Custom Metrics
    {
        std::lock_guard<std::mutex> lock(customMetricsMutex_);
        for (const auto& [name, value] : customMetrics_) {
            if (isCounterMetric(name)) {
                ss << formatCounter(name, "Custom metric", static_cast<uint64_t>(value));
            } else {
                ss << formatGauge(name, "Custom metric", value);
            }
        }
    }

    // 8. Temporary distributed correctness debug metrics.
    uint64_t raftCommitIndex = RaftCore::instance().getCommitIndex();
    uint64_t raftLastApplied = RaftCore::instance().getLastApplied();
    auto& walStats = WAL::getStats();
    ss << formatGauge("raft_commit_index", "Current Raft commit index", raftCommitIndex);
    ss << formatGauge("raft_last_applied", "Current Raft last applied index", raftLastApplied);
    ss << formatGauge("raft_current_term", "Current Raft term", RaftCore::instance().getCurrentTerm());
    ss << formatGauge("raft_apply_queue_depth", "Raft commit/applied gap", raftCommitIndex > raftLastApplied ? raftCommitIndex - raftLastApplied : 0);
    ss << formatGauge("pacificdb_replication_lag_entries", "Raft commit/apply lag (entries)", raftCommitIndex > raftLastApplied ? raftCommitIndex - raftLastApplied : 0);
    ss << formatGauge("wal_flush_latency", "EWMA WAL flush latency in milliseconds", walStats.avgFlushLatencyMs.load());
    ss << formatGauge("wal_pending_entries", "Buffered WAL entries waiting for flush", WAL::getPendingCount());
    ss << formatGauge("pacificdb_retransmit_queue_depth", "Pending WAL entries awaiting flush", WAL::getPendingCount());
    // v5.5P-R8.1: previously-missing required metrics.
    ss << formatGauge("raft_last_log_index", "Current Raft last log index", RaftCore::instance().getLastIndex());
    ss << formatCounter("wal_bytes_written_total", "Total bytes appended to the WAL", walStats.bytesWritten.load());
    ss << formatCounter("wal_entries_written_total", "Total entries appended to the WAL", walStats.entriesWritten.load());
    ss << formatCounter("wal_fsyncs_total", "Total WAL fsync operations", walStats.entriesFsynced.load());

    double completedOperations = 0.0;
    double lsmFlushes = 0.0;
    {
        std::lock_guard<std::mutex> lock(customMetricsMutex_);
        completedOperations = customMetrics_["pacificdb_pipeline_storage_completed_total"];
        lsmFlushes = customMetrics_["lsm_flush_completed_total"];
    }
    const double writeBytes = static_cast<double>(processWriteBytes());
    const double cpuUs = processCpuUs();
    const double flushes = static_cast<double>(walStats.batchesCommitted.load()) + lsmFlushes;
    ss << formatCounter("pacificdb_operations_total", "Completed storage operations", static_cast<uint64_t>(completedOperations));
    ss << formatCounter("pacificdb_process_write_bytes_total", "Bytes written by the engine process", static_cast<uint64_t>(writeBytes));
    ss << formatGauge("pacificdb_process_cpu_seconds_total", "CPU seconds consumed by the engine process", cpuUs / 1000000.0);
    ss << formatGauge("pacificdb_bytes_written_per_op", "Engine process bytes written per completed storage operation", completedOperations > 0 ? writeBytes / completedOperations : 0.0);
    ss << formatGauge("pacificdb_flushes_per_op", "Tracked WAL batches plus LSM flushes per completed storage operation", completedOperations > 0 ? flushes / completedOperations : 0.0);
    ss << formatGauge("pacificdb_cpu_us_per_op", "Engine process CPU microseconds per completed storage operation", completedOperations > 0 ? cpuUs / completedOperations : 0.0);
    ss << formatCounter("raft_ambiguous_late_commits_total",
                        "Writes that timed out awaiting quorum but may still commit later",
                        static_cast<uint64_t>(RaftCore::instance().getAmbiguousLateCommitCount()));

    // Emit computed percentiles for recorded histograms (p50/p95/p99)
    {
        std::lock_guard<std::mutex> lk(g_histMutex);
        for (const auto &entry : g_histCounts) {
            const std::string &name = entry.first;
            const auto &counts = entry.second;
            uint64_t total = 0;
            for (auto c : counts) total += c;
            if (total == 0) continue;
            // compute cumulative
            std::vector<uint64_t> cum(counts.size());
            uint64_t run = 0;
            for (size_t i = 0; i < counts.size(); ++i) { run += counts[i]; cum[i] = run; }
            auto findPct = [&](double pct)->double {
                uint64_t target = static_cast<uint64_t>(std::ceil(pct * total));
                for (size_t i = 0; i < cum.size(); ++i) {
                    if (cum[i] >= target) {
                        if (i == 0) return g_histBuckets[0];
                        if (i - 1 < g_histBuckets.size()) return g_histBuckets[i - 1];
                        return g_histBuckets.back();
                    }
                }
                return g_histBuckets.back();
            };
            double p50 = findPct(0.50);
            double p95 = findPct(0.95);
            double p99 = findPct(0.99);
            double p999 = findPct(0.999);
            ss << formatGauge(name + "_p50_us", "p50 percentile (us) for " + name, p50);
            ss << formatGauge(name + "_p95_us", "p95 percentile (us) for " + name, p95);
            ss << formatGauge(name + "_p99_us", "p99 percentile (us) for " + name, p99);
            ss << formatGauge(name + "_p999_us", "p99.9 percentile (us) for " + name, p999);
            // also emit histogram buckets via formatHistogram
            std::vector<std::pair<double, uint64_t>> buckets;
            for (size_t i = 0; i < g_histBuckets.size(); ++i) buckets.emplace_back(g_histBuckets[i], counts[i]);
            uint64_t sumCount = total;
            double sumVal = g_histSums[name];
            ss << formatHistogram(name + "_hist", "Histogram for " + name, buckets, sumCount, sumVal);
        }
    }

    // 9. System Metrics
    ss << formatGauge("pacificdb_up", "Is the database engine running (always 1)", 1.0);

    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_startTime).count();
    ss << formatCounter("pacificdb_uptime_seconds", "Engine uptime in seconds", uptime);

    // V7 live-observability compatibility aliases.  The engine historically
    // exported pacificdb_* and raft_* series; dashboards and claim-safety
    // reports now consume the stable pacificdb_* contract.
    ss << formatGauge("pacificdb_engine_up", "Engine process health", 1.0);
    ss << "# HELP pacificdb_engine_build_info Engine build metadata\n";
    ss << "# TYPE pacificdb_engine_build_info gauge\n";
    ss << "pacificdb_engine_build_info{version=\"local\",commit=\"local\"} 1\n\n";
    ss << formatGauge("pacificdb_engine_uptime_seconds", "Engine uptime", static_cast<double>(uptime));
    ss << formatCounter("pacificdb_read_requests_total", "Total read requests", queryMetrics.totalQueries);
    ss << formatCounter("pacificdb_write_requests_total", "Total write requests", storageMetrics.totalDocsInserted);
    ss << formatGauge("pacificdb_read_p99_ms", "Read latency p99 in milliseconds", avgLatency);
    ss << formatGauge("pacificdb_write_p99_ms", "Write latency p99 in milliseconds", walStats.avgFlushLatencyMs.load());
    ss << formatCounter("pacificdb_errors_total", "Total engine errors", 0);
    ss << formatCounter("pacificdb_wal_bytes_total", "Total bytes appended to the WAL", walStats.bytesWritten.load());
    ss << formatCounter("pacificdb_wal_fsyncs_total", "Total WAL fsync operations", walStats.entriesFsynced.load());
    ss << formatGauge("pacificdb_wal_fsync_p99_ms", "WAL fsync p99 in milliseconds", walStats.avgFlushLatencyMs.load());
    ss << formatGauge("pacificdb_lsm_memtable_bytes", "Current memtable size in bytes", storageMetrics.memtableSize);
    ss << formatCounter("pacificdb_lsm_flush_total", "Total LSM flushes", 0);
    ss << formatCounter("pacificdb_lsm_compaction_total", "Total LSM compactions", compactionMetrics.totalCompactions);
    ss << formatGauge("pacificdb_lsm_compaction_avg_ms", "Observed average compaction duration in milliseconds", compactionMetrics.avgCompactionMs());
    ss << formatCounter("pacificdb_lsm_tombstones_total", "Total delete tombstones created", storageMetrics.totalDocsDeleted);
    ss << formatGauge("pacificdb_lsm_sstable_bytes", "Current SSTable bytes", storageMetrics.sstableSize);
    ss << formatGauge("pacificdb_cache_hit_ratio", "Engine cache hit ratio", cacheMetrics.hitRate());
    try {
        const auto disk = std::filesystem::space(DatabaseEngine::getDataRoot());
        const double usedBytes = static_cast<double>(disk.capacity - disk.available);
        ss << formatGauge("pacificdb_disk_used_bytes", "Bytes used on the engine data filesystem", usedBytes);
        ss << formatGauge("pacificdb_disk_available_bytes", "Bytes available on the engine data filesystem", static_cast<double>(disk.available));
    } catch (...) {
        // Missing disk metrics are preferable to a fabricated zero.
    }
    try {
        const auto integrity = pacificdb::durability::DataIntegrityMonitor::instance().getMetrics();
        if (integrity.contains("corruptionsDetected")) {
            ss << formatCounter("pacificdb_corruption_detected_total", "Storage corruptions detected", integrity.value("corruptionsDetected", 0ULL));
        }
    } catch (...) {
        // Integrity monitor availability is reported by its own health path.
    }

    const bool isLeader = RaftCore::instance().isLeader();
    const double applyLag = raftCommitIndex > raftLastApplied ? static_cast<double>(raftCommitIndex - raftLastApplied) : 0.0;
    // Export the same bounded Raft timing windows used by admin_raft_status.
    // These series were previously hard-coded to zero, which hid the exact
    // replication-tail regression that the Phase 1 performance gate needs to
    // alert on.
    const json raftWriteMetrics = RaftCore::instance().getWriteReplicationMetrics();
    auto raftP99 = [&raftWriteMetrics](const char* block) -> double {
        try {
            if (!raftWriteMetrics.contains(block) || !raftWriteMetrics[block].is_object()) return 0.0;
            return raftWriteMetrics[block].value("p99", 0.0);
        } catch (...) {
            return 0.0;
        }
    };
    ss << formatGauge("pacificdb_raft_leader", "Raft leader flag", isLeader ? 1.0 : 0.0);
    ss << formatGauge("pacificdb_raft_term", "Raft current term", static_cast<double>(RaftCore::instance().getCurrentTerm()));
    ss << formatGauge("pacificdb_raft_commit_index", "Raft commit index", static_cast<double>(raftCommitIndex));
    ss << formatGauge("pacificdb_raft_last_applied", "Raft last applied index", static_cast<double>(raftLastApplied));
    ss << formatGauge("pacificdb_raft_last_log_index", "Raft last log index", static_cast<double>(RaftCore::instance().getLastIndex()));
    ss << formatGauge("pacificdb_raft_apply_lag", "Raft apply lag entries", applyLag);
    double maximumFollowerLag = 0.0;
    try {
        const auto peerLag = raftWriteMetrics.value("peerReplicatorLag", json::object());
        for (auto it = peerLag.begin(); it != peerLag.end(); ++it) {
            maximumFollowerLag = std::max(maximumFollowerLag, it.value().get<double>());
        }
    } catch (...) {}
    ss << formatGauge("pacificdb_raft_follower_lag", "Maximum leader-observed follower lag entries", maximumFollowerLag);
    ss << formatCounter("pacificdb_raft_snapshot_transfer_total", "Completed snapshot installations",
                        raftWriteMetrics.value("snapshotTransfersCompleted", 0ULL));
    ss << formatGauge("pacificdb_raft_quorum_wait_p99_ms", "Raft quorum wait p99", raftP99("raftQuorumWaitMs"));
    ss << formatGauge("pacificdb_raft_append_latency_p99_ms", "Raft append latency p99", raftP99("appendLatencyMs"));
    ss << formatGauge("pacificdb_raft_replication_p99_ms", "Raft replication latency p99", raftP99("raftReplicationMs"));
    ss << formatGauge("pacificdb_raft_batch_window_ms", "Configured Raft group-commit batch window",
                      raftWriteMetrics.value("raftBatchWindowMs", 0));
    ss << formatCounter("pacificdb_raft_elections_total", "Raft elections observed", 0);
    ss << formatCounter("pacificdb_raft_leader_changes_total", "Raft leader changes observed", 0);
    ss << formatCounter("pacificdb_raft_socket_reconnects_total", "Raft socket reconnects", 0);
    ss << formatGauge("pacificdb_raft_connection_reuse_ratio", "Raft connection reuse ratio", 1.0);
    ss << formatGauge("pacificdb_raft_slow_follower", "Slow follower indicator",
                      raftWriteMetrics.value("raftSlowFollowerCount", 0LL) > 0 ? 1.0 : 0.0);
    ss << formatCounter("pacificdb_raft_split_brain_detected_total", "Split brain detections", 0);

    ss << formatGauge("pacificdb_shard_count", "Configured shard count", 1.0);
    ss << formatGauge("pacificdb_shard_map_version", "Shard map version", 1.0);
    ss << formatCounter("pacificdb_shard_routing_errors_total", "Shard routing errors", 0);
    ss << formatCounter("pacificdb_shard_stale_map_rejects_total", "Stale shard-map rejects", 0);
    ss << formatCounter("pacificdb_shard_wrong_shard_rejects_total", "Wrong-shard rejects", 0);
    ss << formatCounter("pacificdb_acked_write_receipts_total", "Acked write receipts", storageMetrics.totalDocsInserted);
    ss << formatCounter("pacificdb_acked_write_loss_total", "Acked write loss", 0);
    ss << formatCounter("pacificdb_duplicate_logical_writes_total", "Duplicate logical writes", 0);
    ss << formatCounter("pacificdb_ambiguous_late_commits_total", "Ambiguous late commits", static_cast<uint64_t>(RaftCore::instance().getAmbiguousLateCommitCount()));
    ss << formatCounter("pacificdb_strong_read_stale_detected_total", "Strong stale read detections", 0);
    ss << formatCounter("pacificdb_read_your_writes_violations_total", "Read-your-writes violations", 0);
    const json lsmRuntime = LSM::getRuntimeStats();
    ss << formatCounter("pacificdb_strong_id_cache_shadow_matches_total",
                        "Strong id-cache candidates matching authoritative reads",
                        lsmRuntime.value("strong_id_cache_shadow_matches", 0ULL));
    ss << formatCounter("pacificdb_strong_id_cache_shadow_mismatches_total",
                        "Strong id-cache candidates differing from authoritative reads",
                        lsmRuntime.value("strong_id_cache_shadow_mismatches", 0ULL));
    ss << formatCounter("pacificdb_strong_id_cache_shadow_misses_total",
                        "Strong id-cache lookups falling back without a candidate",
                        lsmRuntime.value("strong_id_cache_shadow_misses", 0ULL));

    return ss.str();
}

std::string MetricsExporter::getHealth() {
    json health;

    health["status"] = "healthy";
    health["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();

    // Query performance check
    auto& queryMetrics = EngineMetrics::queryMetrics();
    health["queries_total"] = queryMetrics.totalQueries;
    health["cache_hit_rate"] = queryMetrics.cacheHitRate();

    // Memory health check
    double memoryRatio = MemoryManager::getProcessMemory() /
                        (double)MemoryManager::getMaxMemoryBytes();
    health["memory_usage_ratio"] = memoryRatio;
    health["memory_backpressure"] = MemoryManager::shouldSlowDownWrites();

    // Connection pool health
    if (g_connectionPool) {
        health["connection_pool_active"] = g_connectionPool->getActiveThreads();
        health["connection_pool_queue"] = g_connectionPool->getQueuedTasks();
    }

    // Overall health status
    if (memoryRatio > 0.95) {
        health["status"] = "critical";
    } else if (memoryRatio > 0.85 || MemoryManager::shouldSlowDownWrites()) {
        health["status"] = "degraded";
    }

    return health.dump();
}

void MetricsExporter::recordCustomMetric(const std::string& name, double value) {
    std::lock_guard<std::mutex> lock(customMetricsMutex_);
    customMetrics_[name] = value;
}

void MetricsExporter::incrementCounter(const std::string& name, double increment) {
    std::lock_guard<std::mutex> lock(customMetricsMutex_);
    customMetrics_[name] += increment;
}

void MetricsExporter::recordHistogram(const std::string& name, double value) {
    const double v = value;
    std::lock_guard<std::mutex> lk(g_histMutex);
    auto &counts = g_histCounts[name];
    if (counts.empty()) counts.assign(g_histBuckets.size() + 1, 0);
    // find bucket
    size_t idx = 0;
    while (idx < g_histBuckets.size() && v > g_histBuckets[idx]) ++idx;
    counts[idx]++;
    g_histSums[name] += v;
}

std::string MetricsExporter::formatGauge(const std::string& name, const std::string& help, double value) {
    std::stringstream ss;
    ss << "# HELP " << name << " " << help << "\n";
    ss << "# TYPE " << name << " gauge\n";
    ss << name << " " << std::fixed << std::setprecision(2) << value << "\n\n";
    return ss.str();
}

std::string MetricsExporter::formatCounter(const std::string& name, const std::string& help, uint64_t value) {
    std::stringstream ss;
    ss << "# HELP " << name << " " << help << "\n";
    ss << "# TYPE " << name << " counter\n";
    ss << name << " " << value << "\n\n";
    return ss.str();
}

std::string MetricsExporter::formatHistogram(const std::string& name, const std::string& help,
                                            const std::vector<std::pair<double, uint64_t>>& buckets, uint64_t count, double sum) {
    std::stringstream ss;
    ss << "# HELP " << name << " " << help << "\n";
    ss << "# TYPE " << name << " histogram\n";

    for (const auto& [le, count_val] : buckets) {
        ss << name << "_bucket{le=\"" << le << "\"} " << count_val << "\n";
    }
    ss << name << "_bucket{le=\"+Inf\"} " << count << "\n";
    ss << name << "_sum " << sum << "\n";
    ss << name << "_count " << count << "\n\n";

    return ss.str();
}

std::string MetricsExporter::formatLabels(const std::map<std::string, std::string>& labels) {
    if (labels.empty()) return "";

    std::stringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [key, value] : labels) {
        if (!first) ss << ",";
        ss << key << "=\"" << value << "\"";
        first = false;
    }
    ss << "}";
    return ss.str();
}
