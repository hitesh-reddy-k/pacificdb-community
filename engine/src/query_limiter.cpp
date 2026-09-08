#include "../include/query_limiter.hpp"
#include <stdexcept>
#include <cstdlib>
#include <iostream>

// Initialize static members
std::chrono::seconds QueryLimiter::maxQueryTime_(30);
size_t QueryLimiter::maxResultSize_(100 * 1024 * 1024); // 100MB
size_t QueryLimiter::maxScanRows_(1000000); // 1M rows
size_t QueryLimiter::maxResultDocs_(10000);
std::unordered_map<std::string, QueryLimiter::QueryContext> QueryLimiter::activeQueries_;
std::mutex QueryLimiter::queryMutex_;
bool QueryLimiter::truncated_ = false;
std::string QueryLimiter::truncatedReason_;
std::mutex QueryLimiter::truncMutex_;

void QueryLimiter::init() {
    // Load from environment variables
    const char* queryTimeout = std::getenv("MAX_QUERY_TIMEOUT_SEC");
    if (queryTimeout) {
        maxQueryTime_ = std::chrono::seconds(std::atoi(queryTimeout));
    }

    const char* resultSize = std::getenv("MAX_RESULT_SIZE_MB");
    if (resultSize) {
        maxResultSize_ = std::atoi(resultSize) * 1024 * 1024;
    }

    const char* scanLimit = std::getenv("MAX_SCAN_ROWS");
    if (scanLimit) {
        maxScanRows_ = std::atoi(scanLimit);
    }

    // Max documents returned by a single read/dump. Default 10000. Raising this
    // (e.g. MAX_RESULT_DOCS=20000000) lets the admin_stable_payload_dump path
    // enumerate an entire collection so cross-replica convergence can be proven
    // for collections larger than the default cap (V8.5.6 gate closure).
    const char* resultDocs = std::getenv("MAX_RESULT_DOCS");
    if (resultDocs) {
        long long v = std::atoll(resultDocs);
        if (v > 0) maxResultDocs_ = static_cast<size_t>(v);
    }

    std::cout << "[LIMITER] Initialized - timeout=" << maxQueryTime_.count()
              << "s, maxResult=" << (maxResultSize_ / 1024 / 1024)
              << "MB, maxScan=" << maxScanRows_ << " rows"
              << ", maxResultDocs=" << maxResultDocs_ << std::endl;
}

void QueryLimiter::startQuery(const std::string& queryId) {
    std::lock_guard<std::mutex> lock(queryMutex_);
    activeQueries_[queryId] = {
        std::chrono::steady_clock::now(),
        0,
        0
    };
}

void QueryLimiter::checkTimeout(const std::string& queryId) {
    std::lock_guard<std::mutex> lock(queryMutex_);
    auto it = activeQueries_.find(queryId);
    if (it == activeQueries_.end()) return;

    auto elapsed = std::chrono::steady_clock::now() - it->second.startTime;
    if (elapsed > maxQueryTime_) {
        activeQueries_.erase(it);
        throw std::runtime_error("Query timeout exceeded: " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()) +
            "s > " + std::to_string(maxQueryTime_.count()) + "s");
    }
}

void QueryLimiter::endQuery(const std::string& queryId) {
    std::lock_guard<std::mutex> lock(queryMutex_);
    activeQueries_.erase(queryId);
}

void QueryLimiter::checkResultSize(const std::vector<nlohmann::json>& results) {
    size_t totalSize = 0;
    for (const auto& doc : results) {
        totalSize += doc.dump().size();
    }

    if (totalSize > maxResultSize_) {
        throw std::runtime_error("Result set too large: " +
            std::to_string(totalSize / 1024 / 1024) + "MB > " +
            std::to_string(maxResultSize_ / 1024 / 1024) + "MB");
    }
}

void QueryLimiter::checkMemoryUsage() {
    // This is a placeholder for memory usage checking
    // In production, you would check actual process memory here
    // For now, we just check the number of active queries
    std::lock_guard<std::mutex> lock(queryMutex_);
    if (activeQueries_.size() > 1000) {
        throw std::runtime_error("Too many concurrent queries: " +
            std::to_string(activeQueries_.size()));
    }
}

void QueryLimiter::checkScanLimit(size_t rowsScanned) {
    if (rowsScanned > maxScanRows_) {
        throw std::runtime_error("Scan limit exceeded: " +
            std::to_string(rowsScanned) + " > " +
            std::to_string(maxScanRows_));
    }
}

void QueryLimiter::reportScanLimitReached(size_t rowsScanned, size_t limit) {
    {
        std::lock_guard<std::mutex> lock(truncMutex_);
        truncated_ = true;
        truncatedReason_ = "scan_limit";
    }
    std::cerr << "[LIMITER] Scan limit reached: " << rowsScanned
              << " > " << limit << std::endl;
}

void QueryLimiter::reportResultLimitReached(size_t resultCount, size_t limit) {
    {
        std::lock_guard<std::mutex> lock(truncMutex_);
        truncated_ = true;
        truncatedReason_ = "result_limit";
    }
    std::cerr << "[LIMITER] Result limit reached: " << resultCount
              << " >= " << limit << std::endl;
}

void QueryLimiter::markTruncated(const std::string& reason) {
    std::lock_guard<std::mutex> lock(truncMutex_);
    truncated_ = true;
    truncatedReason_ = reason;
}

bool QueryLimiter::consumeTruncatedFlag(std::string& outReason) {
    std::lock_guard<std::mutex> lock(truncMutex_);
    if (!truncated_) return false;
    truncated_ = false;
    outReason = truncatedReason_;
    truncatedReason_.clear();
    return true;
}
