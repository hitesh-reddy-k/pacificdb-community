#pragma once

#include <string>
#include <functional>
#include <vector>
#include <nlohmann/json.hpp>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <chrono>

using json = nlohmann::json;

/**
 * @brief Connection pooling and resource management
 *
 * Handles:
 * - Reusable connection/resource pools
 * - Thread pool for background tasks
 * - Memory pool for allocations
 * - Graceful degradation under load
 */

class ConnectionPool {
public:
    /**
     * @brief Acquire a resource from the pool
     * Blocks if pool is exhausted and timeout is not reached
     * @param timeoutMs Maximum wait time in milliseconds
     * @return Resource handle, or nullptr if timeout
     */
    template<typename T>
    static std::shared_ptr<T> acquire(int timeoutMs = 5000);

    /**
     * @brief Release a resource back to the pool
     */
    template<typename T>
    static void release(std::shared_ptr<T> resource);

    /**
     * @brief Get current pool statistics
     */
    static json getPoolStats();

    /**
     * @brief Resize pool to new size
     */
    static void resizePool(size_t newSize);

    /**
     * @brief Drain and close all connections
     */
    static void shutdown();

private:
    static struct PoolImpl {
        std::vector<void*> available;
        std::vector<void*> inUse;
        size_t maxSize = 100;
        // Add lock for thread safety
    } impl;
};

/**
 * @brief Task scheduling and background work
 */
class TaskScheduler {
public:
    enum class Priority {
        LOW,
        NORMAL,
        HIGH,
        CRITICAL
    };

    using TaskFunction = std::function<void()>;

    /**
     * @brief Schedule a one-time task
     */
    static void scheduleOnce(
        TaskFunction task,
        uint32_t delayMs,
        Priority priority = Priority::NORMAL
    );

    /**
     * @brief Schedule a recurring task
     * @param task Function to execute
     * @param intervalMs Interval between executions
     * @param priority Task priority
     * @return Task ID for cancellation
     */
    static uint32_t scheduleRecurring(
        TaskFunction task,
        uint32_t intervalMs,
        Priority priority = Priority::NORMAL
    );

    /**
     * @brief Cancel a scheduled task
     */
    static void cancel(uint32_t taskId);

    /**
     * @brief Get scheduler statistics
     */
    static json getSchedulerStats();

    /**
     * @brief Shutdown scheduler gracefully
     */
    static void shutdown();

private:
    struct Task {
        uint32_t id;
        TaskFunction fn;
        uint32_t intervalMs;
        Priority priority;
        bool recurring;
    };

    static std::vector<Task> scheduledTasks;
    static uint32_t nextTaskId;
};

/**
 * @brief Memory management and allocation tracking
 */
class MemoryManager {
public:
    /**
     * @brief Allocate memory with tracking
     */
    static void* allocate(size_t size, const std::string& purpose = "");

    /**
     * @brief Deallocate tracked memory
     */
    static void deallocate(void* ptr);

    /**
     * @brief Get memory usage statistics
     */
    static json getMemoryStats();

    /**
     * @brief Get total allocated bytes
     */
    static size_t getTotalAllocated();

    /**
     * @brief Get memory limit (if set)
     */
    static size_t getMemoryLimit();

    /**
     * @brief Set memory limit for soft constraints
     */
    static void setMemoryLimit(size_t bytes);

    /**
     * @brief Force garbage collection
     */
    static void forceGC();

    /**
     * @brief Get allocation by purpose (for debugging)
     */
    static json getAllocationsByPurpose();

private:
    struct MemoryBlock {
        void* ptr;
        size_t size;
        std::string purpose;
        std::chrono::system_clock::time_point allocTime;
    };

    static std::vector<MemoryBlock> allocations;
    static size_t memoryLimit;
    static size_t totalAllocated;
};
