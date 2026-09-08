#pragma once
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <cstdint>
#include <condition_variable>

// PacificDB v2.9R — Memory pressure state machine.
//
// The engine computes a coarse pressure state from current RSS / configured
// memory budget and uses it to apply graduated write backpressure instead of
// letting the process OOM. State transitions (defaults, tunable via env):
//   < 80%  → NORMAL                (full speed)
//   80-90% → BUSY                  (advisory; flush prioritised)
//   90-95% → THROTTLED             (writes get a small delay)
//   95-98% → BULK_REJECT           (bulk/insertMany rejected, small writes ok)
//   >= 98% → READ_ONLY_EMERGENCY   (all writes rejected until memory drops)
enum class MemoryPressureState : int {
    NORMAL = 0,
    BUSY = 1,
    THROTTLED = 2,
    BULK_REJECT = 3,
    READ_ONLY_EMERGENCY = 4,
};

class MemoryManager {
public:
    static void init();
    static void start();
    static void stop();

    // Check if we should apply backpressure (kept for backward compat).
    static bool shouldSlowDownWrites();

    // Get current memory usage fraction (0.0 to 1.0) of the configured budget.
    static double getMemoryUsage();

    // Force memtable flush (respects a short cooldown unless forced).
    static void forceFlushMemtables();

    // ── v2.9R pressure API ────────────────────────────────────────────────
    // Current pressure state, recomputed on each call from live RSS so callers
    // on the hot write path always see fresh state (not just the 1s monitor tick).
    static MemoryPressureState getPressureState();
    static MemoryPressureState getCachedPressureState();
    static const char* pressureStateName(MemoryPressureState s);

    // Convenience predicates for the write path.
    static bool shouldRejectBulk();          // BULK_REJECT or worse
    static bool isReadOnlyEmergency();        // READ_ONLY_EMERGENCY
    static uint32_t getThrottleDelayMs();     // suggested per-write delay (0 if none)

    // Stats accessors (bytes / MB).
    static size_t getProcessMemory();         // CURRENT resident set size, bytes
    static size_t getMaxMemoryBytes() { return maxMemoryBytes_; }
    static double getMaxMemoryMB() { return maxMemoryBytes_ / 1024.0 / 1024.0; }
    static double getUsedMemoryMB() { return getProcessMemory() / 1024.0 / 1024.0; }
    static double getWarningThreshold() { return warningThreshold_; }
    static double getCriticalThreshold() { return criticalThreshold_; }

    // Counters for observability.
    static uint64_t getBulkRejections() { return bulkRejections_.load(); }
    static uint64_t getWriteRejections() { return writeRejections_.load(); }
    static uint64_t getThrottleEvents() { return throttleEvents_.load(); }
    static void recordBulkRejection() { bulkRejections_.fetch_add(1, std::memory_order_relaxed); }
    static void recordWriteRejection() { writeRejections_.fetch_add(1, std::memory_order_relaxed); }
    static void recordThrottleEvent() { throttleEvents_.fetch_add(1, std::memory_order_relaxed); }

private:
    static std::atomic<bool> running_;
    static std::thread monitorThread_;
    static std::mutex stopMutex_;
    static std::condition_variable stopCv_;
    static std::mutex flushMutex_;

    static size_t maxMemoryBytes_;
    static double warningThreshold_;    // 0.80 = BUSY entry
    static double criticalThreshold_;   // 0.90 = THROTTLED entry
    static double bulkRejectThreshold_; // 0.95 = BULK_REJECT entry
    static double emergencyThreshold_;  // 0.98 = READ_ONLY_EMERGENCY entry

    static std::atomic<int> currentState_;       // cached MemoryPressureState
    static std::atomic<uint32_t> throttleDelayMs_;

    static std::atomic<uint64_t> bulkRejections_;
    static std::atomic<uint64_t> writeRejections_;
    static std::atomic<uint64_t> throttleEvents_;

    static std::atomic<size_t> lastFlushTime_;
    static size_t flushCooldownMs_;     // Minimum time between forced flushes
    static uint32_t pollIntervalMs_;

    static void monitorLoop();
    static size_t getSystemAvailableMemory();
    static MemoryPressureState computeState(double usage);
};
