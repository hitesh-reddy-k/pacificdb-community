#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include "connection_pool.hpp"
#include "query_cancellation.hpp"

int main() {
    std::cout << "[TIMEOUT-TEST] Initializing connection pool...\n";
    initConnectionPool(2, 2);

    std::atomic<bool> observedCancel{false};

    auto longTask = [&observedCancel]() {
        std::cout << "[TIMEOUT-TEST] Long task started on worker\n";
        for (int i = 0; i < 50; ++i) {
            if (QueryCancel::isCancelled()) {
                std::cout << "[TIMEOUT-TEST] Detected cancellation inside task at iteration " << i << "\n";
                observedCancel.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::cout << "[TIMEOUT-TEST] Long task exiting\n";
    };

    std::cout << "[TIMEOUT-TEST] Submitting long task with 1s timeout\n";
    bool ok = g_connectionPool->submit(longTask, std::chrono::milliseconds(1000));
    if (!ok) {
        std::cerr << "[TIMEOUT-TEST] Failed to submit task (queue full?)\n";
        destroyConnectionPool();
        return 2;
    }

    // Wait enough time for the watchdog to fire and the task to detect cancellation
    std::this_thread::sleep_for(std::chrono::seconds(4));

    if (observedCancel.load()) {
        std::cout << "[TIMEOUT-TEST] SUCCESS: Task observed cancellation as expected\n";
    } else {
        std::cerr << "[TIMEOUT-TEST] FAILURE: Task did not observe cancellation\n";
    }

    destroyConnectionPool();
    return observedCancel.load() ? 0 : 1;
}
