#include "db_task_queue_partitioned.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

static void waitForCount(std::atomic<size_t>& counter, size_t target) {
    for (int i = 0; i < 200; ++i) {
        if (counter.load(std::memory_order_relaxed) >= target) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static void waitForDrain(DBTaskQueuePartitioned& queue) {
    for (int i = 0; i < 500; ++i) {
        if (queue.getTotalQueued() == 0 && queue.getActiveWorkers() == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main() {
    DBTaskQueuePartitioned::configure(4, 2, 64);
    auto& queue = DBTaskQueuePartitioned::instance();

    std::atomic<size_t> processed{0};
    const size_t taskCount = 2;

    for (size_t i = 0; i < taskCount; ++i) {
        bool accepted = queue.enqueue(
            [&processed]() {
                processed.fetch_add(1, std::memory_order_relaxed);
            },
            DBTaskQueuePartitioned::Priority::MEDIUM,
            "test_collection",
            1,
            "force_same_shard"
        );
        assert(accepted);
    }

    waitForCount(processed, taskCount);
    waitForDrain(queue);

    assert(processed.load(std::memory_order_relaxed) == taskCount);
    assert(queue.getTotalProcessed() >= taskCount);
    assert(queue.getTotalRejected() <= taskCount);
    assert(queue.getTotalWorkers() == 8);
    assert(queue.getMetrics().shardMetrics.size() == 4);

    std::cout << "✅ db_task_queue_partitioned test passed\n";
    return 0;
}
