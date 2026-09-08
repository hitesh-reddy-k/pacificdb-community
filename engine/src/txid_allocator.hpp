#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>

/**
 * TXIDAllocator - Monotonic transaction ID allocator for MVCC
 *
 * Each transaction gets a unique, ever-increasing TXID.
 * TXIDs are used to track visibility: a row is visible if:
 *   created_txn <= current_txn AND (deleted_txn == INF OR deleted_txn > current_txn)
 */
class TXIDAllocator {
public:
    static const uint64_t TXID_MIN = 1;
    static const uint64_t TXID_MAX = 0xFFFFFFFFFFFFFFFFULL;
    static const uint64_t TXID_INVALID = 0;

    TXIDAllocator() : nextTxid(TXID_MIN) {}

    /**
     * Allocate a new transaction ID
     * Thread-safe via atomic increment
     */
    uint64_t allocateTxid() {
        uint64_t txid = nextTxid.fetch_add(1, std::memory_order_seq_cst);
        if (txid >= TXID_MAX) {
            throw std::runtime_error("TXID overflow - too many transactions");
        }
        return txid;
    }

    /**
     * Get the current highest allocated TXID
     * Used by snapshot manager to compute xmax
     */
    uint64_t getCurrentTxid() const {
        return nextTxid.load(std::memory_order_seq_cst) - 1;
    }

    /**
     * Set the next TXID (used during recovery)
     */
    void setNextTxid(uint64_t txid) {
        nextTxid.store(txid + 1, std::memory_order_seq_cst);
    }

private:
    std::atomic<uint64_t> nextTxid;
};
