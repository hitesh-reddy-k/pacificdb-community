#include "metrics.hpp"
#include <nlohmann/json.hpp>
#include <mutex>
#include <iostream>
#include <ctime>

using json = nlohmann::json;

// Static member initialization
bool EngineMetrics::metricsEnabled = true;
time_t EngineMetrics::startTime_ = std::time(nullptr);
EngineMetrics::QueryMetrics EngineMetrics::queryMetrics_;
EngineMetrics::StorageMetrics EngineMetrics::storageMetrics_;
EngineMetrics::CompactionMetrics EngineMetrics::compactionMetrics_;
EngineMetrics::CacheMetrics EngineMetrics::cacheMetrics_;
EngineMetrics::BackgroundTaskMetrics EngineMetrics::backgroundMetrics_;

static std::mutex metricsMutex;

time_t EngineMetrics::startTime() {
    return startTime_;
}

void EngineMetrics::setStartTime(time_t t) {
    startTime_ = t;
}

EngineMetrics::QueryMetrics& EngineMetrics::queryMetrics() {
    return queryMetrics_;
}

EngineMetrics::StorageMetrics& EngineMetrics::storageMetrics() {
    return storageMetrics_;
}

EngineMetrics::CompactionMetrics& EngineMetrics::compactionMetrics() {
    return compactionMetrics_;
}

EngineMetrics::CacheMetrics& EngineMetrics::cacheMetrics() {
    return cacheMetrics_;
}

EngineMetrics::BackgroundTaskMetrics& EngineMetrics::backgroundMetrics() {
    return backgroundMetrics_;
}

void EngineMetrics::recordQuery(uint64_t latencyMs, bool cached, bool indexed) {
    if (!metricsEnabled) return;
    std::lock_guard<std::mutex> lock(metricsMutex);

    queryMetrics_.totalQueries++;
    queryMetrics_.totalMs += latencyMs;

    if (cached) queryMetrics_.cachedQueries++;
    if (indexed) queryMetrics_.indexedQueries++;
    if (!indexed) queryMetrics_.scannedQueries++;

    if (latencyMs > 1000) queryMetrics_.slowQueries++;
}

void EngineMetrics::recordInsert() {
    if (!metricsEnabled) return;
    std::lock_guard<std::mutex> lock(metricsMutex);
    storageMetrics_.totalDocsInserted++;
    storageMetrics_.activeDocs++;
}

void EngineMetrics::recordDelete() {
    if (!metricsEnabled) return;
    std::lock_guard<std::mutex> lock(metricsMutex);
    storageMetrics_.totalDocsDeleted++;
    if (storageMetrics_.activeDocs > 0) {
        storageMetrics_.activeDocs--;
    }
}

void EngineMetrics::recordCompaction(uint64_t bytesCompacted, uint64_t durationMs) {
    if (!metricsEnabled) return;
    std::lock_guard<std::mutex> lock(metricsMutex);

    compactionMetrics_.totalCompactions++;
    compactionMetrics_.bytesCompacted += bytesCompacted;
    compactionMetrics_.totalCompactionMs += durationMs;
}

void EngineMetrics::recordCacheHit() {
    if (!metricsEnabled) return;
    std::lock_guard<std::mutex> lock(metricsMutex);
    cacheMetrics_.hits++;
}

void EngineMetrics::recordCacheMiss() {
    if (!metricsEnabled) return;
    std::lock_guard<std::mutex> lock(metricsMutex);
    cacheMetrics_.misses++;
}

void EngineMetrics::recordCacheEviction() {
    if (!metricsEnabled) return;
    std::lock_guard<std::mutex> lock(metricsMutex);
    cacheMetrics_.evictions++;
}

std::string EngineMetrics::getMetricsJson() {
    std::lock_guard<std::mutex> lock(metricsMutex);

    json metrics = {
        {"query", {
            {"totalQueries", queryMetrics_.totalQueries},
            {"cachedQueries", queryMetrics_.cachedQueries},
            {"indexedQueries", queryMetrics_.indexedQueries},
            {"scannedQueries", queryMetrics_.scannedQueries},
            {"slowQueries", queryMetrics_.slowQueries},
            {"avgLatencyMs", queryMetrics_.avgLatencyMs()},
            {"cacheHitRate", queryMetrics_.cacheHitRate()}
        }},
        {"storage", {
            {"totalDocsInserted", storageMetrics_.totalDocsInserted},
            {"totalDocsDeleted", storageMetrics_.totalDocsDeleted},
            {"activeDocs", storageMetrics_.activeDocs},
            {"memtableSize", storageMetrics_.memtableSize},
            {"sstableSize", storageMetrics_.sstableSize},
            {"indexSize", storageMetrics_.indexSize}
        }},
        {"compaction", {
            {"totalCompactions", compactionMetrics_.totalCompactions},
            {"bytesCompacted", compactionMetrics_.bytesCompacted},
            {"avgCompactionMs", compactionMetrics_.avgCompactionMs()},
            {"compressionRatio", compactionMetrics_.compressionRatio}
        }},
        {"cache", {
            {"hits", cacheMetrics_.hits},
            {"misses", cacheMetrics_.misses},
            {"evictions", cacheMetrics_.evictions},
            {"hitRate", cacheMetrics_.hitRate()},
            {"currentSize", cacheMetrics_.currentSize},
            {"maxSize", cacheMetrics_.maxSize}
        }}
    };

    return metrics.dump();
}

void EngineMetrics::reset() {
    std::lock_guard<std::mutex> lock(metricsMutex);

    queryMetrics_ = QueryMetrics();
    storageMetrics_ = StorageMetrics();
    compactionMetrics_ = CompactionMetrics();
    cacheMetrics_ = CacheMetrics();
    backgroundMetrics_ = BackgroundTaskMetrics();

    std::cout << "[METRICS] Reset all metrics\n";
}

void EngineMetrics::setEnabled(bool enabled) {
    metricsEnabled = enabled;
    std::cout << "[METRICS] Metrics " << (enabled ? "ENABLED" : "DISABLED") << "\n";
}
