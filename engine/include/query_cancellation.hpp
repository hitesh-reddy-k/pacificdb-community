#pragma once

#include <memory>
#include <atomic>

#include <chrono>

// Simple cancellation token used by worker tasks to cooperatively abort long-running operations
struct CancellationToken {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::chrono::steady_clock::time_point deadline;
};

using CancellationTokenPtr = std::shared_ptr<CancellationToken>;

namespace QueryCancel {
    // Thread-local token for the currently executing request on this worker
    void setToken(const CancellationTokenPtr& t);
    void clearToken();
    CancellationTokenPtr getToken();
    bool isCancelled();
    CancellationTokenPtr makeToken();
}
