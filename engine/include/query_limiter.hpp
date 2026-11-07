#pragma once

#include <cstddef>
#include <iostream>
#include <chrono>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <functional>

class QueryLimiter {
public:
    // One state per request. Shared ownership supports queued work outliving a
    // socket timeout; attachment is scoped to the executing worker.
    struct RequestState {
        std::mutex mutex;
        bool truncated{false};
        std::string reason;
        std::atomic<size_t> scannedRows{0};
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> active;
        ~RequestState();
    };
    using StatePtr = std::shared_ptr<RequestState>;
    class Scope {
    public:
        explicit Scope(StatePtr state = {});
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        StatePtr previous_;
    };
    static StatePtr capture();
    static std::function<void()> bind(std::function<void()> task);

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
    static std::chrono::seconds getMaxQueryTime() { return std::chrono::seconds(maxQuerySeconds_.load()); }
    static size_t getMaxResultSize() { return maxResultSize_.load(); }
    static size_t getMaxScanRows() { return maxScanRows_.load(); }
    static size_t getMaxResultDocs() { return maxResultDocs_.load(); }

    // Legacy compatibility
    static size_t getMaxResultBytes() { return maxResultSize_.load(); }
    static void setMaxScanRows(size_t v) { maxScanRows_ = v; }
    static void setMaxResultDocs(size_t v) { maxResultDocs_ = v; }
    static void setMaxResultBytes(size_t v) { maxResultSize_ = v; }
    static void markTruncated(const std::string& reason);
    static bool consumeTruncatedFlag(std::string& outReason);

private:
    static std::atomic<long long> maxQuerySeconds_;
    static std::atomic<size_t> maxResultSize_;
    static std::atomic<size_t> maxScanRows_;
    static std::atomic<size_t> maxResultDocs_;
    static std::atomic<size_t> activeQueryCount_;
};
