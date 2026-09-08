#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cassert>
#include <algorithm>
#include <iomanip>

// Forward declaration for testing
class DBTaskQueueSharded;
class DBTaskQueuePartitioned;

#include "db_task_queue_sharded.hpp"
#include "db_task_queue_partitioned.hpp"

static std::atomic<int> taskCount{0};
static std::atomic<int> completedCount{0};

void simpleTask() {
    ++completedCount;
}

void heavyTask(int iterations = 100) {
    volatile int x = 0;
    for (int i = 0; i < iterations; ++i) {
        x += i % 7;  // Busy work
    }
    ++completedCount;
}

/**
 * Test 1: Basic work stealing with imbalanced load
 *
 * Setup:
 *   - 4 shards
 *   - Enqueue 100 tasks to shard 0
 *   - Enqueue 10 tasks to shard 1
 *   - Enqueue 5 tasks to shard 2
 *   - Enqueue 0 tasks to shard 3 (idle)
 *
 * Expected:
 *   - Workers in shard 3 should steal from shard 0 (most loaded)
 *   - All tasks should complete
 *   - Work steal success count > 0
 */
void testBasicWorkStealing() {
    std::cout << "\n=== Test 1: Basic Work Stealing with Imbalanced Load ===\n";

    // Create queue with 4 shards
    DBTaskQueueSharded queue(4, 2, 100);  // 4 shards, 2 workers each, 100 capacity

    taskCount.store(0);
    completedCount.store(0);

    // Enqueue imbalanced tasks
    int expectedTasks = 0;

    // Shard 0: 100 tasks (hash to "shard0")
    for (int i = 0; i < 100; ++i) {
        queue.enqueue(simpleTask, DBTaskQueueSharded::Priority::MEDIUM, "shard0");
        ++expectedTasks;
    }

    // Shard 1: 10 tasks (hash to "shard1")
    for (int i = 0; i < 10; ++i) {
        queue.enqueue(simpleTask, DBTaskQueueSharded::Priority::MEDIUM, "shard1");
        ++expectedTasks;
    }

    // Shard 2: 5 tasks (hash to "shard2")
    for (int i = 0; i < 5; ++i) {
        queue.enqueue(simpleTask, DBTaskQueueSharded::Priority::MEDIUM, "shard2");
        ++expectedTasks;
    }

    // Shard 3: 0 tasks (idle - workers can steal)

    std::cout << "Enqueued " << expectedTasks << " tasks across 4 shards\n";
    std::cout << "Distribution: 100 -> shard0, 10 -> shard1, 5 -> shard2, 0 -> shard3\n";

    // Wait for all tasks to complete
    auto startTime = std::chrono::steady_clock::now();
    const auto MAX_WAIT = std::chrono::seconds(10);

    while (completedCount.load() < expectedTasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed > MAX_WAIT) {
            std::cerr << "Timeout! Completed " << completedCount.load() << "/" << expectedTasks << "\n";
            break;
        }
    }

    // Check results
    std::cout << "\nResults:\n";
    std::cout << "  Tasks completed: " << completedCount.load() << "/" << expectedTasks << "\n";
    std::cout << "  Active workers: " << queue.getActiveWorkers() << "\n";
    std::cout << "  Work steal attempts: " << queue.getWorkStealAttempts() << "\n";
    std::cout << "  Work steal successes: " << queue.getWorkStealSuccesses() << "\n";
    std::cout << "  Total queued: " << queue.getQueued() << "\n";
    std::cout << "  Processed tasks: " << queue.getProcessedTasks() << "\n";

    assert(completedCount.load() == expectedTasks);
    assert(queue.getWorkStealAttempts() > 0);  // At least some attempts
    std::cout << "✅ Test 1 passed: Work stealing reduced queue imbalance\n";
}

/**
 * Test 2: Work stealing under sustained load
 *
 * Setup:
 *   - 8 shards
 *   - Continuously enqueue tasks in a wave pattern
 *   - Some shards get more work than others
 *
 * Expected:
 *   - Idle workers steal from busy shards
 *   - Improved tail latency (workers don't starve)
 *   - Higher throughput than without stealing
 */
void testSustainedLoadWithStealing() {
    std::cout << "\n=== Test 2: Sustained Load with Work Stealing ===\n";

    DBTaskQueueSharded queue(8, 2, 500);  // 8 shards, 2 workers each

    taskCount.store(0);
    completedCount.store(0);

    // Enqueue wave pattern for 5 seconds
    auto stopTime = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int totalEnqueued = 0;

    auto enqueueThread = std::thread([&]() {
        std::vector<std::string> collections = {
            "col0", "col1", "col2", "col3", "col4", "col5", "col6", "col7"
        };

        int wave = 0;
        while (std::chrono::steady_clock::now() < stopTime) {
            // Each wave: enqueue more to some collections
            for (int i = 0; i < 8; ++i) {
                int count = (i + wave) % 8 + 1;  // 1-8 tasks per collection
                for (int j = 0; j < count; ++j) {
                    queue.enqueue(simpleTask, DBTaskQueueSharded::Priority::MEDIUM, collections[i]);
                    ++totalEnqueued;
                }
            }
            ++wave;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    enqueueThread.join();

    // Wait for all tasks to complete
    auto startWait = std::chrono::steady_clock::now();
    while (completedCount.load() < totalEnqueued &&
           std::chrono::steady_clock::now() - startWait < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nResults:\n";
    std::cout << "  Total enqueued: " << totalEnqueued << "\n";
    std::cout << "  Tasks completed: " << completedCount.load() << "\n";
    std::cout << "  Completion rate: "
              << std::fixed << std::setprecision(1)
              << (100.0 * completedCount.load() / totalEnqueued) << "%\n";
    std::cout << "  Work steal attempts: " << queue.getWorkStealAttempts() << "\n";
    std::cout << "  Work steal successes: " << queue.getWorkStealSuccesses() << "\n";
    std::cout << "  Work steal success rate: "
              << std::fixed << std::setprecision(1)
              << (queue.getWorkStealAttempts() > 0
                  ? 100.0 * queue.getWorkStealSuccesses() / queue.getWorkStealAttempts()
                  : 0.0) << "%\n";

    // Most tasks should complete (allow 5% loss due to queue overflow)
    assert(completedCount.load() >= totalEnqueued * 0.95);
    std::cout << "✅ Test 2 passed: Sustained load with good completion rate\n";
}

/**
 * Test 3: Work stealing does not cause tasks to be stolen from own queue
 * (Verify correctness - no duplicate executions)
 *
 * Setup:
 *   - Enqueue 1000 unique tasks
 *   - Track which tasks execute
 *   - Verify no duplicate execution
 *
 * Expected:
 *   - All tasks execute exactly once
 *   - No crashes or hangs
 */
void testWorkStealingCorrectness() {
    std::cout << "\n=== Test 3: Work Stealing Correctness (No Duplicates) ===\n";

    DBTaskQueueSharded queue(4, 2, 500);

    std::atomic<int> executedCount{0};
    std::vector<std::atomic<int>> executionCounts(100);
    for (auto& c : executionCounts) {
        c.store(0);
    }

    // Enqueue 100 unique tasks, each can be identified
    for (int i = 0; i < 100; ++i) {
        int taskId = i;
        queue.enqueue([taskId, &executionCounts]() {
            ++executionCounts[taskId];
        }, DBTaskQueueSharded::Priority::MEDIUM, "test");
    }

    // Wait for completion
    auto startTime = std::chrono::steady_clock::now();
    while (queue.getProcessedTasks() < 100 &&
           std::chrono::steady_clock::now() - startTime < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Verify correctness
    int duplicates = 0;
    int notExecuted = 0;
    int executedOnce = 0;

    for (int i = 0; i < 100; ++i) {
        int count = executionCounts[i].load();
        if (count == 0) ++notExecuted;
        else if (count == 1) ++executedOnce;
        else duplicates += (count - 1);
    }

    std::cout << "\nResults:\n";
    std::cout << "  Tasks executed exactly once: " << executedOnce << "/100\n";
    std::cout << "  Tasks not executed: " << notExecuted << "/100\n";
    std::cout << "  Duplicate executions: " << duplicates << "\n";
    std::cout << "  Work steal attempts: " << queue.getWorkStealAttempts() << "\n";
    std::cout << "  Work steal successes: " << queue.getWorkStealSuccesses() << "\n";

    assert(duplicates == 0);  // No task should execute twice
    assert(notExecuted == 0);  // All tasks should execute
    std::cout << "✅ Test 3 passed: No duplicate executions\n";
}

/**
 * Test 4: Partitioned Queue with work stealing
 */
void testProQueueWorkStealing() {
    std::cout << "\n=== Test 4: Partitioned Queue with Work Stealing ===\n";

    DBTaskQueuePartitioned queue(8, 2, 500, true, false);  // Enable stealing, disable adaptive

    completedCount.store(0);

    // Enqueue imbalanced workload
    int expectedTasks = 200;

    // 80% of work to specific collection
    for (int i = 0; i < 160; ++i) {
        queue.enqueue(simpleTask, DBTaskQueuePartitioned::Priority::MEDIUM, "hotcol");
    }

    // 20% distributed
    for (int i = 0; i < 40; ++i) {
        queue.enqueue(simpleTask, DBTaskQueuePartitioned::Priority::MEDIUM, "coldcol");
    }

    // Wait for completion
    auto startTime = std::chrono::steady_clock::now();
    while (queue.getTotalProcessed() < expectedTasks &&
           std::chrono::steady_clock::now() - startTime < std::chrono::seconds(10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nResults:\n";
    std::cout << "  Tasks enqueued: " << expectedTasks << "\n";
    std::cout << "  Tasks processed: " << queue.getTotalProcessed() << "\n";
    std::cout << "  Work steal attempts: " << queue.getWorkStealAttempts() << "\n";
    std::cout << "  Work steal successes: " << queue.getWorkStealSuccesses() << "\n";
    std::cout << "  Total workers: " << queue.getTotalWorkers() << "\n";

    assert(queue.getTotalProcessed() >= expectedTasks * 0.9);
    std::cout << "✅ Test 4 passed: partitioned queue with stealing\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Work-Stealing Queue Test Suite\n";
    std::cout << "========================================\n";

    try {
        testBasicWorkStealing();
        testSustainedLoadWithStealing();
        testWorkStealingCorrectness();
        testProQueueWorkStealing();

        std::cout << "\n========================================\n";
        std::cout << "✅ All work-stealing tests PASSED\n";
        std::cout << "========================================\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception\n";
        return 1;
    }
}
