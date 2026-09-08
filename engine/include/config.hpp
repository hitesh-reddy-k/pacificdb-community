#pragma once

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @brief Configuration management for the database engine
 *
 * This module provides centralized configuration management with:
 * - Environment-based overrides (dev/staging/prod)
 * - Runtime parameter tuning
 * - Default values for all subsystems
 * - Thread-safe access
 */
class EngineConfig {
public:
    // ==================== MEMTABLE SETTINGS ====================
    static constexpr size_t MEMTABLE_LIMIT = 1024;           // docs before flush
    static constexpr size_t MEMTABLE_SIZE_BYTES = 256 * 1024 * 1024;  // 256MB

    // ==================== SST & COMPACTION ====================
    static constexpr size_t SST_THRESHOLD = 10;              // SSTs before compact
    static constexpr size_t LEVEL_AMPLIFICATION = 10;        // size ratio between levels
    static constexpr int MAX_COMPACTION_THREADS = 4;

    // ==================== INDEX SETTINGS ====================
    static constexpr size_t INDEX_SYNC_INTERVAL = 1000;      // ms between index updates
    static constexpr bool INDEX_LAZY_BUILD = true;           // build on-demand vs immediately
    static constexpr int BLOOM_BITS_PER_KEY = 10;            // bloom filter precision

    // ==================== QUERY PERFORMANCE ====================
    static constexpr size_t QUERY_CACHE_SIZE = 10000;        // max cached queries
    static constexpr uint32_t QUERY_CACHE_TTL_MS = 300000;   // 5 minutes
    static constexpr int QUERY_TIMEOUT_MS = 30000;           // 30 second timeout

    // ==================== VECTOR OPERATIONS ====================
    static constexpr int VECTOR_SIMILARITY_TOP_K = 100;      // default k for queries
    static constexpr int VECTOR_BATCH_SIZE = 1000;           // batch processing size

    // ==================== STORAGE & WAL ====================
    static constexpr size_t WAL_BUFFER_SIZE = 8 * 1024 * 1024;  // 8MB
    static constexpr bool WAL_FSYNC_ENABLED = true;          // durability vs perf
    static constexpr uint32_t WAL_ROTATION_INTERVAL_MS = 3600000;  // 1 hour

    // ==================== BACKGROUND TASKS ====================
    static constexpr uint32_t COMPACTION_INTERVAL_MS = 60000;    // 1 minute
    static constexpr uint32_t MAINTENANCE_INTERVAL_MS = 300000;  // 5 minutes
    static constexpr uint32_t CACHE_CLEANUP_INTERVAL_MS = 60000; // 1 minute

    // ==================== MEDIA STORAGE ====================
    static constexpr size_t MAX_MEDIA_SIZE_BYTES = 1024 * 1024 * 1024;  // 1GB
    static constexpr const char* MEDIA_CHUNK_SIZE_BYTES = "4194304";    // 4MB chunks

    // ==================== TRANSACTION SETTINGS ====================
    static constexpr uint32_t TXN_LOCK_TIMEOUT_MS = 5000;    // 5 second lock timeout
    static constexpr int TXN_ISOLATION_LEVEL = 2;             // REPEATABLE_READ

    // ==================== LOGGING & MONITORING ====================
    static constexpr bool VERBOSE_LOGGING = false;           // can override via env
    static constexpr bool METRICS_ENABLED = true;
    static constexpr uint32_t METRICS_INTERVAL_MS = 60000;   // 1 minute

    /**
     * @brief Load configuration from environment variables
     * Supports: ENGINE_MEMTABLE_LIMIT, ENGINE_QUERY_CACHE_TTL, ENGINE_VERBOSE, etc.
     */
    static void loadFromEnvironment();

    /**
     * @brief Validate configuration consistency and constraints
     * @return true if config is valid, false otherwise
     */
    static bool validate();

    /**
     * @brief Get current configuration as JSON for inspection/monitoring
     */
    static json toJson();

    /**
     * @brief Reset to defaults
     */
    static void reset();

private:
    static class ConfigImpl {
    public:
        size_t memtableLimit = MEMTABLE_LIMIT;
        size_t memtableSizeBytes = MEMTABLE_SIZE_BYTES;
        size_t sstThreshold = SST_THRESHOLD;
        size_t queryCacheSize = QUERY_CACHE_SIZE;
        uint32_t queryCacheTtlMs = QUERY_CACHE_TTL_MS;
        bool verboseLogging = VERBOSE_LOGGING;
        bool metricsEnabled = METRICS_ENABLED;
        int maxCompactionThreads = MAX_COMPACTION_THREADS;

        // Add runtime-adjustable settings here
    } impl;
};
