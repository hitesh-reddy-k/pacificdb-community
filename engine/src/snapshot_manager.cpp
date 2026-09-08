#include "snapshot_manager.hpp"
#include "txid_allocator.hpp"
#include <algorithm>

std::shared_ptr<Snapshot> SnapshotManager::createSnapshot() {
    std::lock_guard<std::mutex> lock(inFlightMutex);

    // Capture current state
    uint64_t xmin = 0;
    if (!inFlightTxns.empty()) {
        xmin = *inFlightTxns.begin();
    } else {
        xmin = txidAllocator->getCurrentTxid();
    }

    uint64_t xmax = txidAllocator->getCurrentTxid() + 1;
    uint64_t snapshotId = nextSnapshotId++;

    return std::make_shared<Snapshot>(snapshotId, xmin, xmax, inFlightTxns);
}

void SnapshotManager::registerTransaction(uint64_t txid) {
    std::lock_guard<std::mutex> lock(inFlightMutex);
    inFlightTxns.insert(txid);
}

void SnapshotManager::unregisterTransaction(uint64_t txid) {
    std::lock_guard<std::mutex> lock(inFlightMutex);
    inFlightTxns.erase(txid);
}

uint64_t SnapshotManager::getOldestActiveTxn() const {
    std::lock_guard<std::mutex> lock(inFlightMutex);
    if (inFlightTxns.empty()) {
        return txidAllocator->getCurrentTxid();
    }
    return *inFlightTxns.begin();
}

void SnapshotManager::releaseSnapshot(std::shared_ptr<Snapshot> snapshot) {
    // Snapshot goes out of scope and is destroyed
    // Could track lifetime for debugging
}
