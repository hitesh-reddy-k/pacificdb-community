#include "../include/query_limiter.hpp"
#include <stdexcept>
#include <cstdlib>
#include <iostream>
#include "query_cancellation.hpp"

std::atomic<long long> QueryLimiter::maxQuerySeconds_{30};
std::atomic<size_t> QueryLimiter::maxResultSize_{100 * 1024 * 1024};
std::atomic<size_t> QueryLimiter::maxScanRows_{1000000};
std::atomic<size_t> QueryLimiter::maxResultDocs_{10000};
std::atomic<size_t> QueryLimiter::activeQueryCount_{0};
namespace {
thread_local QueryLimiter::StatePtr currentState;
thread_local unsigned scopeDepth = 0;
}

QueryLimiter::RequestState::~RequestState() {
    QueryLimiter::activeQueryCount_.fetch_sub(active.size(), std::memory_order_relaxed);
}
QueryLimiter::Scope::Scope(StatePtr state) : previous_(currentState) {
    currentState = state ? std::move(state) : std::make_shared<RequestState>();
    ++scopeDepth;
}
QueryLimiter::Scope::~Scope() { currentState = std::move(previous_); --scopeDepth; }
QueryLimiter::StatePtr QueryLimiter::capture() {
    if (!currentState) currentState = std::make_shared<RequestState>();
    return currentState;
}
std::function<void()> QueryLimiter::bind(std::function<void()> task) {
    auto state = scopeDepth ? capture() : std::make_shared<RequestState>();
    auto token = QueryCancel::getToken();
    return [state = std::move(state), token = std::move(token), task = std::move(task)]() mutable {
        Scope scope(state);
        QueryCancel::Scope cancellation(token);
        task();
    };
}

void QueryLimiter::init() {
    // Load from environment variables
    const char* queryTimeout = std::getenv("MAX_QUERY_TIMEOUT_SEC");
    if (queryTimeout) {
        maxQuerySeconds_ = std::atoll(queryTimeout);
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

    std::cout << "[LIMITER] Initialized - timeout=" << getMaxQueryTime().count()
              << "s, maxResult=" << (getMaxResultSize() / 1024 / 1024)
              << "MB, maxScan=" << getMaxScanRows() << " rows"
              << ", maxResultDocs=" << getMaxResultDocs() << std::endl;
}

void QueryLimiter::startQuery(const std::string& queryId) {
    auto state = capture();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active.emplace(queryId, std::chrono::steady_clock::now()).second)
        activeQueryCount_.fetch_add(1, std::memory_order_relaxed);
    else state->active[queryId] = std::chrono::steady_clock::now();
}
void QueryLimiter::checkTimeout(const std::string& queryId) {
    auto state = capture();
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->active.find(queryId);
    if (it == state->active.end()) return;
    const auto elapsed = std::chrono::steady_clock::now() - it->second;
    if (elapsed > getMaxQueryTime()) {
        state->active.erase(it);
        activeQueryCount_.fetch_sub(1, std::memory_order_relaxed);
        throw std::runtime_error("Query timeout exceeded: " +
            std::to_string(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()) +
            "s > " + std::to_string(getMaxQueryTime().count()) + "s");
    }
}
void QueryLimiter::endQuery(const std::string& queryId) {
    auto state = capture();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active.erase(queryId)) activeQueryCount_.fetch_sub(1, std::memory_order_relaxed);
}

void QueryLimiter::checkResultSize(const std::vector<nlohmann::json>& results) {
    size_t totalSize = 0;
    for (const auto& doc : results) {
        totalSize += doc.dump().size();
    }

    if (totalSize > maxResultSize_) {
        throw std::runtime_error("Result set too large: " +
            std::to_string(totalSize / 1024 / 1024) + "MB > " +
            std::to_string(getMaxResultSize() / 1024 / 1024) + "MB");
    }
}

void QueryLimiter::checkMemoryUsage() {
    // Preserve the existing aggregate admission limit; request state itself is
    // never process-global. This is not a complete process memory reservation.
    const auto active = activeQueryCount_.load(std::memory_order_relaxed);
    if (active > 1000) throw std::runtime_error("Too many concurrent queries: " + std::to_string(active));
}

void QueryLimiter::checkScanLimit(size_t rowsScanned) {
    if (rowsScanned > maxScanRows_) {
        throw std::runtime_error("Scan limit exceeded: " +
            std::to_string(rowsScanned) + " > " +
            std::to_string(getMaxScanRows()));
    }
}

void QueryLimiter::reportScanLimitReached(size_t rowsScanned, size_t limit) {
    markTruncated("scan_limit");
    std::cerr << "[LIMITER] Scan limit reached: " << rowsScanned << " > " << limit << std::endl;
}
void QueryLimiter::reportResultLimitReached(size_t resultCount, size_t limit) {
    markTruncated("result_limit");
    std::cerr << "[LIMITER] Result limit reached: " << resultCount << " >= " << limit << std::endl;
}
void QueryLimiter::markTruncated(const std::string& reason) {
    auto state = capture();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->truncated = true;
    state->reason = reason;
}
bool QueryLimiter::consumeTruncatedFlag(std::string& outReason) {
    auto state = capture();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->truncated) return false;
    state->truncated = false;
    outReason = std::move(state->reason);
    state->reason.clear();
    return true;
}
