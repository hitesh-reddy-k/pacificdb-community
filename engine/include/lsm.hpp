#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include "storage_path.hpp"
#include "storage_format_v2.hpp"

using json = nlohmann::json;

class RaftCore;

class LSM {
public:
    class LatestRowView {
    public:
        explicit LatestRowView(const json& row) : json_(&row) {}
        explicit LatestRowView(const pacificdb::storage_v2::MsgpackRowView& row)
            : msgpack_(&row) {}

        std::optional<json> field(std::string_view name) const;
        json materialize() const;

    private:
        const json* json_{nullptr};
        const pacificdb::storage_v2::MsgpackRowView* msgpack_{nullptr};
    };

    // initialize with engine data root
    static void init(const std::string& rootPath);

    // V11.4-PATH-001: single point of truth for storage path construction. Throws rather
    // than returning an empty root, because an empty root silently produces paths relative
    // to the process working directory and lets two engines share storage.
    static const std::string& rootOrThrow();

    // V11.4-PATH-002: user-controlled identifiers reach the filesystem. Each must be
    // exactly one safe path component. Unsafe values are rejected, never sanitized —
    // rewriting "../x" to "x" would collide two distinct client identifiers onto one
    // storage location.
    static std::filesystem::path requireContained(const std::filesystem::path& candidate);
    static std::filesystem::path databasePath(const std::string& userId,
                                              const std::string& dbName);
    static std::filesystem::path collectionArtifactPath(const std::string& userId,
                                                        const std::string& dbName,
                                                        const std::string& collection,
                                                        const std::string& suffix);

    // restore memtables from collection WAL files (best-effort)
    static void restoreFromWal();

    // put document into memtable (and WAL) and schedule flush
    static void put(const std::string& userId,
                    const std::string& dbName,
                    const std::string& collection,
                    const json& doc,
                    bool requireIndexCompletion = false,
                    std::uint64_t applyIndex = 0,
                    bool writeWal = true);

    // Batch put path used by insertMany. Returns write timing and flush metrics.
    static nlohmann::json putMany(const std::string& userId,
                                  const std::string& dbName,
                                  const std::string& collection,
                                  const std::vector<json>& docs,
                                  bool requireIndexCompletion = false,
                                  std::uint64_t applyIndex = 0,
                                  bool writeWal = true);

    // read all documents (merge memtable + SST files)
    static std::vector<json> getAll(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection);

    // Visit the newest visible version of each live document without building
    // a collection-sized map of nlohmann::json rows.
    static bool visitLatest(const std::string& userId,
                            const std::string& dbName,
                            const std::string& collection,
                            const std::function<bool(const LatestRowView&)>& visitor);

    // ⚡ NEW: Fast field-based lookup using indexes
    static std::vector<json> findByField(const std::string& userId,
                                         const std::string& dbName,
                                         const std::string& collection,
                                         const std::string& field,
                                         const std::string& value,
                                         size_t maxCandidates = 0);

    // VISIBILITY CLOSURE: Snapshot-bound strong read with memtable-first precedence.
    // Guarantees returned_version >= floor_version by:
    // 1. Completely bypassing caches (per-key ID cache, read cache, lease cache)
    // 2. Checking memtable FIRST (most recent writes)
    // 3. Then checking SSTs in reverse order (newest first)
    // 4. Enforcing strict per-key floor comparison
    // 5. Returning detailed visibility source telemetry per attempt
    static nlohmann::json findStrongSnapshotBound(
        const std::string& userId,
        const std::string& dbName,
        const std::string& collection,
        const std::string& id,
        uint64_t requiredFloorVersion);

    // Invalidate the hot in-memory id lookup for a key before strict reads.
    static void invalidateIdCache(const std::string& userId,
                                  const std::string& dbName,
                                  const std::string& collection,
                                  const std::string& id);

    // Read visibility tracing for per-attempt forensic telemetry.
    static void setLastReadVisibilitySource(const std::string& source);
    static std::string getLastReadVisibilitySource();
    static void clearLastReadVisibilitySource();

    // Visibility closure: bypass inMemoryIdIndex for strong/session reads.
    static void setSkipIdCache(bool skip);
    static void clearSkipIdCache();

    // force a flush for a specific collection (debug)
    static void flush(const std::string& userId,
                      const std::string& dbName,
                      const std::string& collection);

    // COMPACTION & MAINTENANCE
    // run compaction for a collection (tiered compaction)
    static void compact(const std::string& userId,
                        const std::string& dbName,
                        const std::string& collection);

    // Bloom filter support (adaptive)
    static void buildBloomForSST(const std::string& sstPath);
    static bool mayExistInSST(const std::string& sstPath, const std::string& key);

    // Columnar secondary index scaffold
    static void updateColumnIndexes(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection,
                                    const json& doc);
    static void updateColumnIndexesBatch(const std::string& userId,
                                         const std::string& dbName,
                                         const std::string& collection,
                                         const std::vector<json>& docs);
    static nlohmann::json validateColumnIndexes(const std::string& userId,
                                                const std::string& dbName,
                                                const std::string& collection);
    static nlohmann::json rebuildColumnIndexes(const std::string& userId,
                                               const std::string& dbName,
                                               const std::string& collection);
    static bool waitForColumnIndexUpdates(const std::string& userId,
                                          const std::string& dbName,
                                          const std::string& collection,
                                          int timeoutMs);
    static nlohmann::json createSecondaryIndex(const std::string& userId,
                                               const std::string& dbName,
                                               const std::string& collection,
                                               const nlohmann::json& definition);
    static nlohmann::json listSecondaryIndexes(const std::string& userId,
                                               const std::string& dbName,
                                               const std::string& collection);
    static nlohmann::json dropSecondaryIndex(const std::string& userId,
                                             const std::string& dbName,
                                             const std::string& collection,
                                             const std::string& name);
    static nlohmann::json rebuildSecondaryIndex(const std::string& userId,
                                                const std::string& dbName,
                                                const std::string& collection,
                                                const std::string& name);

    // start/stop background maintenance thread
    static void startBackgroundTasks();
    static void stopBackgroundTasks();

    // Force flush all memtables (for memory pressure management)
    static void forceFlush();

    // Runtime stats for admin/storage commands.
    static nlohmann::json getRuntimeStats();

    // v4.4H: LSM profiling metrics for /debug/lsm_profile endpoint
    static nlohmann::json getLsmMetrics();

    // Snapshot export/apply for Raft install_snapshot: export LSM files into a JSON
    // payload that can be sent to followers and applied atomically.
    static nlohmann::json exportSnapshotJson(uint64_t lastIncludedIndex);

private:
    friend class RaftCore;
    // Capture is internal because discardSnapshotFiles removes its argument.
    // Keep callers constrained to the Raft snapshot coordinator.
    static std::filesystem::path pinSnapshotFiles(uint64_t lastIncludedIndex);
    static void discardSnapshotFiles(const std::filesystem::path& pinnedRoot) noexcept;
    static nlohmann::json exportSnapshotJson(
        uint64_t lastIncludedIndex,
        const std::filesystem::path& pinnedRoot);
    static nlohmann::json exportSnapshotBundleManifest(
        uint64_t lastIncludedIndex,
        const std::filesystem::path& pinnedRoot,
        std::vector<std::filesystem::path>& artifactSources);
    static nlohmann::json exportSnapshotJson(
        uint64_t lastIncludedIndex,
        const std::filesystem::path& pinnedRoot,
        std::vector<std::filesystem::path>* artifactSources);

public:
    static bool applySnapshot(
        const nlohmann::json& payload,
        const std::filesystem::path& bundlePath = {});

    // ═══ V11.4-IDX-001: snapshot index recovery ═══
    // A snapshot restores base data only. Column-index catalog definitions travel in the
    // snapshot's index_catalog section; their CONTENTS are rebuilt locally from raw
    // unfiltered base records after installation. Until that rebuild is durably verified
    // the node is not recovered: readiness and strong reads stay withheld, because a
    // collection whose indexes are missing would otherwise validate as healthy (an empty
    // catalog makes the validator iterate zero fields and report a false clean).
    enum class IndexRecoveryState {
        NONE,                       // no snapshot-driven index recovery pending
        SNAPSHOT_BASE_RESTORING,
        SNAPSHOT_BASE_RESTORED,
        INDEX_CATALOG_RESTORED,
        INDEX_REBUILD_REQUIRED,
        INDEX_REBUILDING,
        INDEX_REBUILD_VERIFYING,
        INDEX_REBUILD_COMPLETE,
        FAILED,
    };
    static const char* indexRecoveryStateName(IndexRecoveryState state);
    static IndexRecoveryState indexRecoveryState();
    // True when no snapshot index rebuild is outstanding (NONE or COMPLETE).
    static bool indexRecoverySettled();
    static nlohmann::json indexRecoveryStatus();
    // Rebuilds every declared index from raw unfiltered base records into a temporary
    // generation, verifies it, then publishes it atomically. Safe to call repeatedly.
    static bool runPendingIndexRebuild();

    // delete helper: write a tombstone entry for an id and log a DELETE in collection WAL
    static void del(const std::string& userId,
                    const std::string& dbName,
                    const std::string& collection,
                    const std::string& id);

    // Permanently remove one collection's in-memory state and durable LSM,
    // WAL, and index artifacts. The caller is responsible for replicating the
    // schema operation before invoking this state-machine method.
    static bool dropCollection(const std::string& userId,
                               const std::string& dbName,
                               const std::string& collection);
};
