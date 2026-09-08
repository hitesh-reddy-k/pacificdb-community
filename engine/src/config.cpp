#include "config.hpp"
#include <cstdlib>
#include <iostream>

// Static member initialization
EngineConfig::ConfigImpl EngineConfig::impl;

void EngineConfig::loadFromEnvironment() {
    // Load from env variables with prefix ENGINE_
    const char* val;

    // MEMTABLE settings
    if ((val = std::getenv("ENGINE_MEMTABLE_LIMIT")) != nullptr) {
        impl.memtableLimit = std::stoull(val);
    }

    if ((val = std::getenv("ENGINE_MEMTABLE_SIZE_BYTES")) != nullptr) {
        impl.memtableSizeBytes = std::stoull(val);
    }

    // QUERY CACHE settings
    if ((val = std::getenv("ENGINE_QUERY_CACHE_SIZE")) != nullptr) {
        impl.queryCacheSize = std::stoull(val);
    }

    if ((val = std::getenv("ENGINE_QUERY_CACHE_TTL_MS")) != nullptr) {
        impl.queryCacheTtlMs = std::stoul(val);
    }

    // LOGGING settings
    if ((val = std::getenv("ENGINE_VERBOSE")) != nullptr) {
        impl.verboseLogging = (std::string(val) == "1" || std::string(val) == "true");
    }

    // METRICS settings
    if ((val = std::getenv("ENGINE_METRICS_ENABLED")) != nullptr) {
        impl.metricsEnabled = (std::string(val) == "1" || std::string(val) == "true");
    }

    // COMPACTION settings
    if ((val = std::getenv("ENGINE_MAX_COMPACTION_THREADS")) != nullptr) {
        impl.maxCompactionThreads = std::stoi(val);
    }

    std::cout << "[CONFIG] Loaded from environment variables\n";
}

bool EngineConfig::validate() {
    // Validate constraints
    if (impl.memtableLimit < 100) {
        std::cerr << "[CONFIG] ERROR: memtableLimit must be >= 100\n";
        return false;
    }

    if (impl.queryCacheSize < 100) {
        std::cerr << "[CONFIG] ERROR: queryCacheSize must be >= 100\n";
        return false;
    }

    if (impl.maxCompactionThreads < 1 || impl.maxCompactionThreads > 64) {
        std::cerr << "[CONFIG] ERROR: maxCompactionThreads must be between 1-64\n";
        return false;
    }

    std::cout << "[CONFIG] Configuration validated successfully\n";
    return true;
}

json EngineConfig::toJson() {
    return {
        {"memtableLimit", impl.memtableLimit},
        {"memtableSizeBytes", impl.memtableSizeBytes},
        {"sstThreshold", impl.sstThreshold},
        {"queryCacheSize", impl.queryCacheSize},
        {"queryCacheTtlMs", impl.queryCacheTtlMs},
        {"maxCompactionThreads", impl.maxCompactionThreads},
        {"verboseLogging", impl.verboseLogging},
        {"metricsEnabled", impl.metricsEnabled}
    };
}

void EngineConfig::reset() {
    impl.memtableLimit = MEMTABLE_LIMIT;
    impl.memtableSizeBytes = MEMTABLE_SIZE_BYTES;
    impl.sstThreshold = SST_THRESHOLD;
    impl.queryCacheSize = QUERY_CACHE_SIZE;
    impl.queryCacheTtlMs = QUERY_CACHE_TTL_MS;
    impl.verboseLogging = VERBOSE_LOGGING;
    impl.metricsEnabled = METRICS_ENABLED;
    impl.maxCompactionThreads = MAX_COMPACTION_THREADS;
    std::cout << "[CONFIG] Reset to defaults\n";
}
