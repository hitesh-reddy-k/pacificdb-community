#pragma once

#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdint>
#include <chrono>
#include <condition_variable>

class SnapshotManager;

/**
 * GarbageCollector - Background thread for cleaning up dead row versions
 *
 * Periodically:
 * 1. Get oldest active transaction (xmin)
 * 2. Find all row versions where deletedTxn < xmin
 * 3. Delete those versions (no transaction can see them anymore)
 *
 * This prevents unbounded growth of version chains.
 */
class GarbageCollector {
public:
    GarbageCollector(SnapshotManager* snapshotMgr)
        : snapshotManager(snapshotMgr),
          running(false),
          interval_ms(5000),  // Run every 5 seconds
          versionsCollected(0) {}

    /**
     * Start the GC background thread
     */
    void start();

    /**
     * Stop the GC background thread (gracefully)
     */
    void stop();

    /**
     * Main GC loop - called by background thread
     */
    void gcLoop();

    /**
     * Get number of versions collected so far
     */
    uint64_t getVersionsCollected() const {
        return versionsCollected.load();
    }

    /**
     * Set GC interval in milliseconds
     */
    void setInterval(uint32_t ms) {
        interval_ms = ms;
    }

private:
    SnapshotManager* snapshotManager;
    std::unique_ptr<std::thread> gcThread;
    std::atomic<bool> running;
    std::mutex stopMutex;
    std::condition_variable stopCv;
    uint32_t interval_ms;
    std::atomic<uint64_t> versionsCollected;
};
