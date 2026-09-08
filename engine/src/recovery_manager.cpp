#include "recovery_manager.hpp"
#include "database_engine.hpp"
#include "raft_core.hpp"
#include "metrics_exporter.hpp"
#include "data_durability.hpp"
#include "storage_path.hpp"
#include "test_failpoint.hpp"
#include "snapshot_bundle.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <array>

static bool recoveryTraceEnabled() {
    const char* v = std::getenv("CONSISTENCY_TRACE");
    return v && std::string(v) == "1";
}

static bool isInactiveRecoveryTreeName(const std::string& name) {
    static constexpr std::array<const char*, 10> exactNames = {
        ".snapshot_backup",
        ".snapshot_old",
        ".snapshot_tmp",
        ".idx-rebuild",
        ".discarded",
        ".discarded_generation",
        ".restore_tmp",
        ".restore-tmp",
        ".rollback",
        ".rollback_only",
    };
    if (std::find(exactNames.begin(), exactNames.end(), name) != exactNames.end()) {
        return true;
    }

    static constexpr std::array<const char*, 10> generationPrefixes = {
        ".snapshot_backup.",
        ".snapshot_old.",
        ".snapshot_tmp.",
        ".idx-rebuild.",
        ".discarded.",
        ".discarded-",
        ".restore_tmp.",
        ".restore-tmp.",
        ".rollback.",
        ".rollback-",
    };
    return std::any_of(
        generationPrefixes.begin(), generationPrefixes.end(),
        [&name](const char* prefix) { return name.rfind(prefix, 0) == 0; });
}

static std::vector<std::string> findWalFiles(const std::string& dataRoot) {
    std::vector<std::string> files;
    std::error_code ec;
    if (!std::filesystem::exists(dataRoot, ec)) return files;
    const std::filesystem::path canonicalRoot =
        std::filesystem::canonical(dataRoot, ec);
    if (ec || canonicalRoot.empty()) {
        throw std::runtime_error(
            "cannot canonicalize DATA_ROOT for WAL recovery");
    }

    auto it = std::filesystem::recursive_directory_iterator(dataRoot, ec);
    const auto end = std::filesystem::recursive_directory_iterator();
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        validateContainedStoragePath(canonicalRoot, entry.path());
        if (entry.is_directory()) {
            const std::string name = entry.path().filename().string();
            // These are inactive, run-owned generations. Replaying their WALs re-applies
            // state that a snapshot already replaced and can fence an otherwise healthy
            // node before the active snapshot is restored.
            if (isInactiveRecoveryTreeName(name)) {
                it.disable_recursion_pending();
            } else if (name == "wal" ||
                       (name.size() > 13 &&
                        name.compare(name.size() - 13, 13, ".wal.segments") == 0)) {
                // Collection WALs are streamed once by LSM::restoreFromWal.
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file()) continue;
        const auto& path = entry.path();
        const std::string name = path.filename().string();
        if (name == "wal.log" || name == "db.wal" || path.extension() == ".wal") {
            files.push_back(path.string());
        }
    }
    if (ec) {
        throw std::runtime_error(
            "WAL recovery discovery failed under DATA_ROOT: " + ec.message());
    }

    return files;
}

#define RECOV_TRACE(x) do { if (recoveryTraceEnabled()) { std::cout << "[RECOVERY-TRACE] " << x; } } while(0)

RecoveryManager::RecoveryState RecoveryManager::executeRecovery(const std::string& dataRoot, const std::string& raftLogPath) {
    RecoveryState state;
    auto failRecovery = [&state](
        const std::string& error,
        uint64_t failedIndex = 0,
        uint64_t failedTerm = 0,
        const std::string& commandType = "startup_recovery",
        const std::string& logicalOperationId = std::string(),
        bool mutationMayHaveOccurred = false) -> RecoveryState {
        state.success = false;
        state.errorMsg = error;
        RaftCore::instance().blockApply(
            failedIndex, failedTerm, commandType, logicalOperationId, error,
            mutationMayHaveOccurred);
        return state;
    };
    if (dataRoot.empty() ||
        !std::filesystem::path(dataRoot).is_absolute()) {
        return failRecovery(
            "Recovery requires an explicit absolute DATA_ROOT");
    }
    if (raftLogPath.empty() ||
        !std::filesystem::path(raftLogPath).is_absolute()) {
        return failRecovery(
            "Recovery requires an explicit absolute RAFT_LOG_PATH");
    }

    std::cout << "\n[RECOVERY] Starting 6-phase recovery sequence..." << std::endl;
    std::cout << "[RECOVERY] Data root: " << dataRoot << std::endl;

    // Phase 1: Lock & Validate
    state.phase = "Phase 1: Lock & Validate Directory";
    std::cout << "\n[RECOVERY] " << state.phase << std::endl;
    const bool wasCleanShutdown = WALIntegrity::wasCleanShutdown(dataRoot);
    state.wasCleanShutdown = wasCleanShutdown;
    if (!phase1_LockAndValidate(dataRoot, wasCleanShutdown)) {
        return failRecovery(
            "Failed to lock data directory or another instance is running");
    }
    std::cout << "[RECOVERY] ✓ Directory locked, clean shutdown flag checked" << std::endl;
    pacificdb::test::hitFailpoint("FP_SHUTDOWN_DURING_RECOVERY", 1);

    // Phase 2: WAL Scan & Truncate
    state.phase = "Phase 2: WAL Scan & Truncate";
    std::cout << "\n[RECOVERY] " << state.phase << std::endl;
    const auto walScan = phase2_WALScanAndTruncate(dataRoot);
    state.walRecordsScanned = walScan.records;
    state.lastValidWalLsn = walScan.lastLsn;
    std::cout << "[RECOVERY] ✓ WAL scanned, " << state.walRecordsScanned
              << " records found, tail truncated" << std::endl;
    pacificdb::test::hitFailpoint("FP_RECOVERY_AFTER_WAL_DISCOVERY", 1);

    // Phase 3: Replay WAL
    state.phase = "Phase 3: Replay WAL (Committed Only)";
    std::cout << "\n[RECOVERY] " << state.phase << std::endl;
    pacificdb::test::hitFailpoint("FP_RECOVERY_BEFORE_WAL_REPLAY", 1);
    // Collection state is restored once, later, by LSM's bounded scanner. Raft
    // restores its own binary log. This phase intentionally retains no payloads.
    MetricsExporter::recordCustomMetric(
        "pacificdb_wal_scan_records", static_cast<double>(state.walRecordsScanned));
    MetricsExporter::recordCustomMetric(
        "pacificdb_wal_scan_last_lsn", static_cast<double>(state.lastValidWalLsn));
    std::cout << "[RECOVERY] ✓ WAL ownership assigned without payload materialization"
              << std::endl;
    pacificdb::test::hitFailpoint("FP_RECOVERY_AFTER_WAL_REPLAY", 1);

    // Phase 4: Restore Raft State
    state.phase = "Phase 4: Restore Raft State";
    std::cout << "\n[RECOVERY] " << state.phase << std::endl;
    state.raftState = phase4_RestoreRaftState(raftLogPath);
    if (state.appliedLSN > 0 && state.raftState.value("commitIndex", 0UL) == 0UL) {
        state.raftState["commitIndex"] = state.appliedLSN;
        state.raftState["lastApplied"] = state.appliedLSN;
        MetricsExporter::incrementCounter("pacificdb_recovery_raft_state_reconstructed_total", 1.0);
    }
    std::cout << "[RECOVERY] ✓ Raft state restored, currentTerm: "
              << state.raftState.value("currentTerm", 0)
              << ", commitIndex: " << state.raftState.value("commitIndex", 0) << std::endl;

    // Phase 5: Snapshot Restore
    state.phase = "Phase 5: Snapshot Restore";
    std::cout << "\n[RECOVERY] " << state.phase << std::endl;
    auto snapshotRestoreStart = std::chrono::steady_clock::now();
    RECOV_TRACE("snapshot_restore_start" << "\n");
    if (wasCleanShutdown) {
        std::cout << "[RECOVERY] ✓ Clean shutdown: retaining flushed LSM state; "
                     "historical snapshot not reapplied" << std::endl;
    } else {
        json snapshotData = phase5_SnapshotRestore(dataRoot);
        if (snapshotData.empty()) {
            std::cout << "[RECOVERY] ✓ No snapshot found (normal)" << std::endl;
            RECOV_TRACE("snapshot_restore_empty" << "\n");
        } else {
            std::cout << "[RECOVERY] ✓ Snapshot restored" << std::endl;
            // If snapshot includes LSM payload, apply it to the storage engine
            try {
                if (snapshotData.contains("lsm_payload")) {
                    const auto& payload = snapshotData["lsm_payload"];
                    RECOV_TRACE("snapshot_apply_start" << "\n");
                    const std::string bundlePath = payload.value("version", 0) == 3
                        ? dataRoot + "/.snapshot"
                        : std::string();
                    bool ok = DatabaseEngine::applySnapshot(payload, bundlePath);
                    if (ok) std::cout << "[RECOVERY] ✓ LSM snapshot applied during recovery" << std::endl;
                    else {
                        const uint64_t snapshotIndex =
                            payload.value("lastIncludedIndex", static_cast<uint64_t>(0));
                        const std::string error =
                            "LSM snapshot application failed during recovery";
                        std::cerr << "[RECOVERY] ✗ " << error << std::endl;
                        return failRecovery(
                            error, snapshotIndex, 0, "snapshot_restore", std::string(),
                            /*mutationMayHaveOccurred=*/true);
                    }
                    RECOV_TRACE("snapshot_apply_complete ok=" << (ok ? "true" : "false") << "\n");
                }
            } catch (const std::exception& error) {
                return failRecovery(
                    "Snapshot restore raised: " + std::string(error.what()),
                    snapshotData.value(
                        "lastIncludedIndex", static_cast<uint64_t>(0)),
                    0, "snapshot_restore", std::string(),
                    /*mutationMayHaveOccurred=*/true);
            } catch (...) {
                return failRecovery(
                    "Snapshot restore raised an unknown error",
                    snapshotData.value(
                        "lastIncludedIndex", static_cast<uint64_t>(0)),
                    0, "snapshot_restore", std::string(),
                    /*mutationMayHaveOccurred=*/true);
            }
        }
    }
    const auto snapshotRestoreMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - snapshotRestoreStart).count();
    MetricsExporter::recordCustomMetric("pacificdb_snapshot_restore_duration_ms", static_cast<double>(snapshotRestoreMs));

    // Phase 6: Consistency Check
    state.phase = "Phase 6: Consistency Check";
    std::cout << "\n[RECOVERY] " << state.phase << std::endl;
    if (!phase6_ConsistencyCheck(state.raftState, state.appliedLSN)) {
        const std::string error =
            "Consistency check failed: critical invariants violated";
        std::cerr << "[RECOVERY] ✗ " << error << std::endl;
        std::cerr << "[RECOVERY] Node will start in READ-ONLY mode" << std::endl;
        return failRecovery(error);
    }
    std::cout << "[RECOVERY] ✓ Consistency check passed" << std::endl;

    pacificdb::test::hitFailpoint(
        "FP_RECOVERY_BEFORE_COMPLETE_PUBLICATION", 1);
    state.success = true;
    state.phase = "Recovery Complete";
    std::cout << "\n[RECOVERY] ✓✓✓ Recovery complete - node ready for operation ✓✓✓" << std::endl;

    return state;
}

bool RecoveryManager::phase1_LockAndValidate(
    const std::string& dataRoot,
    bool wasCleanShutdown) {
    // Create directory if needed
    std::filesystem::create_directories(dataRoot);

    if (!wasCleanShutdown) {
        std::cout << "[RECOVERY] Last shutdown was UNCLEAN - crash recovery in progress" << std::endl;
    } else {
        std::cout << "[RECOVERY] Last shutdown was CLEAN" << std::endl;
    }

    // The process-wide StorageRootGuard is acquired in main before recovery
    // and remains held until shutdown. This phase verifies the guarded root.
    return std::filesystem::exists(dataRoot);
}

RecoveryManager::WALScanSummary RecoveryManager::phase2_WALScanAndTruncate(
    const std::string& dataRoot) {
    WALScanSummary summary;
    const auto walFiles = findWalFiles(dataRoot);
    for (const auto& walPath : walFiles) {
        auto scanned = WALIntegrity::scanAndTruncateWAL(walPath);
        summary.records += scanned.size();
        for (const auto& record : scanned) {
            summary.lastLsn = std::max(summary.lastLsn, record.lsn);
        }
    }
    return summary;
}

json RecoveryManager::phase4_RestoreRaftState(const std::string& raftLogPath) {
    // TODO: Read Raft log metadata
    json state;
    state["currentTerm"] = 0;
    state["votedFor"] = -1;
    state["commitIndex"] = 0;
    state["lastApplied"] = 0;

    // Check if file exists and parse
    std::ifstream raftLog(raftLogPath);
    if (raftLog) {
        try {
            raftLog >> state;
        } catch (...) {
            std::cout << "[RECOVERY] No valid Raft state file, starting fresh" << std::endl;
        }
    }

    raftLog.close();
    return state;
}

json RecoveryManager::phase5_SnapshotRestore(const std::string& dataRoot) {
    std::string snapshotPath = dataRoot + "/.snapshot";
    if (!std::filesystem::is_regular_file(snapshotPath)) {
        return json();  // No snapshot
    }

    try {
        json data;
        const bool bundled = pacificdb::snapshot_bundle::isBundle(snapshotPath);
        if (bundled) {
            uint64_t index = 0;
            uint64_t term = 0;
            std::string reason;
            if (!RaftCore::readPersistedSnapshotMetadata(
                    snapshotPath, index, term, reason)) {
                std::cerr << "[RECOVERY] Invalid snapshot (" << reason
                          << "), skipping snapshot" << std::endl;
                return json();
            }
            data = pacificdb::snapshot_bundle::read(snapshotPath).manifest;
        } else {
            std::ifstream snapshot(snapshotPath);
            snapshot >> data;
        }

        const int version = data.value("version", data.value("snapshot_format_version", 1));
        if (version != 1) {
            std::cerr << "[RECOVERY] Unsupported snapshot version " << version << ", skipping snapshot" << std::endl;
            return json();
        }

        if (data.contains("complete") && data["complete"].is_boolean() && !data["complete"].get<bool>()) {
            std::cerr << "[RECOVERY] Incomplete snapshot marker found, skipping snapshot" << std::endl;
            return json();
        }

        if (data.contains("lastIncludedIndex") && !data["lastIncludedIndex"].is_number_unsigned() && !data["lastIncludedIndex"].is_number_integer()) {
            std::cerr << "[RECOVERY] Invalid snapshot lastIncludedIndex metadata, skipping snapshot" << std::endl;
            return json();
        }

        if (data.contains("lastIncludedIndex")) {
            auto walMeta = WALIntegrity::getWALMetadata(dataRoot);
            const uint64_t snapshotIndex = data.value("lastIncludedIndex", 0UL);
            const uint64_t expectedSnapshotIndex = walMeta.value("snapshotLSN", 0UL);
            if (expectedSnapshotIndex > 0 && snapshotIndex < expectedSnapshotIndex) {
                std::cerr << "[RECOVERY] Stale snapshot index " << snapshotIndex
                          << " < metadata snapshotLSN " << expectedSnapshotIndex
                          << ", skipping snapshot" << std::endl;
                return json();
            }
        }

        std::string validationReason;
        const bool checksummed = bundled || data.contains("snapshot_checksum") ||
            data.contains("lsm_payload_checksum");
        if (checksummed &&
            !RaftCore::instance().validatePersistedSnapshot(data, validationReason)) {
            std::cerr << "[RECOVERY] Invalid snapshot (" << validationReason
                      << "), skipping snapshot" << std::endl;
            return json();
        }

        // A sender streams the exact durable file and verifies this sidecar
        // first. Generate it only after the snapshot's internal checks pass.
        if (bundled) return data;

        const std::string fileChecksum =
            pacificdb::durability::ChecksumCalculator::sha256File(snapshotPath);
        if (fileChecksum.empty()) {
            std::cerr << "[RECOVERY] Cannot checksum snapshot, skipping snapshot" << std::endl;
            return json();
        }
        const std::string checksumPath = snapshotPath + ".sha256";
        const std::string tmpChecksumPath = checksumPath + ".tmp";
        std::ofstream checksum(tmpChecksumPath, std::ios::trunc);
        checksum << fileChecksum << "\n";
        checksum.flush();
        checksum.close();
        if (!checksum) {
            std::filesystem::remove(tmpChecksumPath);
            return json();
        }
        std::error_code checksumEc;
        std::filesystem::rename(tmpChecksumPath, checksumPath, checksumEc);
        if (checksumEc) {
            std::filesystem::remove(tmpChecksumPath);
            return json();
        }

        return data;
    } catch (...) {
        return json();
    }
}

bool RecoveryManager::phase6_ConsistencyCheck(const json& raftState, size_t appliedLSN) {
    // Critical invariants:
    // 1. appliedLSN <= commitIndex
    // 2. votedFor is valid or -1
    // 3. currentTerm >= 0

    size_t commitIndex = raftState.value("commitIndex", 0UL);
    int votedFor = raftState.value("votedFor", -1);
    uint64_t currentTerm = raftState.value("currentTerm", 0UL);

    if (appliedLSN > commitIndex) {
        std::cerr << "[RECOVERY] INVARIANT VIOLATED: appliedLSN (" << appliedLSN
                  << ") > commitIndex (" << commitIndex << ")" << std::endl;
        return false;
    }

    if (votedFor < -1) {
        std::cerr << "[RECOVERY] INVARIANT VIOLATED: invalid votedFor (" << votedFor << ")" << std::endl;
        return false;
    }

    std::cout << "[RECOVERY] Invariants OK: appliedLSN=" << appliedLSN
              << ", commitIndex=" << commitIndex
              << ", currentTerm=" << currentTerm << std::endl;

    return true;
}
