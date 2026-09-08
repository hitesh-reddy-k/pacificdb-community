#include "memory_manager.hpp"
#include "lsm.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h>
// Release free heap memory back to the OS. Under high JSON-allocation churn across
// many connection-pool threads, glibc retains freed pages in per-thread arenas and
// RSS climbs even though logical memory is bounded. malloc_trim hands it back.
static inline void releaseFreeMemoryToOS() { malloc_trim(0); }
#else
static inline void releaseFreeMemoryToOS() {}
#endif

std::atomic<bool> MemoryManager::running_(false);
std::thread MemoryManager::monitorThread_;
std::mutex MemoryManager::stopMutex_;
std::condition_variable MemoryManager::stopCv_;
std::mutex MemoryManager::flushMutex_;
size_t MemoryManager::maxMemoryBytes_(4ULL * 1024 * 1024 * 1024); // 4GB default
double MemoryManager::warningThreshold_(0.80);   // BUSY
double MemoryManager::criticalThreshold_(0.90);  // THROTTLED
double MemoryManager::bulkRejectThreshold_(0.95); // BULK_REJECT
double MemoryManager::emergencyThreshold_(0.98);  // READ_ONLY_EMERGENCY
std::atomic<int> MemoryManager::currentState_(0);
std::atomic<uint32_t> MemoryManager::throttleDelayMs_(0);
std::atomic<uint64_t> MemoryManager::bulkRejections_(0);
std::atomic<uint64_t> MemoryManager::writeRejections_(0);
std::atomic<uint64_t> MemoryManager::throttleEvents_(0);
std::atomic<size_t> MemoryManager::lastFlushTime_(0);
size_t MemoryManager::flushCooldownMs_(3000); // 3s between forced flushes (was 10s)
uint32_t MemoryManager::pollIntervalMs_(1000); // poll every 1s (was 5s)

static double envDouble(const char* name, double fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

void MemoryManager::init() {
    const char* maxMem = std::getenv("MAX_MEMORY_GB");
    if (maxMem) {
        try { maxMemoryBytes_ = std::stoull(maxMem) * 1024ULL * 1024 * 1024; } catch (...) {}
    }
    const char* maxMemMb = std::getenv("MAX_MEMORY_MB");
    if (maxMemMb) {
        try { maxMemoryBytes_ = std::stoull(maxMemMb) * 1024ULL * 1024; } catch (...) {}
    }

    warningThreshold_    = envDouble("MEMORY_WARNING_THRESHOLD", 0.80);
    criticalThreshold_   = envDouble("MEMORY_CRITICAL_THRESHOLD", 0.90);
    bulkRejectThreshold_ = envDouble("MEMORY_BULK_REJECT_THRESHOLD", 0.95);
    emergencyThreshold_  = envDouble("MEMORY_EMERGENCY_THRESHOLD", 0.98);

    const char* poll = std::getenv("MEMORY_POLL_INTERVAL_MS");
    if (poll) { try { pollIntervalMs_ = static_cast<uint32_t>(std::stoul(poll)); } catch (...) {} }

#if defined(__GLIBC__)
    // Cap malloc arenas. glibc spawns up to 8*ncores arenas (one per thread group),
    // each able to retain tens of MB of freed memory. With hundreds of connection
    // -pool threads churning JSON, that fragmentation alone drove RSS to >1.5GB and
    // OOM-killed the box. One arena trades a little allocator contention for an order
    // of magnitude less resident-memory bloat. Honour MALLOC_ARENA_MAX if the user set it.
    {
        int arenaMax = 2;
        const char* am = std::getenv("MALLOC_ARENA_MAX");
        if (am && *am) { try { arenaMax = std::max(1, std::stoi(am)); } catch (...) {} }
        mallopt(M_ARENA_MAX, arenaMax);
        // Return memory to the OS more eagerly (default trim threshold is 128KB but the
        // top-of-heap-only policy keeps fragmented holes; this nudges it).
        mallopt(M_TRIM_THRESHOLD, 256 * 1024);
        std::cout << "[MEMORY] malloc tuned: M_ARENA_MAX=" << arenaMax << ", M_TRIM_THRESHOLD=256KB" << std::endl;
    }
#endif

    std::cout << "[MEMORY] Initialized v2.9R - Max: " << (maxMemoryBytes_ / 1024 / 1024) << "MB"
              << ", BUSY=" << (warningThreshold_ * 100) << "%"
              << ", THROTTLE=" << (criticalThreshold_ * 100) << "%"
              << ", BULK_REJECT=" << (bulkRejectThreshold_ * 100) << "%"
              << ", READ_ONLY=" << (emergencyThreshold_ * 100) << "%"
              << ", poll=" << pollIntervalMs_ << "ms" << std::endl;
}

void MemoryManager::start() {
    if (running_.exchange(true)) return;

    monitorThread_ = std::thread([]() {
        std::cout << "[MEMORY] Monitor thread started" << std::endl;
        while (running_.load()) {
            monitorLoop();
            std::unique_lock<std::mutex> lock(stopMutex_);
            stopCv_.wait_for(lock, std::chrono::milliseconds(pollIntervalMs_), []() {
                return !running_.load(std::memory_order_acquire);
            });
        }
        std::cout << "[MEMORY] Monitor thread stopped" << std::endl;
    });
}

void MemoryManager::stop() {
    running_.store(false);
    stopCv_.notify_all();
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
}

MemoryPressureState MemoryManager::computeState(double usage) {
    if (usage >= emergencyThreshold_)  return MemoryPressureState::READ_ONLY_EMERGENCY;
    if (usage >= bulkRejectThreshold_) return MemoryPressureState::BULK_REJECT;
    if (usage >= criticalThreshold_)   return MemoryPressureState::THROTTLED;
    if (usage >= warningThreshold_)    return MemoryPressureState::BUSY;
    return MemoryPressureState::NORMAL;
}

const char* MemoryManager::pressureStateName(MemoryPressureState s) {
    switch (s) {
        case MemoryPressureState::NORMAL: return "NORMAL";
        case MemoryPressureState::BUSY: return "BUSY";
        case MemoryPressureState::THROTTLED: return "THROTTLED";
        case MemoryPressureState::BULK_REJECT: return "BULK_REJECT";
        case MemoryPressureState::READ_ONLY_EMERGENCY: return "READ_ONLY_EMERGENCY";
    }
    return "NORMAL";
}

void MemoryManager::monitorLoop() {
    size_t current = getProcessMemory();
    double usage = static_cast<double>(current) / maxMemoryBytes_;
    MemoryPressureState state = computeState(usage);

    // Suggested per-write delay scales with severity.
    uint32_t delay = 0;
    switch (state) {
        case MemoryPressureState::THROTTLED:           delay = 5;  break;
        case MemoryPressureState::BULK_REJECT:         delay = 15; break;
        case MemoryPressureState::READ_ONLY_EMERGENCY: delay = 50; break;
        default: delay = 0; break;
    }

    int prev = currentState_.exchange(static_cast<int>(state));
    throttleDelayMs_.store(delay, std::memory_order_relaxed);

    if (prev != static_cast<int>(state)) {
        std::cout << "[MEMORY] state " << pressureStateName(static_cast<MemoryPressureState>(prev))
                  << " -> " << pressureStateName(state)
                  << " (" << (current / 1024 / 1024) << "MB / "
                  << (maxMemoryBytes_ / 1024 / 1024) << "MB, "
                  << (usage * 100) << "%)" << std::endl;
    }

    // At THROTTLED or worse, proactively drain memtables to recover headroom.
    if (state >= MemoryPressureState::THROTTLED) {
        forceFlushMemtables();
    }
    // Hand freed heap pages back to the OS so RSS actually drops after flushes/erases
    // (otherwise glibc retains them and the monitor sees no relief). Trim every tick
    // under pressure, and every ~5s otherwise to keep steady-state RSS low on
    // memory-constrained hosts.
    static int trimTick = 0;
    if (state >= MemoryPressureState::BUSY || (++trimTick % 5 == 0)) {
        releaseFreeMemoryToOS();
    }
}

size_t MemoryManager::getProcessMemory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
    return 0;
#else
    // CURRENT resident set size from /proc/self/statm (page 2 = resident pages).
    // NOTE: getrusage(ru_maxrss) returns PEAK rss which never decreases — using it
    // made the monitor believe memory only ever grows. statm gives live RSS.
    static const long page_size = sysconf(_SC_PAGE_SIZE);
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        long total_pages = 0, resident_pages = 0;
        if (statm >> total_pages >> resident_pages) {
            return static_cast<size_t>(resident_pages) * static_cast<size_t>(page_size);
        }
    }
    // Fallback to peak rss if /proc is unavailable.
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<size_t>(usage.ru_maxrss) * 1024;
#endif
}

size_t MemoryManager::getSystemAvailableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        return memInfo.ullAvailPhys;
    }
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    return static_cast<size_t>(pages) * static_cast<size_t>(page_size);
#endif
}

bool MemoryManager::shouldSlowDownWrites() {
    return getCachedPressureState() >= MemoryPressureState::THROTTLED;
}

double MemoryManager::getMemoryUsage() {
    size_t current = getProcessMemory();
    return static_cast<double>(current) / maxMemoryBytes_;
}

MemoryPressureState MemoryManager::getPressureState() {
    // Recompute from live RSS so hot-path callers see fresh state.
    return computeState(getMemoryUsage());
}

MemoryPressureState MemoryManager::getCachedPressureState() {
    return static_cast<MemoryPressureState>(currentState_.load(std::memory_order_relaxed));
}

bool MemoryManager::shouldRejectBulk() {
    return getPressureState() >= MemoryPressureState::BULK_REJECT;
}

bool MemoryManager::isReadOnlyEmergency() {
    return getPressureState() >= MemoryPressureState::READ_ONLY_EMERGENCY;
}

uint32_t MemoryManager::getThrottleDelayMs() {
    return throttleDelayMs_.load(std::memory_order_relaxed);
}

void MemoryManager::forceFlushMemtables() {
    // Non-blocking try-lock: if a flush is already happening, skip.
    std::unique_lock<std::mutex> lock(flushMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    size_t lastFlush = lastFlushTime_.load();
    if (now - static_cast<long long>(lastFlush) < static_cast<long long>(flushCooldownMs_)) {
        return; // cooldown active — quiet (no log spam)
    }

    try {
        LSM::forceFlush();
        releaseFreeMemoryToOS(); // give the just-freed memtable memory back to the OS
        lastFlushTime_.store(static_cast<size_t>(now));
    } catch (const std::exception& e) {
        std::cerr << "[MEMORY] Flush failed: " << e.what() << std::endl;
    }
}
