#pragma once

#include <cstddef>
#include <iostream>
#include <chrono>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>

class QueryLimiter {
public:
    // Configuration (load from env)
    static void init();

    // Timeout enforcement
    static void startQuery(const std::string& queryId);
    static void checkTimeout(const std::string& queryId);
    static void endQuery(const std::string& queryId);

    // Memory enforcement
    static void checkResultSize(const std::vector<nlohmann::json>& results);
    static void checkMemoryUsage();

    // Scan limit enforcement
    static void checkScanLimit(size_t rowsScanned);

    // Reporting helpers (non-throwing) for legacy call sites
    static void reportScanLimitReached(size_t rowsScanned, size_t limit);
    static void reportResultLimitReached(size_t resultCount, size_t limit);

    // Get current limits (for testing/debugging)
    static std::chrono::seconds getMaxQueryTime() { return maxQueryTime_; }
    static size_t getMaxResultSize() { return maxResultSize_; }
    static size_t getMaxScanRows() { return maxScanRows_; }
    static size_t getMaxResultDocs() { return maxResultDocs_; }

    // Legacy compatibility
    static size_t getMaxResultBytes() { return maxResultSize_; }
    static void setMaxScanRows(size_t v) { maxScanRows_ = v; }
    static void setMaxResultDocs(size_t v) { maxResultDocs_ = v; }
    static void setMaxResultBytes(size_t v) { maxResultSize_ = v; }
    static void markTruncated(const std::string& reason);
    static bool consumeTruncatedFlag(std::string& outReason);

private:
    static std::chrono::seconds maxQueryTime_;    // Default: 30s
    static size_t maxResultSize_;                  // Default: 100MB
    static size_t maxScanRows_;                    // Default: 1M rows
    static size_t maxResultDocs_;                  // Default: 10k docs

    struct QueryContext {
        std::chrono::steady_clock::time_point startTime;
        size_t rowsScanned;
        size_t memoryUsed;
    };

    static std::unordered_map<std::string, QueryContext> activeQueries_;
    static std::mutex queryMutex_;

    // Truncation state (shared)
    static bool truncated_;
    static std::string truncatedReason_;
    static std::mutex truncMutex_;
};
