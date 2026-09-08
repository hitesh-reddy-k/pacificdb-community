#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <ostream>

/**
 * @brief Structured metrics and monitoring for the database engine
 *
 * Tracks:
 * - Query performance (latency, throughput)
 * - Storage efficiency (compression ratio, index size)
 * - Compaction activity and effectiveness
 * - Cache hit/miss rates
 * - Background task execution
 */
class EngineMetrics {
public:
    struct QueryMetrics {
        uint64_t totalQueries = 0;
        uint64_t cachedQueries = 0;
        uint64_t indexedQueries = 0;
        uint64_t scannedQueries = 0;
        uint64_t totalMs = 0;
        uint32_t slowQueries = 0;  // > 1000ms

        double avgLatencyMs() const {
            return totalQueries > 0 ? (double)totalMs / totalQueries : 0;
        }

        double cacheHitRate() const {
            return totalQueries > 0 ? (double)cachedQueries / totalQueries : 0;
        }
    };

    struct StorageMetrics {
        uint64_t totalDocsInserted = 0;
        uint64_t totalDocsDeleted = 0;
        uint64_t activeDocs = 0;
        uint64_t memtableSize = 0;
        uint64_t sstableSize = 0;
        uint64_t indexSize = 0;
        uint32_t numIndexes = 0;
        uint32_t bloomFilterCount = 0;
    };

    struct CompactionMetrics {
        uint64_t totalCompactions = 0;
        uint64_t bytesCompacted = 0;
        uint64_t totalCompactionMs = 0;
        uint32_t currentLevel = 0;
        float compressionRatio = 0.0f;

        double avgCompactionMs() const {
            return totalCompactions > 0 ? (double)totalCompactionMs / totalCompactions : 0;
        }
    };

    struct CacheMetrics {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        size_t currentSize = 0;
        size_t maxSize = 0;

        double hitRate() const {
            uint64_t total = hits + misses;
            return total > 0 ? (double)hits / total : 0;
        }
    };

    struct BackgroundTaskMetrics {
        uint32_t compactionThreadsActive = 0;
        uint64_t maintenanceTasksCompleted = 0;
        uint64_t totalMaintenanceMs = 0;
        std::chrono::system_clock::time_point lastFullCompaction;
    };

    // Global metric collectors
    static QueryMetrics& queryMetrics();
    static StorageMetrics& storageMetrics();
    static CompactionMetrics& compactionMetrics();
    static CacheMetrics& cacheMetrics();
    static BackgroundTaskMetrics& backgroundMetrics();

    /**
     * @brief Record a query execution
     */
    static void recordQuery(uint64_t latencyMs, bool cached, bool indexed);

    /**
     * @brief Record storage activity
     */
    static void recordInsert();
    static void recordDelete();

    /**
     * @brief Record compaction activity
     */
    static void recordCompaction(uint64_t bytesCompacted, uint64_t durationMs);

    /**
     * @brief Record cache activity
     */
    static void recordCacheHit();
    static void recordCacheMiss();
    static void recordCacheEviction();

    /**
     * @brief Get metrics snapshot as JSON for monitoring/alerts
     */
    static std::string getMetricsJson();

    /**
     * @brief Reset all metrics (use cautiously)
     */
    static void reset();

    /**
     * @brief Enable/disable metrics collection
     */
    static void setEnabled(bool enabled);

    /**
     * @brief Get/set engine start time for uptime calculation
     */
    static time_t startTime();
    static void setStartTime(time_t t);

private:
    static bool metricsEnabled;
    static time_t startTime_;
    static QueryMetrics queryMetrics_;
    static StorageMetrics storageMetrics_;
    static CompactionMetrics compactionMetrics_;
    static CacheMetrics cacheMetrics_;
    static BackgroundTaskMetrics backgroundMetrics_;
};
