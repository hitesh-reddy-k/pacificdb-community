#include "resource_pool.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

// ==================== TaskScheduler Implementation ====================

std::vector<TaskScheduler::Task> TaskScheduler::scheduledTasks;
uint32_t TaskScheduler::nextTaskId = 1;
static std::mutex schedulerMutex;

void TaskScheduler::scheduleOnce(
    TaskFunction task,
    uint32_t delayMs,
    Priority priority) {

    std::thread([task, delayMs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        try {
            task();
        } catch (const std::exception& e) {
            std::cerr << "[SCHEDULER] Task error: " << e.what() << "\n";
        }
    }).detach();
}

uint32_t TaskScheduler::scheduleRecurring(
    TaskFunction task,
    uint32_t intervalMs,
    Priority priority) {

    std::lock_guard<std::mutex> lock(schedulerMutex);
    uint32_t taskId = nextTaskId++;

    std::thread([task, intervalMs, taskId]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "[SCHEDULER] Task " << taskId << " error: " << e.what() << "\n";
            }
        }
    }).detach();

    std::cout << "[SCHEDULER] Scheduled recurring task " << taskId << " every " << intervalMs << "ms\n";
    return taskId;
}

void TaskScheduler::cancel(uint32_t taskId) {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    // Note: In practice, would need task handle to cancel
    std::cout << "[SCHEDULER] Cancel requested for task " << taskId << "\n";
}

json TaskScheduler::getSchedulerStats() {
    std::lock_guard<std::mutex> lock(schedulerMutex);
    return {
        {"scheduledTasks", scheduledTasks.size()},
        {"nextTaskId", nextTaskId}
    };
}

void TaskScheduler::shutdown() {
    std::cout << "[SCHEDULER] Shutdown initiated\n";
    // In practice, would signal all threads to stop
}

// ==================== MemoryManager Implementation ====================

std::vector<MemoryManager::MemoryBlock> MemoryManager::allocations;
size_t MemoryManager::memoryLimit = 0;
size_t MemoryManager::totalAllocated = 0;
static std::mutex memoryMutex;

void* MemoryManager::allocate(size_t size, const std::string& purpose) {
    std::lock_guard<std::mutex> lock(memoryMutex);

    if (memoryLimit > 0 && totalAllocated + size > memoryLimit) {
        std::cerr << "[MEMORY] Allocation rejected: would exceed limit\n";
        return nullptr;
    }

    void* ptr = std::malloc(size);
    if (!ptr) {
        std::cerr << "[MEMORY] Failed to allocate " << size << " bytes\n";
        return nullptr;
    }

    allocations.push_back({
        ptr,
        size,
        purpose,
        std::chrono::system_clock::now()
    });

    totalAllocated += size;

    if (purpose != "") {
        std::cout << "[MEMORY] Allocated " << size << " bytes for: " << purpose << "\n";
    }

    return ptr;
}

void MemoryManager::deallocate(void* ptr) {
    std::lock_guard<std::mutex> lock(memoryMutex);

    auto it = std::find_if(allocations.begin(), allocations.end(),
        [ptr](const MemoryBlock& b) { return b.ptr == ptr; });

    if (it != allocations.end()) {
        totalAllocated -= it->size;
        allocations.erase(it);
    }

    std::free(ptr);
}

json MemoryManager::getMemoryStats() {
    std::lock_guard<std::mutex> lock(memoryMutex);

    return {
        {"totalAllocated", totalAllocated},
        {"memoryLimit", memoryLimit},
        {"activeBlocks", allocations.size()},
        {"limitReached", (memoryLimit > 0 && totalAllocated >= memoryLimit)}
    };
}

size_t MemoryManager::getTotalAllocated() {
    std::lock_guard<std::mutex> lock(memoryMutex);
    return totalAllocated;
}

size_t MemoryManager::getMemoryLimit() {
    std::lock_guard<std::mutex> lock(memoryMutex);
    return memoryLimit;
}

void MemoryManager::setMemoryLimit(size_t bytes) {
    std::lock_guard<std::mutex> lock(memoryMutex);
    memoryLimit = bytes;
    std::cout << "[MEMORY] Memory limit set to " << bytes << " bytes\n";
}

void MemoryManager::forceGC() {
    std::lock_guard<std::mutex> lock(memoryMutex);
    // In C++, explicit GC is not typical, but could trim allocations
    std::cout << "[MEMORY] GC triggered\n";
}

json MemoryManager::getAllocationsByPurpose() {
    std::lock_guard<std::mutex> lock(memoryMutex);

    std::unordered_map<std::string, size_t> byPurpose;
    for (const auto& block : allocations) {
        byPurpose[block.purpose] += block.size;
    }

    json result = json::object();
    for (const auto& [purpose, size] : byPurpose) {
        result[purpose] = size;
    }

    return result;
}
