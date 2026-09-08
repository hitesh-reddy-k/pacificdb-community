#pragma once

#include <vector>
#include <set>
#include <mutex>
#include <memory>
#include <cstdint>
#include <atomic>

/**
 * SnapshotManager - Manages transaction snapshots for MVCC
 *
 * A snapshot captures:
 * - xmin: oldest active transaction (nothing created before this is deleted)
 * - xmax: highest allocated transaction at snapshot start (anything >= this doesn't exist yet)
 * - active_txns: set of in-flight transactions at snapshot start
 *
 * Used by readers to determine visibility and by GC to know when versions are safe to delete.
 */
class Snapshot {
public:
    uint64_t xmin;           // Oldest active transaction
    uint64_t xmax;           // Highest allocated transaction at snapshot start
    std::set<uint64_t> inFlightTxns;  // Currently running transactions
    uint64_t snapshotId;     // Unique ID for this snapshot

    Snapshot(uint64_t id, uint64_t min, uint64_t max, const std::set<uint64_t>& inFlight)
        : xmin(min), xmax(max), inFlightTxns(inFlight), snapshotId(id) {}

    /**
     * Check if a row version is visible in this snapshot
     * Visibility rule: created_txn <= xmin OR (created_txn not in inFlightTxns AND created_txn < xmax)
     * AND: (deleted_txn is null OR deleted_txn > current_txn)
     */
    bool isRowVersionVisible(uint64_t createdTxn, uint64_t deletedTxn) const {
        // Row not yet created in this snapshot
        if (createdTxn >= xmax) return false;

        // Row created by an aborted or in-flight transaction outside our snapshot
        if (createdTxn >= xmin && inFlightTxns.count(createdTxn)) return false;

        // Row was deleted before/during our snapshot
        if (deletedTxn != 0 && deletedTxn <= xmin) return false;

        return true;
    }

    /**
     * Oldest transaction we can safely delete versions for (everyone before xmin is done)
     */
    uint64_t getOldestActiveXmin() const {
        return xmin;
    }
};

class SnapshotManager {
public:
    SnapshotManager() : nextSnapshotId(1), txidAllocator(nullptr) {}

    void setTXIDAllocator(class TXIDAllocator* allocator) {
        txidAllocator = allocator;
    }

    /**
     * Create a new snapshot for a transaction
     * Captures current state: xmin, xmax, active transactions
     */
    std::shared_ptr<Snapshot> createSnapshot();

    /**
     * Register that a transaction is in-flight
     */
    void registerTransaction(uint64_t txid);

    /**
     * Unregister transaction (commit or abort)
     */
    void unregisterTransaction(uint64_t txid);

    /**
     * Get the oldest transaction still in-flight
     * Used by garbage collector to know what versions are safe to delete
     */
    uint64_t getOldestActiveTxn() const;

    /**
     * Release a snapshot when transaction finishes
     */
    void releaseSnapshot(std::shared_ptr<Snapshot> snapshot);

private:
    std::atomic<uint64_t> nextSnapshotId;
    class TXIDAllocator* txidAllocator;
    mutable std::mutex inFlightMutex;
    std::set<uint64_t> inFlightTxns;
};
