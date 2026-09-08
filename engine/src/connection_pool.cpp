#include "connection_pool.hpp"
#include "request_timing.hpp"
#include <iostream>
#include <algorithm>
#include "query_cancellation.hpp"
#include <thread>
#include <cstdlib>
#include <string>

namespace {
std::chrono::milliseconds resolveEnqueueWaitBudget() {
    long long waitMs = 25;
    if (const char* env = std::getenv("CONN_POOL_ENQUEUE_WAIT_MS")) {
        try {
            waitMs = std::stoll(env);
        } catch (...) {
            waitMs = 25;
        }
    }
    if (waitMs < 0) waitMs = 0;
    return std::chrono::milliseconds(waitMs);
}
} // namespace

ConnectionPool* g_connectionPool = nullptr;

ConnectionPool::ConnectionPool(size_t minThreads, size_t maxThreads, size_t maxQueueSize)
    : enqueueWaitBudget_(resolveEnqueueWaitBudget()),
      minThreads_(minThreads), maxThreads_(maxThreads), maxQueueSize_(maxQueueSize) {

    std::cout << "[CONNECTION-POOL] Initializing with " << minThreads
              << " min threads, " << maxThreads << " max threads\n";

    // Start minimum number of worker threads
    for (size_t i = 0; i < minThreads_; ++i) {
        workers_.emplace_back(&ConnectionPool::workerThread, this);
        totalThreads_++;
    }

    // Start monitoring thread for dynamic scaling
    monitor_ = std::thread(&ConnectionPool::monitorThread, this);

    std::cout << "[CONNECTION-POOL] Started " << minThreads_ << " worker threads\n";
}

ConnectionPool::~ConnectionPool() {
    shutdown();
}

ConnectionPool::Task ConnectionPool::wrapTask(Task task, std::chrono::milliseconds timeout) {
    const uint64_t enqueuedAtUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    return wrapTask(std::move(task), timeout, enqueuedAtUs);
}

ConnectionPool::Task ConnectionPool::wrapTask(Task task, std::chrono::milliseconds timeout, uint64_t enqueuedAtUs) {
    auto token = std::make_shared<CancellationToken>();

    if (timeout.count() > 0) {
        token->deadline = std::chrono::steady_clock::now() + timeout;
    }

    return [task = std::move(task), token, enqueuedAtUs]() mutable {
        QueryCancel::setToken(token);

        if (enqueuedAtUs > 0) {
            const uint64_t nowUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if (nowUs > enqueuedAtUs) {
                pacificdb::timing::recordStage(pacificdb::timing::Stage::ConnPoolWait, nowUs - enqueuedAtUs);
            }
        }

        try {
            task();
        } catch (const std::exception& e) {
            std::cerr << "[CONNECTION-POOL] Worker caught exception: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[CONNECTION-POOL] Worker caught unknown exception\n";
        }

        token->finished.store(true);
        QueryCancel::clearToken();
    };
}


std::deque<ConnectionPool::Task>& ConnectionPool::queueForPriority(Priority priority) {
    switch (priority) {
        case Priority::HIGH:
            return highPriorityQueue_;
        case Priority::LOW:
            return lowPriorityQueue_;
        case Priority::NORMAL:
        default:
            return normalPriorityQueue_;
    }
}

bool ConnectionPool::hasPendingTasksUnsafe() const {
    return !highPriorityQueue_.empty() || !normalPriorityQueue_.empty() || !lowPriorityQueue_.empty();
}

ConnectionPool::Task ConnectionPool::popNextTaskUnsafe() {
    if (!highPriorityQueue_.empty()) {
        Task task = std::move(highPriorityQueue_.front());
        highPriorityQueue_.pop_front();
        return task;
    }
    if (!normalPriorityQueue_.empty()) {
        Task task = std::move(normalPriorityQueue_.front());
        normalPriorityQueue_.pop_front();
        return task;
    }
    if (!lowPriorityQueue_.empty()) {
        Task task = std::move(lowPriorityQueue_.front());
        lowPriorityQueue_.pop_front();
        return task;
    }
    return {};
}

void ConnectionPool::shutdown() {
    if (shutdown_.load()) return;

    std::cout << "[CONNECTION-POOL] Shutting down...\n";
    std::cout << "[CONNECTION-POOL] Total tasks processed: " << processedTasks_.load() << "\n";

    shutdown_.store(true);
    condition_.notify_all();

    // Wait for all workers to finish
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Wait for monitor thread
    if (monitor_.joinable()) {
        monitor_.join();
    }

    std::cout << "[CONNECTION-POOL] Shutdown complete\n";
}

bool ConnectionPool::submit(Task task, std::chrono::milliseconds timeout, Priority priority) {
    {
        auto lockStart = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(queueMutex_);
        auto lockEnd = std::chrono::steady_clock::now();
        uint64_t lockWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(lockEnd - lockStart).count());
        queueMutexWaitTotalUs_.fetch_add(lockWaitUs, std::memory_order_relaxed);
        queueMutexWaitCount_.fetch_add(1, std::memory_order_relaxed);

        // Apply bounded backpressure before rejecting when queue is full.
        size_t totalQueued = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
        if (totalQueued >= maxQueueSize_) {
            if (enqueueWaitBudget_.count() > 0) {
                const auto deadline = std::chrono::steady_clock::now() + enqueueWaitBudget_;
                while (!shutdown_.load()) {
                    auto cvStart = std::chrono::steady_clock::now();
                    if (condition_.wait_until(lock, deadline) == std::cv_status::timeout) {
                        auto cvEnd = std::chrono::steady_clock::now();
                        uint64_t cvWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(cvEnd - cvStart).count());
                        queueCvWaitTotalUs_.fetch_add(cvWaitUs, std::memory_order_relaxed);
                        queueCvWaitCount_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    auto cvEnd = std::chrono::steady_clock::now();
                    uint64_t cvWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(cvEnd - cvStart).count());
                    queueCvWaitTotalUs_.fetch_add(cvWaitUs, std::memory_order_relaxed);
                    queueCvWaitCount_.fetch_add(1, std::memory_order_relaxed);
                    totalQueued = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
                    if (totalQueued < maxQueueSize_) {
                        break;
                    }
                }
            }

            totalQueued = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
            if (totalQueued >= maxQueueSize_) {
                std::cerr << "[CONNECTION-POOL] WARNING: Queue full ("
                          << totalQueued << ") >= max (" << maxQueueSize_ << "), rejecting task\n";
                return false;
            }
        }

        queueForPriority(priority).push_back(wrapTask(std::move(task), timeout, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count())));
        queueDepthSamples_.fetch_add(1, std::memory_order_relaxed);
        queueDepthTotal_.fetch_add(highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size(), std::memory_order_relaxed);
        const uint64_t queuedNow = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
        queueDepthMax_.store(std::max<uint64_t>(queueDepthMax_.load(std::memory_order_relaxed), queuedNow), std::memory_order_relaxed);
    }

    condition_.notify_one();
    return true;
}

size_t ConnectionPool::submitBatch(std::vector<Task> tasks, std::chrono::milliseconds timeout, Priority priority) {
    if (tasks.empty()) return 0;

    size_t accepted = 0;
    {
        auto lockStart = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(queueMutex_);
        auto lockEnd = std::chrono::steady_clock::now();
        uint64_t lockWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(lockEnd - lockStart).count());
        queueMutexWaitTotalUs_.fetch_add(lockWaitUs, std::memory_order_relaxed);
        queueMutexWaitCount_.fetch_add(1, std::memory_order_relaxed);
        std::deque<Task>& targetQueue = queueForPriority(priority);
        size_t remainingCapacity = 0;
        size_t totalQueued = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();

        if (totalQueued >= maxQueueSize_ && enqueueWaitBudget_.count() > 0) {
            const auto deadline = std::chrono::steady_clock::now() + enqueueWaitBudget_;
            while (!shutdown_.load()) {
                auto cvStart = std::chrono::steady_clock::now();
                if (condition_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    auto cvEnd = std::chrono::steady_clock::now();
                    uint64_t cvWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(cvEnd - cvStart).count());
                    queueCvWaitTotalUs_.fetch_add(cvWaitUs, std::memory_order_relaxed);
                    queueCvWaitCount_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                auto cvEnd = std::chrono::steady_clock::now();
                uint64_t cvWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(cvEnd - cvStart).count());
                queueCvWaitTotalUs_.fetch_add(cvWaitUs, std::memory_order_relaxed);
                queueCvWaitCount_.fetch_add(1, std::memory_order_relaxed);
                totalQueued = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
                if (totalQueued < maxQueueSize_) {
                    break;
                }
            }
        }

        if (totalQueued < maxQueueSize_) {
            remainingCapacity = maxQueueSize_ - totalQueued;
        }
        if (remainingCapacity == 0) {
            return 0;
        }

        accepted = std::min(remainingCapacity, tasks.size());
        for (size_t i = 0; i < accepted; ++i) {
            targetQueue.push_back(wrapTask(std::move(tasks[i]), timeout, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count())));
        }
        queueDepthSamples_.fetch_add(accepted, std::memory_order_relaxed);
        queueDepthTotal_.fetch_add(highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size(), std::memory_order_relaxed);
        const uint64_t queuedNow = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
        queueDepthMax_.store(std::max<uint64_t>(queueDepthMax_.load(std::memory_order_relaxed), queuedNow), std::memory_order_relaxed);
    }

    if (accepted > 0) {
        condition_.notify_all();
    }
    if (accepted < tasks.size()) {
        std::cerr << "[CONNECTION-POOL] WARNING: Batch partially accepted ("
                  << accepted << "/" << tasks.size() << ")\n";
    }
    return accepted;
}

void ConnectionPool::workerThread() {
    while (true) {
        Task task;
        bool dequeuedTask = false;

        {
            auto lockStart = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(queueMutex_);
            auto lockEnd = std::chrono::steady_clock::now();
            uint64_t lockWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(lockEnd - lockStart).count());
            queueMutexWaitTotalUs_.fetch_add(lockWaitUs, std::memory_order_relaxed);
            queueMutexWaitCount_.fetch_add(1, std::memory_order_relaxed);

            auto cvStart = std::chrono::steady_clock::now();
            condition_.wait(lock, [this] {
                return shutdown_.load() || hasPendingTasksUnsafe();
            });
            auto cvEnd = std::chrono::steady_clock::now();
            uint64_t cvWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(cvEnd - cvStart).count());
            queueCvWaitTotalUs_.fetch_add(cvWaitUs, std::memory_order_relaxed);
            queueCvWaitCount_.fetch_add(1, std::memory_order_relaxed);

            if (shutdown_.load() && !hasPendingTasksUnsafe()) {
                return;
            }

            task = popNextTaskUnsafe();
            dequeuedTask = static_cast<bool>(task);
            if (!dequeuedTask) {
                queueSpuriousWakeups_.fetch_add(1, std::memory_order_relaxed);
            } else {
                auto queuedNow = highPriorityQueue_.size() + normalPriorityQueue_.size() + lowPriorityQueue_.size();
                queueDepthSamples_.fetch_add(1, std::memory_order_relaxed);
                queueDepthTotal_.fetch_add(queuedNow, std::memory_order_relaxed);
                queueDepthMax_.store(std::max<uint64_t>(queueDepthMax_.load(std::memory_order_relaxed), queuedNow), std::memory_order_relaxed);
            }
        }

        if (dequeuedTask) {
            // Wake blocked producers waiting for queue capacity.
            condition_.notify_all();
        }

        if (task) {
            activeThreads_++;
            auto workerActiveStart = std::chrono::steady_clock::now();

            try {
                task();
                processedTasks_++;
            } catch (const std::exception& e) {
                std::cerr << "[CONNECTION-POOL] Worker caught exception: "
                          << e.what() << "\n";
            } catch (...) {
                std::cerr << "[CONNECTION-POOL] Worker caught unknown exception\n";
            }

            activeThreads_--;
            auto workerActiveEnd = std::chrono::steady_clock::now();
            uint64_t activeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(workerActiveEnd - workerActiveStart).count());
            workerExecTotalUs_.fetch_add(activeUs, std::memory_order_relaxed);
            workerExecCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void ConnectionPool::scaleUp() {
    std::lock_guard<std::mutex> lock(queueMutex_);

    if (totalThreads_.load() >= maxThreads_) {
        return;
    }

    size_t newThreads = std::min(
        size_t(5),
        maxThreads_ - totalThreads_.load()
    );

    for (size_t i = 0; i < newThreads; ++i) {
        workers_.emplace_back(&ConnectionPool::workerThread, this);
        totalThreads_++;
    }

    std::cout << "[CONNECTION-POOL] Scaled up: " << totalThreads_.load()
              << " threads (+" << newThreads << ")\n";
}

void ConnectionPool::scaleDown() {
    // Note: In production, implement proper thread termination
    // For now, threads will naturally terminate on shutdown
    // To properly scale down, you'd need to:
    // 1. Mark threads for termination
    // 2. Wake them up with condition variable
    // 3. Join and remove from workers_ vector
}

void ConnectionPool::monitorThread() {
    size_t scaleIntervalMs = 50;
    if (const char* env = std::getenv("CONN_POOL_SCALE_INTERVAL_MS")) {
        try {
            scaleIntervalMs = std::max<size_t>(10, std::stoul(env));
        } catch (...) {
            scaleIntervalMs = 50;
        }
    }

    size_t maxScaleStep = 128;
    if (const char* env = std::getenv("CONN_POOL_SCALE_STEP_MAX")) {
        try {
            maxScaleStep = std::max<size_t>(1, std::stoul(env));
        } catch (...) {
            maxScaleStep = 128;
        }
    }

    while (!shutdown_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(scaleIntervalMs));

        if (shutdown_.load()) break;

        size_t queued = getQueuedTasks();
        size_t active = activeThreads_.load();
        size_t total = totalThreads_.load();

        // Scale only for work that the currently idle workers cannot absorb. The old
        // rule added `queued` threads whenever a transient sample saw any queue, while
        // ignoring idle capacity. Since workers are retained, short connection/status
        // bursts ratcheted the pool to its maximum (955 clone3 calls in the V10 syscall
        // profile), creating futex contention instead of useful parallelism.
        const size_t idle = total > active ? total - active : 0;
        const size_t shortage = queued > idle ? queued - idle : 0;
        if (shortage > 0 && total < maxThreads_) {
            size_t toAdd = std::min(shortage, maxThreads_ - total);
            toAdd = std::min(toAdd, maxScaleStep);

            if (toAdd > 0) {
                std::lock_guard<std::mutex> lock(queueMutex_);
                for (size_t i = 0; i < toAdd && totalThreads_.load() < maxThreads_; ++i) {
                    workers_.emplace_back(&ConnectionPool::workerThread, this);
                    totalThreads_++;
                }
                std::cout << "[CONNECTION-POOL] Scaled up to " << totalThreads_.load()
                          << " threads (queued=" << queued << ", idle=" << idle
                          << ", shortage=" << shortage << ")\n";
            }
        }
    }
}

void initConnectionPool(size_t minThreads, size_t maxThreads, size_t maxQueueSize) {
    if (!g_connectionPool) {
        g_connectionPool = new ConnectionPool(minThreads, maxThreads, maxQueueSize);
    }
}

void destroyConnectionPool() {
    if (g_connectionPool) {
        delete g_connectionPool;
        g_connectionPool = nullptr;
    }
}
