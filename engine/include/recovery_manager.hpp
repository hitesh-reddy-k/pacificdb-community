#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "wal_integrity.hpp"

using json = nlohmann::json;

/**
 * Recovery Manager
 * Implements all startup recovery phases for production durability
 *
 * Phases:
 * 1. Lock & Validate Directory
 * 2. WAL Scan & Truncation
 * 3. Replay WAL (committed only)
 * 4. Restore Raft State
 * 5. Snapshot Restore (if exists)
 * 6. Consistency Check
 */
class RecoveryManager {
public:
    struct RecoveryState {
        bool success = false;
        bool wasCleanShutdown = false;
        std::string phase;
        size_t walRecordsScanned = 0;
        uint64_t lastValidWalLsn = 0;
        json raftState;
        size_t appliedLSN = 0;
        std::string errorMsg;
    };

    // Execute full recovery sequence
    static RecoveryState executeRecovery(const std::string& dataRoot, const std::string& raftLogPath);

    static void markCleanShutdown(const std::string& dataRoot) {
        WALIntegrity::markCleanShutdown(dataRoot);
    }

private:
    struct WALScanSummary {
        size_t records = 0;
        uint64_t lastLsn = 0;
    };

    // Phase 1: Lock directory & check clean shutdown
    static bool phase1_LockAndValidate(
        const std::string& dataRoot,
        bool wasCleanShutdown);

    // Phase 2: Scan WAL and truncate corrupted tail
    static WALScanSummary phase2_WALScanAndTruncate(const std::string& dataRoot);

    // Phase 4: Restore Raft state (currentTerm, votedFor, commitIndex)
    static json phase4_RestoreRaftState(const std::string& raftLogPath);

    // Phase 5: Load snapshot if it exists and is valid
    static json phase5_SnapshotRestore(const std::string& dataRoot);

    // Phase 6: Verify critical invariants
    static bool phase6_ConsistencyCheck(const json& raftState, size_t appliedLSN);
};
