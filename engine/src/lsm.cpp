#include "lsm.hpp"
#include "database_engine.hpp"
#include "wal.hpp"
#include "raft_core.hpp"
#include "request_timing.hpp"
#include "metrics_exporter.hpp"
#include "connection_pool.hpp"
#include "test_failpoint.hpp"
#include "data_durability.hpp"
#include "lsm_manifest.hpp"
#include "storage_format_v2.hpp"
#include "snapshot_bundle.hpp"
#include <openssl/evp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <thread>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <functional>
#include <unordered_set>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include <set>
#include <array>
#include <atomic>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <optional>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <iterator>

namespace fs = std::filesystem;

static std::string encodeBase64(const std::string& bytes) {
    if (bytes.empty()) return {};
    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()),
        static_cast<int>(bytes.size()));
    if (size < 0) throw std::runtime_error("base64 encoding failed");
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}

static std::string decodeBase64(const std::string& encoded) {
    if (encoded.empty()) return {};
    if (encoded.size() % 4 != 0) throw std::runtime_error("invalid base64 snapshot payload");
    std::string decoded(3 * (encoded.size() / 4), '\0');
    int size = EVP_DecodeBlock(
        reinterpret_cast<unsigned char*>(decoded.data()),
        reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size()));
    if (size < 0) throw std::runtime_error("invalid base64 snapshot payload");
    if (!encoded.empty() && encoded.back() == '=') --size;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') --size;
    decoded.resize(static_cast<std::size_t>(size));
    return decoded;
}

// Production: Use per-collection locks instead of global lock for parallelism
static std::shared_mutex lsm_global_mutex;  // For memtables map access
static std::unordered_map<std::string, std::unique_ptr<std::shared_mutex>> collection_mutexes;
static std::mutex collection_mutex_map_lock;

// Separate per-collection mutex for column index file I/O.  The main collection mutex
// serializes memtable/WAL access (~0.2ms hold). Column index filesystem I/O (~8ms) must
// NOT hold the collection mutex, but still needs per-collection serialization to prevent
// concurrent JSON file corruption (SIGFPE on bad parse).
// Shared mutex so reads can proceed concurrently; only exclusive-locked during index writes.
static std::unordered_map<std::string, std::unique_ptr<std::shared_mutex>> col_idx_mutexes;
static std::mutex col_idx_mutex_map_lock;

static std::shared_mutex& getColIdxMutex(const std::string& key) {
    std::lock_guard<std::mutex> lk(col_idx_mutex_map_lock);
    auto it = col_idx_mutexes.find(key);
    if (it == col_idx_mutexes.end()) {
        col_idx_mutexes[key] = std::make_unique<std::shared_mutex>();
        it = col_idx_mutexes.find(key);
    }
    return *it->second;
}
static thread_local std::string g_lastReadVisibilitySource;
static thread_local bool g_skipIdCacheForStrongRead = false;
static thread_local bool g_collectionWalReplay = false;

// Fix 2: Per-key session floor tracking and invalidation
static std::mutex g_sessionFloorMapMutex;
static std::unordered_map<std::string, uint64_t> g_sessionFloorVersions;  // key -> floor version

static bool isStrongReadIteratorPoolingDisabled() {
    const char* v = std::getenv("DISABLE_STRONG_READ_ITERATOR_POOLING");
    if (!v) return false;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE";
}

static std::shared_mutex& getCollectionMutex(const std::string& key) {
    auto lock = pacificdb::timing::makeTimedUniqueLock(collection_mutex_map_lock, "pacificdb_lock_collection_mutex_map");
    auto it = collection_mutexes.find(key);
    if (it == collection_mutexes.end()) {
        collection_mutexes[key] = std::make_unique<std::shared_mutex>();
        it = collection_mutexes.find(key);
    }
    return *it->second;
}

// Readers use the collection lock for a stable file set. The generation lets
// them retry lock acquisition only when a publish actually completed, without
// polling the read path while compaction waits for that lock.
static std::array<std::atomic<uint64_t>, 256> g_compactionPublishGeneration{};

static std::atomic<uint64_t>& compactionPublishGeneration(const std::string& key) {
    return g_compactionPublishGeneration[std::hash<std::string>{}(key) %
                                         g_compactionPublishGeneration.size()];
}

static std::unique_ptr<std::shared_lock<std::shared_mutex>> stableCollectionReadLock(
    const std::string& key) {
    auto& generation = compactionPublishGeneration(key);
    for (;;) {
        const auto before = generation.load(std::memory_order_acquire);
        auto lock = std::make_unique<std::shared_lock<std::shared_mutex>>(
            getCollectionMutex(key));
        if (before == generation.load(std::memory_order_acquire)) return lock;
    }
}

static bool isFindDeepLogEnabled() {
    const char* v = std::getenv("FIND_DEEP_LOG");
    if (!v) return false;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE";
}

static bool isFindLockBypassEnabled() {
    const char* v = std::getenv("FIND_DISABLE_LOCKS");
    if (!v) return false;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE";
}

static std::mutex g_findLogMutex;

#define FIND_LOG(msg) do { if (isFindDeepLogEnabled()) { std::lock_guard<std::mutex> _lk(g_findLogMutex); std::cerr << msg << std::endl; } } while(0)
#define FIND_STEP(n, msg) FIND_LOG("[FIND] " << n << ": " << msg)

// v2.9R: per-put/per-flush stdout logging is catastrophic under sustained load
// (synchronised std::cout on the hot path). Gate it behind LSM_VERBOSE, cached
// once so getenv isn't called on every write.
static const bool g_lsmVerbose = []() {
    const char* v = std::getenv("LSM_VERBOSE");
    if (!v) return false;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE";
}();
#define LSM_LOG(msg) do { if (g_lsmVerbose) { std::cout << msg << std::endl; } } while(0)

// memtable keyed by collectionPath -> map<id,json>
using Memtable = std::unordered_map<std::string, json>;
static std::unordered_map<std::string, Memtable> memtables;
// v5.5P-R4.2: immutable memtables are O(1)-swapped out of the foreground
// active memtable during flush. They remain visible to reads until the SST is
// durable, removing the old O(n) deep-copy stall under the collection lock.
static std::unordered_map<std::string, std::deque<std::shared_ptr<Memtable>>> immutableMemtables;

struct WalApplyProgress {
    uint64_t contiguous{0};
    std::map<uint64_t, uint64_t> intervals;
};
static std::unordered_map<std::string, WalApplyProgress> g_walApplyProgress;

// Called only while holding the collection mutex.
static void markWalApplied(const std::string& key, uint64_t first, uint64_t last) {
    if (first == 0 || last < first) return;
    auto& progress = g_walApplyProgress[key];
    if (last <= progress.contiguous) return;
    first = std::max(first, progress.contiguous + 1);
    auto it = progress.intervals.lower_bound(first);
    if (it != progress.intervals.begin()) {
        auto previous = std::prev(it);
        if (previous->second >= first - 1) {
            first = previous->first;
            last = std::max(last, previous->second);
            it = progress.intervals.erase(previous);
        }
    }
    while (it != progress.intervals.end() && it->first <= last + 1) {
        last = std::max(last, it->second);
        it = progress.intervals.erase(it);
    }
    progress.intervals[first] = last;
    while (!progress.intervals.empty() &&
           progress.intervals.begin()->first <= progress.contiguous + 1) {
        progress.contiguous = std::max(progress.contiguous,
                                       progress.intervals.begin()->second);
        progress.intervals.erase(progress.intervals.begin());
    }
}
static std::mutex g_flushStateMutex;
static std::unordered_set<std::string> g_collectionsFlushing;
struct FlushTask {
    std::string userId;
    std::string dbName;
    std::string collection;
    std::string key;
};
static std::mutex g_flushQueueMutex;
static std::condition_variable g_flushQueueCv;
static std::deque<FlushTask> g_flushQueue;
static std::unordered_set<std::string> g_flushQueuedKeys;
static std::vector<std::thread> g_flushWorkers;
static std::atomic<uint64_t> g_flushQueuedTotal{0};
static std::atomic<uint64_t> g_flushCompletedTotal{0};
static std::atomic<uint64_t> g_flushSkippedTotal{0};
static std::atomic<uint64_t> g_lastFlushWaitUs{0};
static std::atomic<bool> bgRunning(false);
static std::atomic<uint64_t> g_columnIndexHits{0};
static std::atomic<uint64_t> g_columnIndexAuthoritativeEmpty{0};
static std::atomic<uint64_t> g_columnIndexFallbackScans{0};
static std::atomic<uint64_t> g_columnIndexStaleReads{0};
static std::atomic<uint64_t> g_columnIndexSinglePassMissingSidecar{0};
static std::atomic<uint64_t> g_sstSidecarsRepaired{0};
static std::atomic<uint64_t> g_sstSidecarRepairFailures{0};

// ─── v3.0R-G: parsed column-index cache ──────────────────────────────────────
// findByField() previously re-read AND re-parsed the on-disk <field>.json index
// file on EVERY query — the dominant read-P99 cost under concurrency. This caches
// the parsed index keyed by file path, guarded by (size,mtime): ANY write to the
// file (applyColumnIndexBatch, compaction rebuild, drop) changes mtime and is
// detected, so a cached read can never be stale. Bounded by LSM_COLINDEX_CACHE_MAX_BYTES.
struct ColIndexCacheEntry {
    nlohmann::json parsed;
    uintmax_t size = 0;
    int64_t   mtime = 0;
    size_t    bytes = 0;
    uint64_t  lru = 0;
};
static std::mutex g_colIdxCacheMutex;
static std::unordered_map<std::string, ColIndexCacheEntry> g_colIdxCache;
static std::atomic<uint64_t> g_colIdxCacheHits{0};
static std::atomic<uint64_t> g_colIdxCacheMisses{0};
static std::atomic<uint64_t> g_colIdxCacheEvictions{0};
static std::atomic<uint64_t> g_colIdxCacheBytes{0};
static std::atomic<uint64_t> g_colIdxCacheLru{0};
static std::mutex g_colIdxTimingMutex;
static std::vector<double> g_colIdxParseMs;
static std::vector<double> g_colIdxLookupMs;

struct BinaryColumnIndexState {
    std::mutex mutex;
    bool loaded = false;
    std::unordered_map<std::string, std::string> valueById;
    std::unordered_map<std::string, std::unordered_set<std::string>> idsByValue;
};
static std::mutex g_binaryColIdxCacheMutex;
static std::unordered_map<std::string, std::shared_ptr<BinaryColumnIndexState>> g_binaryColIdxCache;
static std::atomic<uint64_t> g_binaryColIdxSegmentsWritten{0};
static std::atomic<uint64_t> g_binaryColIdxCompactions{0};
static std::atomic<uint64_t> g_binaryColIdxBytesWritten{0};

static void recordColIdxTiming(std::vector<double>& v, double ms) {
    std::lock_guard<std::mutex> lk(g_colIdxTimingMutex);
    if (v.size() < 4096) v.push_back(ms);
    else v[g_colIdxCacheLru.load(std::memory_order_relaxed) % 4096] = ms;
}
static double colIdxTimingP99(std::vector<double>& v) {
    std::lock_guard<std::mutex> lk(g_colIdxTimingMutex);
    if (v.empty()) return 0.0;
    std::vector<double> s(v);
    std::sort(s.begin(), s.end());
    return s[std::min(s.size() - 1, (size_t)std::ceil(s.size() * 0.99) - 1)];
}
static size_t colIdxCacheMaxBytes() {
    static const size_t v = []() -> size_t {
        const char* e = std::getenv("LSM_COLINDEX_CACHE_MAX_BYTES");
        if (e && *e) { try { return (size_t)std::stoull(e); } catch (...) {} }
        return 64ULL * 1024 * 1024; // 64MB default
    }();
    return v;
}
static int64_t fileMtimeCount(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return -1;
    return (int64_t)t.time_since_epoch().count();
}

// Look up candidate ids for (idxFile, value), capped at candidateLimit, via the cache.
// Sets fileExists / valueHit; fills idsOut. Returns true if the index file was consulted.
static bool columnIndexLookupCached(const fs::path& idxFile, const std::string& value,
                                    size_t candidateLimit, std::vector<std::string>& idsOut,
                                    bool& fileExists, bool& valueHit) {
    auto t0 = std::chrono::steady_clock::now();
    fileExists = false; valueHit = false;
    std::error_code ec;
    if (!fs::exists(idxFile, ec) || ec) return false;
    fileExists = true;
    uintmax_t sz = fs::file_size(idxFile, ec); if (ec) sz = 0;
    int64_t mt = fileMtimeCount(idxFile);
    const std::string key = idxFile.string();

    auto extract = [&](const nlohmann::json& idx) {
        auto vIt = idx.find(value);
        if (vIt != idx.end() && vIt->is_array()) {
            valueHit = true;
            for (const auto& v : *vIt) {
                try { idsOut.push_back(v.is_string() ? v.get<std::string>() : v.dump()); } catch (...) {}
                if (candidateLimit > 0 && idsOut.size() >= candidateLimit) break;
            }
        }
    };

    bool needLoad = false;
    {
        std::lock_guard<std::mutex> lk(g_colIdxCacheMutex);
        auto it = g_colIdxCache.find(key);
        if (it != g_colIdxCache.end() && it->second.size == sz && it->second.mtime == mt) {
            it->second.lru = g_colIdxCacheLru.fetch_add(1, std::memory_order_relaxed);
            extract(it->second.parsed);
            g_colIdxCacheHits.fetch_add(1, std::memory_order_relaxed);
        } else {
            needLoad = true;
        }
    }
    if (needLoad) {
        // Miss: read + parse OUTSIDE the cache lock so collections don't serialize.
        auto p0 = std::chrono::steady_clock::now();
        nlohmann::json idx = nlohmann::json::object();
        try { std::ifstream in(key); if (in.is_open()) in >> idx; } catch (...) { idx = nlohmann::json::object(); }
        recordColIdxTiming(g_colIdxParseMs,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count());
        extract(idx);
        const size_t approxBytes = key.size() + (size_t)sz;
        {
            std::lock_guard<std::mutex> lk(g_colIdxCacheMutex);
            auto existing = g_colIdxCache.find(key);
            if (existing != g_colIdxCache.end())
                g_colIdxCacheBytes.fetch_sub(existing->second.bytes, std::memory_order_relaxed);
            ColIndexCacheEntry e;
            e.parsed = std::move(idx); e.size = sz; e.mtime = mt; e.bytes = approxBytes;
            e.lru = g_colIdxCacheLru.fetch_add(1, std::memory_order_relaxed);
            g_colIdxCacheBytes.fetch_add(approxBytes, std::memory_order_relaxed);
            g_colIdxCache[key] = std::move(e);
            const size_t budget = colIdxCacheMaxBytes();
            while (g_colIdxCacheBytes.load(std::memory_order_relaxed) > budget && g_colIdxCache.size() > 1) {
                auto victim = g_colIdxCache.end(); uint64_t best = UINT64_MAX;
                for (auto i2 = g_colIdxCache.begin(); i2 != g_colIdxCache.end(); ++i2)
                    if (i2->first != key && i2->second.lru < best) { best = i2->second.lru; victim = i2; }
                if (victim == g_colIdxCache.end()) break;
                g_colIdxCacheBytes.fetch_sub(victim->second.bytes, std::memory_order_relaxed);
                g_colIdxCache.erase(victim);
                g_colIdxCacheEvictions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_colIdxCacheMisses.fetch_add(1, std::memory_order_relaxed);
    }
    recordColIdxTiming(g_colIdxLookupMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}
// O(1) in-memory read index keyed by collectionPath -> id -> doc.
// v5.5P-R6.10: STRIPED locking. A single global shared_mutex starved follower reads (shared
// lookups) behind the ~1500 writes/s apply stream (unique upserts) at 500 users/shard — read
// P99 climbed to ~2.8s while an idle read is ~2ms. Striping by hash(key,id) spreads reads and
// writes across N independent locks, so a read only contends with applies that hash to the
// same stripe (~1/N of them). Each stripe owns its own map shard, so the outer maps never race.
static constexpr size_t ID_INDEX_STRIPES = 64;
struct IdIndexStripe {
    std::shared_mutex mu;
    std::unordered_map<std::string, std::unordered_map<std::string, json>> map;
};
static IdIndexStripe g_idIndexStripes[ID_INDEX_STRIPES];
static inline IdIndexStripe& idIndexStripeFor(const std::string& key, const std::string& id) {
    size_t h = (std::hash<std::string>{}(key) * 1099511628211ULL) ^ std::hash<std::string>{}(id);
    return g_idIndexStripes[h % ID_INDEX_STRIPES];
}

static std::string colKey(const std::string& userId, const std::string& db, const std::string& coll);

// v3.0 perf: Centralized async column index batcher.
// Replaces per-insert detached threads (each taking exclusive col-idx lock for 8ms of disk I/O)
// with a single background thread that accumulates writes and applies them in batches every 5ms.
// This eliminates the thundering-herd of exclusive lock acquisitions that blocked findByField
// shared-lock readers (root cause of P99=8-9s at 1000c).
struct IndexPendingEntry {
    std::string userId, dbName, collection;
    std::vector<json> docs; // always a batch (single puts add 1 doc)
};
static std::vector<IndexPendingEntry> g_idxPendingQueue;
static std::mutex g_idxPendingMutex;
static std::condition_variable g_idxPendingCv;
static bool g_idxCompletionRequested = false; // guarded by g_idxPendingMutex
static std::unordered_map<std::string, size_t> g_idxPendingByCollection;
static std::unordered_set<std::string> g_idxCollectionsProcessing;
static std::atomic<bool> g_idxBatcherRunning{false};
static std::thread g_idxBatcherThread;

static void enqueueIndexUpdate(const std::string& userId, const std::string& dbName,
                               const std::string& collection, std::vector<json> docs,
                               bool requireCompletion) {
    std::lock_guard<std::mutex> lk(g_idxPendingMutex);
    g_idxPendingByCollection[colKey(userId, dbName, collection)] += docs.size();
    g_idxPendingQueue.push_back({userId, dbName, collection, std::move(docs)});
    if (requireCompletion) {
        g_idxCompletionRequested = true;
        g_idxPendingCv.notify_all();
    }
}

// V11.4-IDX-001 diagnostics: exact pending/processing depth for one collection, so a
// validation result can state whether asynchronous indexing was still in flight.
static nlohmann::json columnIndexQueueState(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_idxPendingMutex);
    const auto it = g_idxPendingByCollection.find(key);
    return nlohmann::json{
        {"pendingDocs", it == g_idxPendingByCollection.end() ? 0 : it->second},
        {"processing", g_idxCollectionsProcessing.find(key) != g_idxCollectionsProcessing.end()},
        {"globalQueuedBatches", g_idxPendingQueue.size()},
        {"batcherRunning", g_idxBatcherRunning.load(std::memory_order_acquire)},
    };
}

static bool hasPendingIndexUpdates(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_idxPendingMutex);
    auto it = g_idxPendingByCollection.find(key);
    return it != g_idxPendingByCollection.end() && it->second > 0;
}

bool LSM::waitForColumnIndexUpdates(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection,
                                    int timeoutMs) {
    const std::string key = colKey(userId, dbName, collection);
    std::unique_lock<std::mutex> lock(g_idxPendingMutex);
    const auto complete = [&]() {
        const auto pending = g_idxPendingByCollection.find(key);
        const bool queued = pending != g_idxPendingByCollection.end() && pending->second > 0;
        return !queued && g_idxCollectionsProcessing.find(key) == g_idxCollectionsProcessing.end();
    };
    if (complete()) return true;
    return g_idxPendingCv.wait_for(
        lock,
        std::chrono::milliseconds(std::max(1, timeoutMs)),
        complete);
}

static size_t ID_INDEX_MAX_PER_COLLECTION = []() {
    const char* v = std::getenv("FIND_ID_INDEX_MAX");
    if (v) {
        try { return static_cast<size_t>(std::stoul(v)); } catch (...) {}
    }
    return static_cast<size_t>(50000);
}();

static size_t idIndexMaxPerStripePerCollection() {
    // Each stripe owns a shard of every collection, so applying the full cap to
    // every stripe multiplied retained documents (and memory) by 64.
    return std::max<size_t>(1, ID_INDEX_MAX_PER_COLLECTION / ID_INDEX_STRIPES);
}

static void upsertIdIndex(const std::string& key, const std::string& id, const json& doc) {
    if (id.empty()) return;
    auto& stripe = idIndexStripeFor(key, id);
    auto lk = pacificdb::timing::makeTimedUniqueLock(stripe.mu, "pacificdb_lock_in_memory_id_index");
    auto& m = stripe.map[key];
    const size_t stripeLimit = idIndexMaxPerStripePerCollection();
    if (m.find(id) == m.end()) {
        // Evict before insertion: unordered_map::begin() can be the new document,
        // making every post-apply visibility check scan SSTs once the cache fills.
        // ponytail: arbitrary old victim; use LRU if read hit rate needs improvement.
        while (m.size() >= stripeLimit) {
            m.erase(m.begin());
        }
    }
    m[id] = doc;
}

static size_t idIndexEntryCount() {
    size_t total = 0;
    for (auto& stripe : g_idIndexStripes) {
        auto lk = pacificdb::timing::makeTimedSharedLock(
            stripe.mu, "pacificdb_lock_in_memory_id_index");
        for (const auto& [key, entries] : stripe.map) {
            (void)key;
            total += entries.size();
        }
    }
    return total;
}

static void removeIdIndex(const std::string& key, const std::string& id) {
    if (id.empty()) return;
    auto& stripe = idIndexStripeFor(key, id);
    auto lk = pacificdb::timing::makeTimedUniqueLock(stripe.mu, "pacificdb_lock_in_memory_id_index");
    auto it = stripe.map.find(key);
    if (it == stripe.map.end()) return;
    it->second.erase(id);
    if (it->second.empty()) {
        stripe.map.erase(it);
    }
}

static std::optional<json> lookupIdIndex(const std::string& key, const std::string& id) {
    if (id.empty()) return std::nullopt;
    auto& stripe = idIndexStripeFor(key, id);
    auto lk = pacificdb::timing::makeTimedSharedLock(stripe.mu, "pacificdb_lock_in_memory_id_index");
    auto it = stripe.map.find(key);
    if (it == stripe.map.end()) return std::nullopt;
    auto dit = it->second.find(id);
    if (dit == it->second.end()) return std::nullopt;
    return dit->second;
}

static bool isDeletedDoc(const json& doc) {
    return doc.is_object() &&
           doc.contains("_deleted") &&
           doc["_deleted"].is_boolean() &&
           doc["_deleted"].get<bool>();
}

static bool tryBeginCollectionFlush(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_flushStateMutex);
    if (g_collectionsFlushing.find(key) != g_collectionsFlushing.end()) return false;
    g_collectionsFlushing.insert(key);
    return true;
}

static void finishCollectionFlush(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_flushStateMutex);
    g_collectionsFlushing.erase(key);
}

struct CollectionFlushGuard {
    std::string key;
    bool active{false};
    explicit CollectionFlushGuard(std::string k) : key(std::move(k)), active(tryBeginCollectionFlush(key)) {}
    ~CollectionFlushGuard() {
        if (active) finishCollectionFlush(key);
    }
};

static size_t envSizeT(const char* name, size_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::max<size_t>(1, static_cast<size_t>(std::stoull(v))); } catch (...) { return fallback; }
}

static size_t maxConcurrentFlushes() {
    return envSizeT("MAX_CONCURRENT_FLUSHES", envSizeT("LSM_FLUSH_THREADS", 2));
}

static size_t flushThreadCount() {
    return std::min(envSizeT("LSM_FLUSH_THREADS", 2), maxConcurrentFlushes());
}

static bool enqueueBackgroundFlush(const std::string& userId,
                                   const std::string& dbName,
                                   const std::string& collection) {
    const std::string key = colKey(userId, dbName, collection);
    {
        std::lock_guard<std::mutex> lock(g_flushQueueMutex);
        if (g_flushQueuedKeys.find(key) != g_flushQueuedKeys.end()) {
            return false;
        }
        g_flushQueue.push_back(FlushTask{userId, dbName, collection, key});
        g_flushQueuedKeys.insert(key);
        g_flushQueuedTotal.fetch_add(1, std::memory_order_relaxed);
    }
    g_flushQueueCv.notify_one();
    return true;
}

static size_t flushQueueDepth() {
    std::lock_guard<std::mutex> lock(g_flushQueueMutex);
    return g_flushQueue.size();
}

static void flushWorkerLoop(size_t workerId) {
    while (bgRunning.load()) {
        FlushTask task;
        {
            std::unique_lock<std::mutex> lock(g_flushQueueMutex);
            g_flushQueueCv.wait(lock, []() {
                return !bgRunning.load() || !g_flushQueue.empty();
            });
            if (!bgRunning.load() && g_flushQueue.empty()) return;
            task = std::move(g_flushQueue.front());
            g_flushQueue.pop_front();
            g_flushQueuedKeys.erase(task.key);
        }

        auto started = std::chrono::steady_clock::now();
        try {
            LSM::flush(task.userId, task.dbName, task.collection);
            g_flushCompletedTotal.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            g_flushSkippedTotal.fetch_add(1, std::memory_order_relaxed);
        }
        auto finished = std::chrono::steady_clock::now();
        g_lastFlushWaitUs.store(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count()),
            std::memory_order_relaxed);
        (void)workerId;
    }
}

static bool extractDocVersionScore(const json& doc, long long& out) {
    if (!doc.is_object()) return false;
    // Application documents may own a business field named `version`. MVCC
    // visibility must always use the engine-owned version when it is present.
    const char* fields[] = {"_mvcc_version", "version", "deleted_txn", "created_txn", "_timestamp"};
    for (const char* field : fields) {
        if (!doc.contains(field)) continue;
        try {
            if (doc[field].is_number_integer() || doc[field].is_number_unsigned()) {
                out = doc[field].get<long long>();
                return true;
            }
            if (doc[field].is_string()) {
                out = std::stoll(doc[field].get<std::string>());
                return true;
            }
        } catch (...) {}
    }
    return false;
}

static bool isCandidateNewer(const json& candidate, const json& current) {
    long long candidateVersion = std::numeric_limits<long long>::min();
    long long currentVersion = std::numeric_limits<long long>::min();
    const bool candidateHasVersion = extractDocVersionScore(candidate, candidateVersion);
    const bool currentHasVersion = extractDocVersionScore(current, currentVersion);

    if (candidateHasVersion && currentHasVersion && candidateVersion != currentVersion) {
        return candidateVersion > currentVersion;
    }
    if (candidateHasVersion != currentHasVersion) {
        return candidateHasVersion;
    }

    // Equal or missing versions: later scan order wins. SSTs are scanned in
    // filename order and memtable entries are applied last.
    return true;
}

static void mergeLatestById(std::unordered_map<std::string, json>& latestById,
                            std::vector<json>& noIdDocs,
                            const json& doc) {
    if (!doc.is_object() || !doc.contains("id")) {
        noIdDocs.push_back(doc);
        return;
    }

    std::string id;
    try {
        id = doc["id"].is_string() ? doc["id"].get<std::string>() : doc["id"].dump();
    } catch (...) {
        noIdDocs.push_back(doc);
        return;
    }
    if (id.empty()) {
        noIdDocs.push_back(doc);
        return;
    }

    auto it = latestById.find(id);
    if (it == latestById.end() || isCandidateNewer(doc, it->second)) {
        latestById[id] = doc;
    }
}

static void clearIdIndexForCollection(const std::string& key) {
    // a collection's ids are spread across all stripes -> erase from each.
    for (auto& stripe : g_idIndexStripes) {
        auto lk = pacificdb::timing::makeTimedUniqueLock(stripe.mu, "pacificdb_lock_in_memory_id_index");
        stripe.map.erase(key);
    }
}

static void clearAllIdIndex() {
    for (auto& stripe : g_idIndexStripes) {
        auto lk = pacificdb::timing::makeTimedUniqueLock(stripe.mu, "pacificdb_lock_in_memory_id_index");
        stripe.map.clear();
    }
}

// Fix 2: Invalidate per-key session floor on memtable flush
static void invalidateSessionFloor(const std::string& key) {
    auto lk = pacificdb::timing::makeTimedUniqueLock(g_sessionFloorMapMutex, "pacificdb_lock_session_floor_map");
    auto it = g_sessionFloorVersions.find(key);
    if (it != g_sessionFloorVersions.end()) {
        g_sessionFloorVersions.erase(it);
    }
}

static void clearAllSessionFloors() {
    auto lk = pacificdb::timing::makeTimedUniqueLock(g_sessionFloorMapMutex, "pacificdb_lock_session_floor_map");
    g_sessionFloorVersions.clear();
}

void LSM::invalidateIdCache(const std::string& userId,
                            const std::string& dbName,
                            const std::string& collection,
                            const std::string& id) {
    removeIdIndex(colKey(userId, dbName, collection), id);
}

void LSM::setLastReadVisibilitySource(const std::string& source) {
    g_lastReadVisibilitySource = source;
}

std::string LSM::getLastReadVisibilitySource() {
    return g_lastReadVisibilitySource;
}

void LSM::clearLastReadVisibilitySource() {
    g_lastReadVisibilitySource.clear();
}

void LSM::setSkipIdCache(bool skip) {
    g_skipIdCacheForStrongRead = skip;
}

void LSM::clearSkipIdCache() {
    g_skipIdCacheForStrongRead = false;
}

static bool isTruthyEnv(const char* v) {
    if (!v) return false;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on";
}

static bool inMemoryOltpModeEnabled() {
    return isTruthyEnv(std::getenv("ENGINE_IN_MEMORY_OLTP")) ||
           isTruthyEnv(std::getenv("IN_MEMORY_OLTP"));
}

static std::string LSM_ROOT;
// memtable limit: default 10000 entries for high throughput
static size_t MEMTABLE_LIMIT = []() {
    const char* v = std::getenv("LSM_MEMTABLE_LIMIT");
    if (v) {
        try { return static_cast<size_t>(std::stoul(v)); } catch (...) { }
    }

    if (inMemoryOltpModeEnabled()) {
        // Keep hot OLTP working set in memory longer before flushing to SST.
        return static_cast<size_t>(200000);
    }

    return static_cast<size_t>(10000);  // ⚡ Production: larger memtable for batching
}();
static std::atomic<uint64_t> sstFileSequence{0};

static size_t compactionFanout() {
    static const size_t value = [] {
        if (const char* raw = std::getenv("LSM_COMPACTION_FANOUT")) {
            try { return std::max<size_t>(2, std::stoull(raw)); } catch (...) {}
        }
        return size_t{8};
    }();
    return value;
}

// v2.9R — write stall: hard cap on a single collection's active memtable. When
// the memtable grows past this, the writer is paced (stalled) until a flush
// drains it below the soft limit, so a hot collection can't grow unbounded and
// OOM the process. Absolute cap (env LSM_MEMTABLE_HARD_LIMIT) takes precedence.
static std::atomic<uint64_t> g_writeStallsTotal{0};
static std::atomic<uint64_t> g_writeStallWaitUs{0};

// v4.4H — LSM profiling metrics (Phase 5-9)
static std::atomic<uint64_t> g_compactionActiveCount{0};     // in-flight compactions
static std::atomic<uint64_t> g_compactionSweepsTotal{0};     // bg maintenance sweeps
static std::atomic<uint64_t> g_compactionRunsTotal{0};       // actual compact() calls that merged
static std::atomic<uint64_t> g_compactionThrottledTotal{0};  // times compaction yielded to reads
static std::atomic<uint64_t> g_compactionYieldMsTotal{0};    // total ms spent yielding
static std::atomic<uint64_t> g_compactionBytesWritten{0};    // bytes written by compaction
static std::atomic<uint64_t> g_compactionBytesRead{0};       // bytes read by compaction
static std::atomic<uint64_t> g_bytesIngested{0};             // logical MessagePack bytes applied
static std::atomic<uint64_t> g_compactionTombstonesObserved{0};
static std::atomic<uint64_t> g_compactionTombstonesReclaimed{0};
static std::atomic<uint64_t> g_compactionReclaimedBytes{0};
static std::atomic<uint64_t> g_sstScansPerReadTotal{0};      // cumulative SSTs read by getAll()
static std::atomic<uint64_t> g_sstScansPerReadCount{0};      // number of getAll() calls counted
static std::atomic<uint64_t> g_flushActiveCount{0};          // in-flight flushes
static std::atomic<uint64_t> g_flushDurationMsTotal{0};      // total flush duration ms
static std::atomic<uint64_t> g_flushBytesWritten{0};         // bytes written by flushes
static std::atomic<uint64_t> g_readExecuteMsTotal{0};        // total getAll() execution ms
static std::atomic<uint64_t> g_readExecuteCount{0};          // number of timed getAll() calls
static std::atomic<uint64_t> g_readMemtableHits{0};          // reads that found data in memtable
static std::atomic<uint64_t> g_readSstHits{0};               // reads that read from SST
static std::atomic<uint64_t> g_readImmutableHits{0};         // reads that consulted immutable memtables
static std::atomic<uint64_t> g_flushSwapUsTotal{0};
static std::atomic<uint64_t> g_flushWriteUsTotal{0};
static std::atomic<uint64_t> g_flushSwapCount{0};
static std::atomic<uint64_t> g_flushWriteCount{0};
static std::atomic<uint64_t> g_bloomFilterHits{0};           // SSTs skipped by bloom filter
static std::atomic<uint64_t> g_bloomFilterMisses{0};         // SSTs that needed full scan (no bloom)
static std::atomic<uint64_t> g_bloomCacheHits{0};
static std::atomic<uint64_t> g_bloomCacheMisses{0};
static std::atomic<uint64_t> g_strongIdCacheShadowMatches{0};
static std::atomic<uint64_t> g_strongIdCacheShadowMismatches{0};
static std::atomic<uint64_t> g_strongIdCacheShadowMisses{0};

// Per-read SST scan histogram (ring buffer, 4096 samples) for P95/P99
static std::mutex g_sstScanHistMutex;
static std::vector<uint32_t> g_sstScanHistogram;  // SST count per read
static std::atomic<uint64_t> g_sstScanHistHead{0};
static constexpr size_t SST_HIST_CAP = 4096;

// Per-read execute-time histogram (ring buffer, 4096 samples) for P95/P99
static std::mutex g_readExecHistMutex;
static std::vector<uint32_t> g_readExecHistogramMs;  // ms per read
static std::atomic<uint64_t> g_readExecHistHead{0};
static std::mutex g_flushSwapHistMutex;
static std::vector<uint32_t> g_flushSwapHistogramUs;
static std::atomic<uint64_t> g_flushSwapHistHead{0};
static std::mutex g_flushWriteHistMutex;
static std::vector<uint32_t> g_flushWriteHistogramUs;
static std::atomic<uint64_t> g_flushWriteHistHead{0};

static void recordSstScanSample(size_t n) {
    std::lock_guard<std::mutex> lk(g_sstScanHistMutex);
    if (g_sstScanHistogram.size() < SST_HIST_CAP) g_sstScanHistogram.resize(SST_HIST_CAP, 0);
    size_t idx = g_sstScanHistHead.fetch_add(1, std::memory_order_relaxed) % SST_HIST_CAP;
    g_sstScanHistogram[idx] = (uint32_t)std::min<size_t>(n, 65535);
}
static void recordReadExecSample(uint64_t ms) {
    std::lock_guard<std::mutex> lk(g_readExecHistMutex);
    if (g_readExecHistogramMs.size() < SST_HIST_CAP) g_readExecHistogramMs.resize(SST_HIST_CAP, 0);
    size_t idx = g_readExecHistHead.fetch_add(1, std::memory_order_relaxed) % SST_HIST_CAP;
    g_readExecHistogramMs[idx] = (uint32_t)std::min<uint64_t>(ms, 65535);
}
static void recordFlushSwapSample(uint64_t us) {
    std::lock_guard<std::mutex> lk(g_flushSwapHistMutex);
    if (g_flushSwapHistogramUs.size() < SST_HIST_CAP) g_flushSwapHistogramUs.resize(SST_HIST_CAP, 0);
    size_t idx = g_flushSwapHistHead.fetch_add(1, std::memory_order_relaxed) % SST_HIST_CAP;
    g_flushSwapHistogramUs[idx] = (uint32_t)std::min<uint64_t>(us, 65535);
}
static void recordFlushWriteSample(uint64_t us) {
    std::lock_guard<std::mutex> lk(g_flushWriteHistMutex);
    if (g_flushWriteHistogramUs.size() < SST_HIST_CAP) g_flushWriteHistogramUs.resize(SST_HIST_CAP, 0);
    size_t idx = g_flushWriteHistHead.fetch_add(1, std::memory_order_relaxed) % SST_HIST_CAP;
    g_flushWriteHistogramUs[idx] = (uint32_t)std::min<uint64_t>(us, 65535);
}
static double histPercentile(const std::vector<uint32_t>& h, double pct) {
    if (h.empty()) return 0.0;
    std::vector<uint32_t> s(h);
    std::sort(s.begin(), s.end());
    size_t idx = std::min(s.size()-1, (size_t)std::ceil(s.size() * pct / 100.0) - 1);
    return (double)s[idx];
}

// Compaction throttle threshold (env: COMPACTION_READ_QUEUE_THRESHOLD, default 100)
static size_t compactionReadQueueThreshold() {
    static const size_t v = []() -> size_t {
        const char* e = std::getenv("COMPACTION_READ_QUEUE_THRESHOLD");
        if (e && *e) { try { return (size_t)std::stoul(e); } catch(...) {} }
        return (size_t)100;
    }();
    return v;
}

// Return current connection pool queue depth (0 if unavailable)
static inline size_t currentReadQueueDepth() {
    return (g_connectionPool) ? g_connectionPool->getQueuedTasks() : 0;
}

static size_t memtableHardLimit() {
    static const size_t hard = []() -> size_t {
        const char* v = std::getenv("LSM_MEMTABLE_HARD_LIMIT");
        if (v && *v) { try { return static_cast<size_t>(std::stoull(v)); } catch (...) {} }
        const char* m = std::getenv("LSM_MEMTABLE_STALL_MULT");
        size_t mult = 2;
        if (m && *m) { try { mult = std::max<size_t>(2, static_cast<size_t>(std::stoull(m))); } catch (...) {} }
        // Default: 2x the soft limit, but never let it exceed 50k docs of headroom
        size_t computed = MEMTABLE_LIMIT * mult;
        size_t capped = MEMTABLE_LIMIT + 50000;
        return std::min(computed, capped);
    }();
    return hard;
}

static bool maybeStallForFlush(const std::string& userId,
                               const std::string& dbName,
                               const std::string& collection,
                               const std::string& key) {
    const size_t hard = memtableHardLimit();
    size_t sz = 0;
    {
        auto lk = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(key));
        auto it = memtables.find(key);
        sz = (it != memtables.end()) ? it->second.size() : 0;
    }
    if (sz < hard) return false;

    // Deterministic recovery replay runs BEFORE LSM background maintenance starts, so the
    // background flush worker that this stall waits on does not exist yet and the memtable
    // can never drain. A captured backtrace showed recovery parked here in nanosleep:
    //   main -> executeRecovery -> phase3_ReplayWAL -> applyReplicatedEntry
    //        -> LSM::put -> maybeStallForFlush
    // With a 5s ceiling per call and every post-cap record hitting it, a large WAL turned
    // startup into hours and the node never bound its client port. Flush synchronously
    // instead: that bounds memtable growth exactly as the stall intended, and cannot
    // deadlock on a worker that has not been started.
    if (DatabaseEngine::recoveryReplayMode()) {
        LSM::forceFlush();
        return false;
    }

    // Over the hard cap: force a flush and pace the writer until it drains.
    enqueueBackgroundFlush(userId, dbName, collection);

    const auto stallStart = std::chrono::steady_clock::now();
    const int maxWaitMs = 5000;          // safety ceiling — never block a writer forever
    int waitedMs = 0;
    bool stalled = false;
    while (waitedMs < maxWaitMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        waitedMs += 2;
        stalled = true;
        size_t cur = 0;
        {
            auto lk = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(key));
            auto it = memtables.find(key);
            cur = (it != memtables.end()) ? it->second.size() : 0;
        }
        if (cur < MEMTABLE_LIMIT) break;
        // keep nudging the flush worker
        enqueueBackgroundFlush(userId, dbName, collection);
    }
    if (stalled) {
        g_writeStallsTotal.fetch_add(1, std::memory_order_relaxed);
        g_writeStallWaitUs.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - stallStart).count()),
            std::memory_order_relaxed);
    }
    return stalled;
}

static bool docVisibleByCommitIndex(const json& doc, uint64_t committedIndex);

static std::vector<std::shared_ptr<Memtable>> immutableSnapshotForKey(const std::string& key) {
    auto it = immutableMemtables.find(key);
    if (it == immutableMemtables.end()) return {};
    return std::vector<std::shared_ptr<Memtable>>(it->second.begin(), it->second.end());
}

static size_t immutableMemtableCount() {
    size_t n = 0;
    for (const auto& [key, tables] : immutableMemtables) {
        (void)key;
        n += tables.size();
    }
    return n;
}

static size_t immutableMemtableEntries() {
    size_t n = 0;
    for (const auto& [key, tables] : immutableMemtables) {
        (void)key;
        for (const auto& table : tables) {
            if (table) n += table->size();
        }
    }
    return n;
}

static size_t approximateMemtableBytes(const Memtable& table) {
    size_t bytes = 0;
    for (const auto& [id, doc] : table) {
        bytes += id.size();
        try { bytes += doc.dump().size(); } catch (...) {}
    }
    return bytes;
}

static size_t immutableMemtableBytes() {
    size_t bytes = 0;
    for (const auto& [key, tables] : immutableMemtables) {
        bytes += key.size();
        for (const auto& table : tables) {
            if (table) bytes += approximateMemtableBytes(*table);
        }
    }
    return bytes;
}

static void mergeVisibleMemtableInto(std::unordered_map<std::string, json>& latestById,
                                     std::vector<json>& noIdDocs,
                                     const Memtable& table,
                                     uint64_t committedIndex) {
    for (const auto& [id, doc] : table) {
        (void)id;
        if (!docVisibleByCommitIndex(doc, committedIndex)) continue;
        mergeLatestById(latestById, noIdDocs, doc);
    }
}

static std::string nextSstFileName() {
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t seq = sstFileSequence.fetch_add(1, std::memory_order_relaxed);
    return std::to_string(micros) + "_" + std::to_string(seq) + ".sst";
}

static size_t SPARSE_INDEX_STRIDE = []() {
    const char* v = std::getenv("LSM_SPARSE_INDEX_STRIDE");
    if (v) {
        try { return std::max<size_t>(8, static_cast<size_t>(std::stoul(v))); } catch (...) {}
    }
    return static_cast<size_t>(64);
}();

// simple bloom params
static const size_t BLOOM_SIZE = 1024; // bits

static std::shared_mutex g_bloomCacheMutex;
static std::unordered_map<std::string, std::shared_ptr<const std::vector<uint64_t>>> g_bloomCache;

static size_t bloomCacheMaxEntries() {
    static const size_t maxEntries = [] {
        const char* value = std::getenv("LSM_BLOOM_CACHE_MAX_ENTRIES");
        if (value) {
            try { return std::max<size_t>(1, std::stoull(value)); } catch (...) {}
        }
        return static_cast<size_t>(65536);
    }();
    return maxEntries;
}

static void cacheBloom(const std::string& path, std::vector<uint64_t> bits) {
    auto cached = std::make_shared<const std::vector<uint64_t>>(std::move(bits));
    std::unique_lock<std::shared_mutex> lock(g_bloomCacheMutex);
    // ponytail: whole-cache eviction is intentionally rare; add per-entry LRU only
    // if deployments exceed 65k live SSTs and cache cold-starts become measurable.
    if (g_bloomCache.size() >= bloomCacheMaxEntries() && !g_bloomCache.count(path)) {
        g_bloomCache.clear();
    }
    g_bloomCache[path] = std::move(cached);
}

static std::thread bgThread;

// forward declarations
static std::string colKey(const std::string& userId, const std::string& db, const std::string& coll);
static std::optional<std::tuple<std::string, std::string, std::string>>
replayCollectionWal(const fs::path& walPath);
namespace {
bool scheduleReplayedIndexRecovery(
    const std::set<std::tuple<std::string, std::string, std::string>>& collections);
}
static void buildSparseIndexForSST(const std::string& sstPath);
static std::optional<json> seekDocInSSTById(const std::string& sstPath, const std::string& id);

static size_t sstBlockBytes() {
    static const size_t bytes = std::clamp<size_t>(
        envSizeT("LSM_SST_BLOCK_BYTES", 8192), 4096, 16384);
    return bytes;
}

static bool scanSstRows(const fs::path& path,
                        const std::function<bool(json&&)>& visitor,
                        std::string* error = nullptr) {
    if (pacificdb::storage_v2::isBinarySst(path)) {
        return pacificdb::storage_v2::scanSst(path, visitor, error);
    }
    std::ifstream in(path);
    if (!in.is_open()) {
        if (error) *error = "cannot open legacy SST";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            if (!visitor(json::parse(line))) return true;
        } catch (const std::exception& e) {
            if (error) *error = std::string("corrupt legacy SST row: ") + e.what();
            return false;
        }
    }
    return true;
}

static std::vector<fs::path> publishedSstsForCollection(const fs::path& directory) {
    std::vector<fs::path> files;
    std::string error;
    if (!LsmManifestStore::publishedSsts(directory, files, &error)) {
        throw std::runtime_error("cannot load published SST set for " +
                                 directory.string() + ": " + error);
    }
    return files;
}

static bool sstHasPointIndex(const fs::path& path) {
    if (pacificdb::storage_v2::isBinarySst(path)) return true;
    std::error_code ec;
    const fs::path sidecar = path.string() + ".sidx";
    return fs::exists(sidecar, ec) && !ec && fs::file_size(sidecar, ec) > 0 && !ec;
}

// Only called before serving traffic or on an unpublished snapshot staging tree.
// A zero WAL boundary publishes file membership without reclaiming any WAL.
static void publishMissingSstManifest(const fs::path& directory) {
    const auto current = LsmManifestStore::load(directory);
    if (current.status == LsmManifestLoadStatus::ERROR) {
        throw std::runtime_error("cannot load SST manifest: " + current.error);
    }
    if (current.status == LsmManifestLoadStatus::OK) return;
    const auto files = publishedSstsForCollection(directory);
    if (files.empty()) return;
    LsmManifest manifest;
    manifest.generation = 1;
    for (const auto& file : files) manifest.sstFiles.push_back(file.filename().string());
    std::string error;
    if (!LsmManifestStore::publish(directory, manifest, files, &error)) {
        throw std::runtime_error("cannot publish initial SST manifest: " + error);
    }
}

// Raft snapshots intentionally ship authoritative SST contents, not derived
// bloom/sparse-index sidecars. Rebuild any missing sidecars before the node can
// serve traffic. Without this, a bounded secondary-index lookup performs one
// full SST scan per candidate ID (for example 40 scans of a 60 MiB SST).
static void repairMissingSstReadSidecars() {
    const fs::path root(LSM::rootOrThrow());
    std::error_code ec;
    for (auto& userDir : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!userDir.is_directory() || userDir.is_symlink()) continue;
        const std::string userName = userDir.path().filename().string();
        if (userName.empty() || userName.front() == '.' || userName == "raft" ||
            userName == "logs" || userName == "backups" || userName == "restores") continue;
        for (auto& dbDir : fs::directory_iterator(userDir.path(), ec)) {
            if (ec) break;
            if (!dbDir.is_directory() || dbDir.is_symlink()) continue;
            for (auto& collectionDir : fs::directory_iterator(dbDir.path(), ec)) {
                if (ec) break;
                if (!collectionDir.is_directory() || collectionDir.is_symlink()) continue;
                const std::string dirName = collectionDir.path().filename().string();
                if (dirName.size() <= 4 || dirName.substr(dirName.size() - 4) != ".lsm") continue;
                for (const auto& artifact : publishedSstsForCollection(collectionDir.path())) {
                    const std::string sst = artifact.string();
                    const fs::path bloom = sst + ".bloom";
                    std::error_code sidecarEc;
                    const bool bloomMissing = !fs::exists(bloom, sidecarEc) ||
                        (!sidecarEc && fs::file_size(bloom, sidecarEc) == 0);
                    sidecarEc.clear();
                    const bool sparseMissing = !sstHasPointIndex(artifact);
                    if (!bloomMissing && !sparseMissing) continue;
                    try {
                        if (bloomMissing) LSM::buildBloomForSST(sst);
                        if (sparseMissing) buildSparseIndexForSST(sst);
                        sidecarEc.clear();
                        const bool repaired = fs::exists(bloom, sidecarEc) && !sidecarEc &&
                            fs::file_size(bloom, sidecarEc) > 0;
                        sidecarEc.clear();
                        const bool sparseReady = sstHasPointIndex(artifact);
                        if (!repaired || !sparseReady) {
                            throw std::runtime_error("derived SST sidecar remained missing");
                        }
                        g_sstSidecarsRepaired.fetch_add(1, std::memory_order_relaxed);
                        std::cout << "[LSM][SIDECAR] repaired derived read sidecars for "
                                  << artifact.filename().string() << std::endl;
                    } catch (const std::exception& e) {
                        g_sstSidecarRepairFailures.fetch_add(1, std::memory_order_relaxed);
                        std::cerr << "[LSM][SIDECAR] repair failed for " << sst
                                  << ": " << e.what() << std::endl;
                    }
                }
                publishMissingSstManifest(collectionDir.path());
            }
        }
    }
}

static bool hasVisibleCommitState(const json& doc) {
    if (!doc.is_object()) return false;
    if (doc.contains("_visibility_state") && doc["_visibility_state"].is_string()) {
        const std::string state = doc["_visibility_state"].get<std::string>();
        return state == "COMMITTED_VISIBLE" || state == "TOMBSTONED";
    }
    if (doc.contains("visibility_state") && doc["visibility_state"].is_string()) {
        const std::string state = doc["visibility_state"].get<std::string>();
        return state == "COMMITTED_VISIBLE" || state == "TOMBSTONED";
    }
    if (doc.contains("committed") && doc["committed"].is_boolean()) {
        return doc["committed"].get<bool>();
    }
    return true;
}

static bool extractRowCommitIndex(const json& doc, uint64_t& rowCommit) {
    try {
        if (doc.contains("_raft_commit_index") && (doc["_raft_commit_index"].is_number_integer() || doc["_raft_commit_index"].is_number_unsigned())) {
            rowCommit = doc["_raft_commit_index"].get<uint64_t>();
            return true;
        }
        if (doc.contains("commit_index") && (doc["commit_index"].is_number_integer() || doc["commit_index"].is_number_unsigned())) {
            rowCommit = doc["commit_index"].get<uint64_t>();
            return true;
        }
    } catch (...) {}
    return false;
}

static bool standaloneRaftVisibilityBypass() {
    auto envFalse = [](const char* name) {
        const char* value = std::getenv(name);
        if (!value || !*value) return false;
        std::string normalized(value);
        std::transform(
            normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return normalized == "0" || normalized == "false" ||
               normalized == "no" || normalized == "off";
    };
    if (envFalse("RAFT_STANDALONE_BYPASS") ||
        envFalse("RAFT_BYPASS_SINGLE_NODE")) {
        return false;
    }
    try {
        return RaftCore::instance().isEnabled() &&
               RaftCore::instance().isLeader() &&
               RaftCore::instance().peerCount() == 0;
    } catch (...) {
        return false;
    }
}

static void stampVisibilityMetadata(json& doc, uint64_t committedIndex) {
    if (!doc.is_object()) return;
    if (!RaftCore::instance().isEnabled()) {
        if (!doc.contains("_visibility_state")) {
            doc["_visibility_state"] = isDeletedDoc(doc) ? "TOMBSTONED" : "COMMITTED_VISIBLE";
        }
        return;
    }
    if (standaloneRaftVisibilityBypass()) {
        if (!doc.contains("_visibility_state")) {
            doc["_visibility_state"] =
                isDeletedDoc(doc) ? "TOMBSTONED" : "COMMITTED_VISIBLE";
        }
        if (!doc.contains("committed")) {
            doc["committed"] =
                doc.value("_visibility_state", "") != "PENDING";
        }
        return;
    }

    uint64_t rowCommit = 0;
    if (!extractRowCommitIndex(doc, rowCommit)) {
        rowCommit = committedIndex;
        doc["_raft_commit_index"] = rowCommit;
    }
    if (!doc.contains("_visibility_floor")) {
        doc["_visibility_floor"] = rowCommit;
    }
    doc["committed"] = rowCommit <= committedIndex;
    doc["_visibility_state"] = doc["committed"].get<bool>()
        ? (isDeletedDoc(doc) ? "TOMBSTONED" : "COMMITTED_VISIBLE")
        : "PENDING";
}

static bool docVisibleByCommitIndex(const json& doc, uint64_t committedIndex) {
    if (!doc.is_object()) return false;

    if (!RaftCore::instance().isEnabled()) {
        if (doc.contains("_visibility_state") && doc["_visibility_state"].is_string()) {
            const std::string state = doc["_visibility_state"].get<std::string>();
            return state != "PENDING";
        }
        return true;
    }

    if (!hasVisibleCommitState(doc)) return false;
    if (standaloneRaftVisibilityBypass()) return true;

    try {
        uint64_t rowCommit = 0;
        if (!extractRowCommitIndex(doc, rowCommit)) return false;
        if (rowCommit > committedIndex) return false;

        uint64_t floor = rowCommit;
        if (doc.contains("_visibility_floor") && (doc["_visibility_floor"].is_number_integer() || doc["_visibility_floor"].is_number_unsigned())) {
            floor = doc["_visibility_floor"].get<uint64_t>();
        }
        if (floor > committedIndex) return false;
    } catch (...) {
        return false;
    }

    return true;
}

// V11.4-PATH-001. LSM_ROOT is a plain std::string that defaults to empty, and 44 of its 47
// uses built paths as fs::path(LSM::rootOrThrow()) / user / db / ... . With an empty root that yields
// "user/db/..." — a path relative to the process working directory. Two engines configured
// with different absolute DATA_ROOTs but the same cwd therefore shared storage, which
// silently destroyed test isolation and produced false reproductions.
//
// Root resolution now fails closed: the root must be non-empty and absolute, it is
// canonicalized once, and every path is derived from the canonical value.
void LSM::init(const std::string& rootPath) {
    if (rootPath.empty()) {
        throw std::runtime_error(
            "LSM::init requires a non-empty DATA_ROOT; an empty root resolves to paths "
            "relative to the process working directory (V11.4-PATH-001)");
    }
    fs::path candidate(rootPath);
    if (!candidate.is_absolute()) {
        throw std::runtime_error(
            "LSM::init requires an ABSOLUTE DATA_ROOT, refused relative path: " + rootPath
            + " (V11.4-PATH-001)");
    }
    std::error_code ec;
    fs::create_directories(candidate, ec);
    fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (ec || canonical.empty()) canonical = candidate.lexically_normal();
    LSM_ROOT = canonical.string();
    if (LSM_ROOT.back() != '/' && LSM_ROOT.back() != '\\') LSM_ROOT += "/";
    // A process crash can leave transient hard links behind. They are never a
    // recovery source; the durable .snapshot file is authoritative.
    fs::remove_all(fs::path(LSM_ROOT) / ".snapshot-pins", ec);
    std::cout << "[LSM] Initialized at: " << LSM_ROOT << std::endl;
    repairMissingSstReadSidecars();
}

// V11.4-PATH-002. Client-controlled identifiers reach the filesystem directly. A userId of
// "../escape" created a directory one level above the data root, and "../../escape2"
// created /tmp/escape2 containing x/wal/db.wal — an engine WAL written outside the root
// entirely. Every user-controlled path component must therefore be validated to be exactly
// one safe filesystem component before it is joined to any path.
//
// Unsafe values are REJECTED, never sanitized: silently rewriting "../x" to "x" would make
// two distinct client identifiers collide on one storage location.
// Verifies a constructed path really lies beneath the canonical root. Component-wise, not
// string-prefix: a string prefix test would accept "/data-evil" for root "/data". Symlinked
// components are refused because a link inside the root can still point outside it.
fs::path LSM::requireContained(const fs::path& candidate) {
    return validateContainedStoragePath(fs::path(rootOrThrow()), candidate);
}

fs::path LSM::databasePath(const std::string& userId,
                           const std::string& dbName) {
    validateStorageIdentifier(userId, "userId");
    validateStorageIdentifier(dbName, "databaseName");
    return requireContained(fs::path(rootOrThrow()) / userId / dbName);
}

// Builds a validated, contained path for a collection-scoped artifact. The
// client-controlled collection is validated before the engine-owned suffix is
// appended.
fs::path LSM::collectionArtifactPath(const std::string& userId,
                                     const std::string& dbName,
                                     const std::string& collection,
                                     const std::string& suffix) {
    validateStorageIdentifier(collection, "collectionName");
    validateStorageIdentifier(suffix, "artifactSuffix");
    return requireContained(databasePath(userId, dbName) / (collection + suffix));
}

// Single point of truth for LSM path construction. Refuses to hand out a root that would
// produce cwd-relative paths, so a missing or mis-ordered init cannot silently write
// storage into whatever directory the process happens to be running from.
const std::string& LSM::rootOrThrow() {
    if (LSM_ROOT.empty()) {
        throw std::runtime_error(
            "LSM root is not initialised; refusing to construct a working-directory "
            "relative storage path (V11.4-PATH-001)");
    }
    return LSM_ROOT;
}

// Replay all collection WALs under LSM_ROOT into bounded memtables.
void LSM::restoreFromWal() {
    if (LSM_ROOT.empty() || !fs::exists(LSM_ROOT)) {
        std::cout << "[LSM][RESTORE] LSM root missing, skip replay" << std::endl;
        return;
    }

    std::set<fs::path> logicalWals;
    for (auto& userDir : fs::directory_iterator(LSM_ROOT)) {
        if (!userDir.is_directory()) continue;
        for (auto& dbDir : fs::directory_iterator(userDir)) {
            if (!dbDir.is_directory()) continue;
            fs::path walDir = dbDir.path() / "wal";
            if (!fs::exists(walDir)) continue;
            for (auto& walFile : fs::directory_iterator(walDir)) {
                const auto name = walFile.path().filename().string();
                if (walFile.is_regular_file() && walFile.path().extension() == ".wal") {
                    logicalWals.insert(walFile.path());
                } else if (walFile.is_directory() && name.size() > 13 &&
                           name.compare(name.size() - 13, 13, ".wal.segments") == 0) {
                    logicalWals.insert(walDir / name.substr(0, name.size() - 9));
                }
            }
        }
    }
    std::set<std::tuple<std::string, std::string, std::string>> replayedCollections;
    for (const auto& wal : logicalWals) {
        if (auto replayed = replayCollectionWal(wal)) replayedCollections.insert(*replayed);
    }
    if (!scheduleReplayedIndexRecovery(replayedCollections)) {
        throw std::runtime_error("column-index recovery failed after WAL replay");
    }
    std::cout << "[LSM][RESTORE] Replayed " << logicalWals.size() << " WAL files" << std::endl;
}

// Stream one collection WAL. Flush uses the normal checkpoint path but defers
// segment reclamation until WAL::scan releases its per-log lock.
static std::optional<std::tuple<std::string, std::string, std::string>>
replayCollectionWal(const fs::path& walPath) {
    std::string expectedUser;
    std::string expectedDatabase;
    std::string expectedCollection;
    std::string expectedKey;
    uint64_t coveredLsn = 0;
    uint64_t applied = 0;
    std::string replayError;
    g_collectionWalReplay = true;
    struct ReplayGuard { ~ReplayGuard() { g_collectionWalReplay = false; } } replayGuard;

    const auto scan = WAL::scan(walPath.string(), [&](const WalReplayRecord& record) {
        try {
            const auto& entry = record.entry;
            if (!entry.is_object()) throw std::runtime_error("WAL entry is not an object");
            const std::string userId = entry.value("userId", std::string());
            const std::string database = entry.value("db", std::string());
            const std::string collection = entry.value("collection", std::string());
            if (userId.empty() || database.empty() || collection.empty()) {
                throw std::runtime_error("WAL entry has no namespace identity");
            }
            if (expectedKey.empty()) {
                expectedUser = userId;
                expectedDatabase = database;
                expectedCollection = collection;
                expectedKey = colKey(userId, database, collection);
                const auto manifest = LsmManifestStore::load(
                    LSM::collectionArtifactPath(userId, database, collection, ".lsm"));
                if (manifest.status == LsmManifestLoadStatus::ERROR) {
                    throw std::runtime_error("invalid recovery manifest: " + manifest.error);
                }
                if (manifest.status == LsmManifestLoadStatus::OK) {
                    coveredLsn = manifest.manifest.coveredWalLsn;
                }
                auto lock = pacificdb::timing::makeTimedUniqueLock(
                    getCollectionMutex(expectedKey));
                g_walApplyProgress[expectedKey].contiguous = coveredLsn;
                g_walApplyProgress[expectedKey].intervals.clear();
            } else if (userId != expectedUser || database != expectedDatabase ||
                       collection != expectedCollection) {
                throw std::runtime_error("collection WAL namespace identity changed");
            }
            if (record.lsn <= coveredLsn) return true;

            json document;
            const std::string operation = entry.value("op", std::string("PUT"));
            if (operation == "DELETE") {
                const std::string id = entry.value("id", std::string());
                if (id.empty()) throw std::runtime_error("DELETE WAL entry has no id");
                document = entry.contains("data") && entry["data"].is_object()
                    ? entry["data"] : json{{"id", id}, {"_deleted", true}};
            } else {
                if (!entry.contains("data") || !entry["data"].is_object()) {
                    throw std::runtime_error("mutation WAL entry has no object data");
                }
                document = entry["data"];
            }
            std::string id;
            if (document.contains("id")) {
                id = document["id"].is_string()
                    ? document["id"].get<std::string>() : document["id"].dump();
                document["id"] = id;
            } else {
                throw std::runtime_error("mutation WAL document has no id");
            }

            bool shouldFlush = false;
            {
                auto lock = pacificdb::timing::makeTimedUniqueLock(
                    getCollectionMutex(expectedKey));
                memtables[expectedKey][id] = document;
                // The ID index is a cache, not recovery state. Rebuilding it with full
                // documents makes startup memory proportional to total WAL history;
                // targeted ID reads already fall back to the manifest SSTs.
                markWalApplied(expectedKey, record.lsn, record.lsn);
                shouldFlush = memtables[expectedKey].size() >= MEMTABLE_LIMIT;
            }
            ++applied;
            if (shouldFlush) LSM::flush(userId, database, collection);
            return true;
        } catch (const std::exception& error) {
            replayError = error.what();
            return false;
        }
    });

    if (!replayError.empty()) {
        throw std::runtime_error("collection WAL replay failed: " + replayError);
    }
    if (scan.status == WalScanStatus::CORRUPT || scan.status == WalScanStatus::IO_ERROR) {
        throw std::runtime_error("collection WAL scan failed: " + scan.error);
    }
    if (!expectedKey.empty()) {
        bool hasTail = false;
        {
            auto lock = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(expectedKey));
            const auto found = memtables.find(expectedKey);
            hasTail = found != memtables.end() && !found->second.empty();
        }
        if (hasTail) LSM::flush(expectedUser, expectedDatabase, expectedCollection);
        if (scan.status == WalScanStatus::OK) {
            const auto manifest = LsmManifestStore::load(LSM::collectionArtifactPath(
                expectedUser, expectedDatabase, expectedCollection, ".lsm"));
            if (manifest.status == LsmManifestLoadStatus::OK) {
                std::string reclaimError;
                if (!WAL::reclaimThrough(walPath.string(), manifest.manifest.coveredWalLsn,
                                         &reclaimError)) {
                    std::cerr << "[LSM][RESTORE] WAL reclaim deferred: "
                              << reclaimError << std::endl;
                }
            }
        }
    }
    std::cout << "[LSM][RESTORE] Replayed " << applied << " records from "
              << walPath.filename().string() << std::endl;
    if (expectedKey.empty()) return std::nullopt;
    return std::make_tuple(expectedUser, expectedDatabase, expectedCollection);
}

static std::string colKey(const std::string& userId, const std::string& db, const std::string& coll) {
    // Length-prefix every namespace component. Delimiter concatenation can
    // alias distinct triples when a protocol client bypasses HTTP validation.
    return std::to_string(userId.size()) + ":" + userId
        + std::to_string(db.size()) + ":" + db
        + std::to_string(coll.size()) + ":" + coll;
}

static bool parseColKey(const std::string& key,
                        std::string& userId,
                        std::string& db,
                        std::string& coll) {
    size_t offset = 0;
    auto readComponent = [&](std::string& output) {
        const size_t colon = key.find(':', offset);
        if (colon == std::string::npos || colon == offset) return false;
        size_t length = 0;
        for (size_t i = offset; i < colon; ++i) {
            const unsigned char c = static_cast<unsigned char>(key[i]);
            if (c < '0' || c > '9') return false;
            const size_t digit = static_cast<size_t>(c - '0');
            if (length > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
            length = (length * 10) + digit;
        }
        offset = colon + 1;
        if (length > key.size() - offset) return false;
        output.assign(key, offset, length);
        offset += length;
        return true;
    };

    return readComponent(userId) && readComponent(db) && readComponent(coll) && offset == key.size();
}

void LSM::put(const std::string& userId,
              const std::string& dbName,
              const std::string& collection,
              const json& doc,
              bool requireIndexCompletion,
              std::uint64_t applyIndex,
              bool writeWal) {
    std::string key = colKey(userId, dbName, collection);
    json storedDoc = doc;
    uint64_t committedIndex = 0;
    try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) { committedIndex = 0; }
    stampVisibilityMetadata(storedDoc, committedIndex);

    // v3.0 perf: pre-compute id before entering the lock so we can pass storedDoc to
    // updateColumnIndexes OUTSIDE the exclusive lock. The column index update is pure
    // filesystem I/O (~8ms per write) and was the dominant lock hold-time driver under
    // concurrent load (250 writers × 8ms = 2s average queue, 18s P99 at 1000 concurrent).
    // The memtable + id index are the only structures that need the exclusive lock.
    // A document whose id is not a string (e.g. {"id": 42}) threw type_error.302 here.
    // LSM::put runs inside the Raft apply path, which never advances lastApplied past a
    // failed entry, so one such document stalled the apply loop permanently: reads kept
    // serving, while every later insert/update/delete died as write_not_committed.
    // Normalize the id to its string form and store it back, because every downstream
    // consumer (bloom filter, sparse index, SST id-seek, findByField) requires a string
    // id and silently skips documents that carry anything else.
    std::string id;
    if (storedDoc.contains("id")) {
        id = storedDoc["id"].is_string() ? storedDoc["id"].get<std::string>() : storedDoc["id"].dump();
        storedDoc["id"] = id;
    } else {
        id = std::to_string(std::time(nullptr));
    }
    const uint64_t logicalBytes = json::to_msgpack(storedDoc).size();
    bool shouldFlush = false;
    WalAppendResult walAppend;
    {
        // Production: per-collection lock for parallel writes to different collections.
        // Never call flush while holding this lock; flush may need the same lock.
        auto lk = pacificdb::timing::makeTimedUniqueLock(getCollectionMutex(key));

        // ensure directory
        fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
        createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), dir);
        fs::path walDir = LSM::requireContained(LSM::databasePath(userId, dbName) / "wal");
        createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), walDir);

        // WAL entry
        json walEntry = {
            {"op","PUT"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"data", storedDoc}
        };
        std::string walFile = LSM::requireContained(
            walDir / (validateStorageIdentifier(collection, "collectionName") + ".wal")).string();
        if (writeWal) {
            auto walStart = std::chrono::steady_clock::now();
            walAppend = WAL::log(walFile, walEntry);
            auto walEnd = std::chrono::steady_clock::now();
            pacificdb::timing::recordStage(pacificdb::timing::Stage::WalAppend,
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(walEnd - walStart).count()));
        }

        // memtable insert
        auto memtableStart = std::chrono::steady_clock::now();
        memtables[key][id] = storedDoc;
        upsertIdIndex(key, id, storedDoc);
        markWalApplied(key, walAppend.firstLsn, walAppend.lastLsn);
        auto memtableEnd = std::chrono::steady_clock::now();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::MemtableInsert,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(memtableEnd - memtableStart).count()));

        LSM_LOG("[LSM][PUT] " << key << " / id=" << id);
        shouldFlush = memtables[key].size() >= MEMTABLE_LIMIT;
    }
    g_bytesIngested.fetch_add(logicalBytes, std::memory_order_relaxed);
    if (requireIndexCompletion) {
        pacificdb::test::hitFailpoint(
            "FP_APPLY_AFTER_BASE_BEFORE_INDEX", applyIndex);
    }
    const bool hasColumnIndexes = fs::exists(
        LSM::collectionArtifactPath(userId, dbName, collection, ".idx") / "_catalog.json");
    // v3.0 perf: enqueue to centralized batcher instead of spawning a detached thread.
    // The batcher groups writes by collection and applies them in a single exclusive-lock
    // acquisition per collection every 5ms, eliminating the thundering-herd of concurrent
    // exclusive locks that blocked findByField shared-lock readers.
    if (hasColumnIndexes && requireIndexCompletion
        && !g_idxBatcherRunning.load(std::memory_order_acquire)) {
        // Recovery replays committed Raft entries before background maintenance
        // starts. Apply the index update inline so replay cannot wait for a
        // batcher thread that does not exist yet.
        updateColumnIndexesBatch(userId, dbName, collection, {storedDoc});
    } else if (hasColumnIndexes) {
        enqueueIndexUpdate(userId, dbName, collection, {storedDoc}, requireIndexCompletion);
        if (requireIndexCompletion
            && !waitForColumnIndexUpdates(userId, dbName, collection, 30000)) {
            throw std::runtime_error("replicated_apply_index_completion_timeout");
        }
    }

    if (shouldFlush) {
        LSM_LOG("[LSM] memtable threshold reached, queueing background flush...");
        enqueueBackgroundFlush(userId, dbName, collection);
    }
    // v2.9R: bound per-collection memtable memory by stalling the writer if the
    // active memtable has grown past the hard cap (flush can't keep up).
    maybeStallForFlush(userId, dbName, collection, key);
}

nlohmann::json LSM::putMany(const std::string& userId,
                            const std::string& dbName,
                            const std::string& collection,
                            const std::vector<json>& docs,
                            bool requireIndexCompletion,
                            std::uint64_t applyIndex,
                            bool writeWal) {
    auto totalStart = std::chrono::steady_clock::now();
    // V11.4-DIV-001 progress guard. Fails the REAL storage mutation for one exact Raft
    // index so a test can prove a genuine storage failure propagates back through
    // DatabaseEngine::applyReplicatedEntry() as false, rather than mocking that return
    // value. Compiled out of production builds; inert without explicit test env.
    if (pacificdb::test::injectApplyFailure("FP_APPLY_STORAGE_FAILURE", applyIndex)) {
        return nlohmann::json{
            {"status", "error"},
            {"error", "div001_injected_storage_failure"},
            {"inserted", 0},
            {"applyIndex", applyIndex}
        };
    }
    nlohmann::json result = {
        {"status", "ok"},
        {"inserted", docs.size()},
        {"flush_triggered", false},
        {"memtable_size", 0},
        {"timings_us", {
            {"wal_append", 0},
            {"memtable_write", 0},
            {"index_update", 0},
            {"flush_wait", 0},
            {"total", 0}
        }}
    };
    if (docs.empty()) return result;

    std::string key = colKey(userId, dbName, collection);
    uint64_t committedIndex = 0;
    try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) { committedIndex = 0; }
    std::vector<json> storedDocs;
    storedDocs.reserve(docs.size());
    uint64_t logicalBytes = 0;
    bool allStoredDocsApplied = true;
    for (auto storedDoc : docs) {
        if (storedDoc.is_object()) {
            stampVisibilityMetadata(storedDoc, committedIndex);
            logicalBytes += json::to_msgpack(storedDoc).size();
        } else {
            allStoredDocsApplied = false;
        }
        storedDocs.push_back(std::move(storedDoc));
    }

    fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
    fs::path walDir = LSM::requireContained(LSM::databasePath(userId, dbName) / "wal");
    createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), dir);
    createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), walDir);

    std::vector<json> walEntries;
    walEntries.reserve(storedDocs.size());
    for (const auto& storedDoc : storedDocs) {
        walEntries.push_back({
            {"op", "PUT"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"data", storedDoc}
        });
    }

    std::string walFile = (walDir / (collection + ".wal")).string();
    WalAppendResult walAppend;
    if (writeWal) {
        auto walStart = std::chrono::steady_clock::now();
        walAppend = WAL::logBatch(walFile, walEntries);
        auto walEnd = std::chrono::steady_clock::now();
        const uint64_t walUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(walEnd - walStart).count());
        result["timings_us"]["wal_append"] = walUs;
    }

    bool shouldFlush = false;
    {
        // ── Critical section: hold lock only for memtable + id-index update ──
        // Column index file I/O is done OUTSIDE the lock to avoid serializing
        // concurrent bulk insertMany calls on the same collection.
        auto lk = pacificdb::timing::makeTimedUniqueLock(getCollectionMutex(key));
        auto memtableStart = std::chrono::steady_clock::now();
        for (const auto& storedDoc : storedDocs) {
            if (!storedDoc.is_object()) continue;
            std::string id = storedDoc.contains("id")
                ? (storedDoc["id"].is_string() ? storedDoc["id"].get<std::string>() : storedDoc["id"].dump())
                : std::to_string(std::time(nullptr));
            memtables[key][id] = storedDoc;
            upsertIdIndex(key, id, storedDoc);
        }
        if (allStoredDocsApplied) {
            markWalApplied(key, walAppend.firstLsn, walAppend.lastLsn);
        }
        auto memtableEnd = std::chrono::steady_clock::now();
        const uint64_t memtableUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(memtableEnd - memtableStart).count());
        pacificdb::timing::recordStage(pacificdb::timing::Stage::MemtableInsert, memtableUs);
        result["timings_us"]["memtable_write"] = memtableUs;
        result["memtable_size"] = memtables[key].size();
        shouldFlush = memtables[key].size() >= MEMTABLE_LIMIT;
    }
    g_bytesIngested.fetch_add(logicalBytes, std::memory_order_relaxed);
    if (requireIndexCompletion) {
        pacificdb::test::hitFailpoint(
            "FP_APPLY_AFTER_BASE_BEFORE_INDEX", applyIndex);
    }
    const bool hasColumnIndexes = fs::exists(
        LSM::collectionArtifactPath(userId, dbName, collection, ".idx") / "_catalog.json");
    // v3.0 perf: enqueue to centralized batcher (same as put()).
    auto indexStart = std::chrono::steady_clock::now();
    if (hasColumnIndexes && requireIndexCompletion
        && !g_idxBatcherRunning.load(std::memory_order_acquire)) {
        updateColumnIndexesBatch(userId, dbName, collection, storedDocs);
    } else if (hasColumnIndexes) {
        enqueueIndexUpdate(userId, dbName, collection, storedDocs, requireIndexCompletion);
        if (requireIndexCompletion
            && !waitForColumnIndexUpdates(userId, dbName, collection, 30000)) {
            result["status"] = "error";
            result["error"] = "replicated_apply_index_completion_timeout";
            return result;
        }
    }
    auto indexEnd = std::chrono::steady_clock::now();
    const uint64_t indexUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(indexEnd - indexStart).count());
    result["timings_us"]["index_update"] = indexUs;

    if (shouldFlush) {
        result["flush_triggered"] = true;
        auto flushStart = std::chrono::steady_clock::now();
        enqueueBackgroundFlush(userId, dbName, collection);
        auto flushEnd = std::chrono::steady_clock::now();
        result["timings_us"]["flush_wait"] =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(flushEnd - flushStart).count());
    }

    // v2.9R: bound per-collection memtable memory under sustained bulk writes.
    bool stalled = maybeStallForFlush(userId, dbName, collection, key);
    result["write_stalled"] = stalled;

    auto totalEnd = std::chrono::steady_clock::now();
    result["timings_us"]["total"] =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(totalEnd - totalStart).count());
    return result;
}

void LSM::flush(const std::string& userId, const std::string& dbName, const std::string& collection) {
    std::string key = colKey(userId, dbName, collection);
    CollectionFlushGuard flushGuard(key);
    if (!flushGuard.active) {
        LSM_LOG("[LSM][FLUSH] flush already running for " << key);
        return;
    }

    std::shared_ptr<Memtable> flushSnapshot;
    uint64_t flushCoveredWalLsn = 0;
    {
        auto swapStart = std::chrono::steady_clock::now();
        auto lk = pacificdb::timing::makeTimedUniqueLock(getCollectionMutex(key));
        auto it = memtables.find(key);
        if (it == memtables.end() || it->second.empty()) {
            LSM_LOG("[LSM][FLUSH] memtable empty for " << key);
            return;
        }
        flushSnapshot = std::make_shared<Memtable>();
        flushSnapshot->swap(it->second);
        flushCoveredWalLsn = g_walApplyProgress[key].contiguous;
        immutableMemtables[key].push_front(flushSnapshot);
        const uint64_t swapUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - swapStart).count());
        g_flushSwapUsTotal.fetch_add(swapUs, std::memory_order_relaxed);
        g_flushSwapCount.fetch_add(1, std::memory_order_relaxed);
        recordFlushSwapSample(swapUs);
    }

    fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
    createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), dir);

    // create SST file
    std::string sstName = nextSstFileName();
    fs::path sstPath = dir / sstName;
    fs::path writingPath = dir / (sstName + ".writing");

    // Keep SST sorted by id to support sparse-index assisted seeks.
    std::vector<std::string> ids;
    auto flushWriteStart = std::chrono::steady_clock::now();
    ids.reserve(flushSnapshot->size());
    // V11.4-DIV-001 P0.6: every document in the swapped snapshot is persisted.
    // Filtering the SST write by commit-index visibility destroyed applied but
    // not-yet-visible documents: they were written to no SST and dropped from
    // memory. Persisting them is visibility-safe because EVERY read path already
    // applies docVisibleByCommitIndex (memtable, SST and snapshot reads alike), so
    // a not-yet-visible document stays invisible until its commit index is covered
    // and simply becomes visible later instead of being lost.
    //
    // Returning such documents to the active memtable instead is NOT viable: it
    // breaks the flush-progress contract that LSM::forceFlush() depends on (it
    // re-invokes flush every 2ms until the memtable drains), which livelocks the
    // drain and emits an SST per iteration.
    for (const auto& [id, doc] : *flushSnapshot) {
        (void)doc;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    std::vector<std::reference_wrapper<const json>> rows;
    rows.reserve(ids.size());
    for (const auto& id : ids) {
        rows.emplace_back((*flushSnapshot)[id]);
    }
    pacificdb::storage_v2::SstWriteStats sstStats;
    std::string sstError;
    if (!pacificdb::storage_v2::writeSst(
            writingPath, rows, sstBlockBytes(), &sstStats, &sstError)) {
        std::cerr << "[LSM][FLUSH] cannot write sst file " << writingPath
                  << ": " << sstError << std::endl;
        return;
    }
    const uint64_t flushWriteBytes = sstStats.storedBytes;
    const uint64_t flushWriteUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - flushWriteStart).count());
    g_flushWriteUsTotal.fetch_add(flushWriteUs, std::memory_order_relaxed);
    g_flushWriteCount.fetch_add(1, std::memory_order_relaxed);
    recordFlushWriteSample(flushWriteUs);
    g_flushBytesWritten.fetch_add(flushWriteBytes, std::memory_order_relaxed);

    LSM_LOG("[LSM][FLUSH] Wrote " << flushSnapshot->size() << " entries to " << sstPath.string());

    // Build bloom filter for newly flushed SST to speed up negative lookups
    LSM::buildBloomForSST(writingPath.string());
    buildSparseIndexForSST(writingPath.string());
    pacificdb::test::hitFailpoint(
        "FP_LSM_CHECKPOINT_AFTER_SST_SYNC", flushCoveredWalLsn);

    uint64_t publishedWalLsn = 0;
    {
        auto lk = pacificdb::timing::makeTimedUniqueLock(getCollectionMutex(key));
        const auto current = LsmManifestStore::load(dir);
        if (current.status == LsmManifestLoadStatus::ERROR) {
            throw std::runtime_error("cannot load LSM manifest before flush publish: " +
                                     current.error);
        }
        auto published = publishedSstsForCollection(dir);

        std::error_code renameError;
        const fs::path writingBloom = writingPath.string() + ".bloom";
        const fs::path finalBloom = sstPath.string() + ".bloom";
        const fs::path writingSparse = writingPath.string() + ".sidx";
        const fs::path finalSparse = sstPath.string() + ".sidx";
        if (fs::exists(writingBloom)) fs::rename(writingBloom, finalBloom, renameError);
        if (!renameError && fs::exists(writingSparse)) {
            fs::rename(writingSparse, finalSparse, renameError);
        }
        if (!renameError) fs::rename(writingPath, sstPath, renameError);
        if (renameError) {
            fs::remove(writingPath);
            fs::remove(writingBloom);
            fs::remove(writingSparse);
            fs::remove(sstPath);
            fs::remove(finalBloom);
            fs::remove(finalSparse);
            throw std::runtime_error("cannot finalize LSM flush artifacts: " +
                                     renameError.message());
        }
        pacificdb::test::hitFailpoint(
            "FP_LSM_CHECKPOINT_AFTER_ARTIFACT_RENAME", flushCoveredWalLsn);

        published.push_back(sstPath);
        std::sort(published.begin(), published.end());
        published.erase(std::unique(published.begin(), published.end()), published.end());

        LsmManifest next;
        next.generation = current.status == LsmManifestLoadStatus::OK
            ? current.manifest.generation + 1 : 1;
        next.coveredWalLsn = std::max(
            flushCoveredWalLsn,
            current.status == LsmManifestLoadStatus::OK
                ? current.manifest.coveredWalLsn : uint64_t{0});
        for (const auto& path : published) next.sstFiles.push_back(path.filename().string());
        std::string publishError;
        if (!LsmManifestStore::publish(dir, next, {sstPath}, &publishError)) {
            fs::remove(sstPath);
            fs::remove(finalBloom);
            fs::remove(finalSparse);
            throw std::runtime_error("cannot publish LSM flush checkpoint: " + publishError);
        }
        publishedWalLsn = next.coveredWalLsn;

        auto immIt = immutableMemtables.find(key);
        if (immIt != immutableMemtables.end()) {
            auto& dq = immIt->second;
            dq.erase(std::remove(dq.begin(), dq.end(), flushSnapshot), dq.end());
            if (dq.empty()) immutableMemtables.erase(immIt);
        }
        compactionPublishGeneration(key).fetch_add(1, std::memory_order_release);
    }

    if (publishedWalLsn > 0 && !g_collectionWalReplay) {
        const fs::path wal = LSM::requireContained(
            LSM::databasePath(userId, dbName) / "wal" /
            (validateStorageIdentifier(collection, "collectionName") + ".wal"));
        std::string reclaimError;
        pacificdb::test::hitFailpoint(
            "FP_LSM_CHECKPOINT_BEFORE_WAL_RECLAIM", publishedWalLsn);
        if (!WAL::reclaimThrough(wal.string(), publishedWalLsn, &reclaimError)) {
            std::cerr << "[LSM][FLUSH] checkpoint published but WAL reclaim failed: "
                      << reclaimError << std::endl;
        }
        pacificdb::test::hitFailpoint(
            "FP_LSM_CHECKPOINT_AFTER_WAL_RECLAIM", publishedWalLsn);
    }

    // Fix 2: Per-Key Floor Invalidation on Flush
    // Atomically invalidate per-key session floor BEFORE SST is visible to reads
    // This forces recomputation of floor on next strong read
    invalidateSessionFloor(key);
    LSM_LOG("[LSM][FLUSH][FLOOR_INVALIDATION] key=" << key << " INVALIDATED_FLOOR_REGISTRY");

    // v2.9R: DO NOT rebuild id/column indexes here. Both are maintained
    // incrementally on the write path (upsertIdIndex / updateColumnIndexes), so
    // the old per-flush rebuild was redundant — and it called getAll(), loading
    // the ENTIRE growing collection into RAM on every flush. Under sustained
    // writes that O(collection) allocation was the dominant OOM driver. Index
    // hygiene (cleaning stale entries) now happens during compaction instead.

    bool stillAboveLimit = false;
    {
        auto lk = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(key));
        auto it = memtables.find(key);
        stillAboveLimit = (it != memtables.end() && it->second.size() >= MEMTABLE_LIMIT);
    }
    if (stillAboveLimit) {
        enqueueBackgroundFlush(userId, dbName, collection);
    }
}

// ---------------- COMPACTION ----------------
void LSM::compact(const std::string& userId, const std::string& dbName, const std::string& collection) {
    // Visibility filtering depends on Raft's restored commit index. Startup
    // maintenance can race replay; compacting with the default watermark would
    // classify committed snapshot rows as invisible and delete them.
    try {
        if (!RaftCore::instance().isRecoveryComplete()) return;
    } catch (...) {
        return;
    }
    std::string key = colKey(userId, dbName, collection);
    fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
    if (!fs::exists(dir)) return;
    pacificdb::test::hitFailpoint("FP_SHUTDOWN_DURING_COMPACTION", 1);

    // --- Phase 1: Snapshot SST file list under collection read lock ---
    // We only need the lock to prevent concurrent flush from adding new SSTs while we're
    // collecting the input set. We release it before any I/O to avoid blocking reads.
    std::vector<fs::path> ssts;
    uint64_t totalSstBytes = 0;
    {
        auto lk = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(key));
        std::vector<std::pair<fs::path, uintmax_t>> allSsts;
        for (const auto& sst : publishedSstsForCollection(dir)) {
            std::error_code ec;
            uintmax_t sz = fs::file_size(sst, ec);
            if (ec) sz = 0;
            totalSstBytes += sz;
            allSsts.emplace_back(sst, sz);
        }
        std::sort(allSsts.begin(), allSsts.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; }); // oldest first

        // v2.9R-hotfix: size-tiered selection. Phase 2 builds an in-memory `merged` map of
        // EVERY doc in the selected SSTs. Merging the whole collection every pass is an
        // O(collection) allocation that spiked RSS past the memory budget during growth
        // (peak 150% of limit even after removing the redundant rebuild). Only merge SSTs
        // below a "sealed" size cap, and at most N per pass, so the working set is bounded
        // no matter how large the collection grows. Large already-merged SSTs stay sealed —
        // reads still reconcile newest-version-wins across all SSTs, so this is correct;
        // it only trades some space (tombstones in sealed SSTs aren't GC'd) for a hard
        // memory bound. Tunable via LSM_COMPACT_MAX_SST_BYTES / LSM_COMPACT_MAX_SSTS_PER_PASS.
        static const uintmax_t MAX_SST_BYTES = []() -> uintmax_t {
            const char* v = std::getenv("LSM_COMPACT_MAX_SST_BYTES");
            if (v && *v) { try { return static_cast<uintmax_t>(std::stoull(v)); } catch (...) {} }
            return 16ULL * 1024 * 1024; // 16MB sealed threshold
        }();
        static const size_t MAX_SSTS_PER_PASS = []() -> size_t {
            const char* v = std::getenv("LSM_COMPACT_MAX_SSTS_PER_PASS");
            if (v && *v) { try { return std::max<size_t>(2, std::stoull(v)); } catch (...) {} }
            return 32;
        }();
        const size_t fanout = std::min(compactionFanout(), MAX_SSTS_PER_PASS);
        for (size_t start = 0; start < allSsts.size() && ssts.empty(); ++start) {
            const uintmax_t seedBytes = allSsts[start].second;
            if (seedBytes == 0 || seedBytes > MAX_SST_BYTES) continue;
            const uintmax_t minimumBytes = (seedBytes + 1) / 2;
            const uintmax_t maximumBytes = seedBytes * 2;
            std::vector<fs::path> run;
            for (size_t i = start; i < allSsts.size() && run.size() < fanout; ++i) {
                const uintmax_t bytes = allSsts[i].second;
                if (bytes < minimumBytes || bytes > maximumBytes || bytes > MAX_SST_BYTES) break;
                run.push_back(allSsts[i].first);
            }
            if (run.size() == fanout) ssts = std::move(run);
        }
    }

    if (ssts.empty()) return;
    const bool fullCoverageCompaction = totalSstBytes > 0 &&
        publishedSstsForCollection(dir).size() == ssts.size();

    // --- Phase 2: Merge SSTs without holding any lock (pure I/O) ---
    // Reads/writes to the collection can proceed concurrently while we merge.
    // v4.4H: Track compaction as active and throttle if read queue is deep.
    g_compactionActiveCount.fetch_add(1, std::memory_order_relaxed);
    struct CompactionGuard {
        ~CompactionGuard() { g_compactionActiveCount.fetch_sub(1, std::memory_order_relaxed); }
    } cg;

    const size_t ioThreshold = compactionReadQueueThreshold();
    std::unordered_map<std::string, json> merged;
    uint64_t committedIndex = 0;
    try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) { committedIndex = 0; }
    uint64_t compactionInputBytes = 0;
    for (const auto& path : ssts) {
        std::error_code sizeError;
        compactionInputBytes += fs::file_size(path, sizeError);
    }
    g_compactionBytesRead.fetch_add(compactionInputBytes, std::memory_order_relaxed);
    for (auto& p : ssts) {
        // Yield to reads: if the connection pool has pending requests, pause briefly
        // before each SST read so those requests can be dequeued and processed.
        if (ioThreshold > 0) {
            size_t qd = currentReadQueueDepth();
            if (qd >= ioThreshold) {
                auto yieldStart = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                uint64_t yieldMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - yieldStart).count();
                g_compactionThrottledTotal.fetch_add(1, std::memory_order_relaxed);
                g_compactionYieldMsTotal.fetch_add(yieldMs, std::memory_order_relaxed);
            }
        }
        std::string readError;
        if (!scanSstRows(p, [&](json&& j) {
                if (!docVisibleByCommitIndex(j, committedIndex)) return true;
                std::string id = j.contains("id")
                    ? (j["id"].is_string() ? j["id"].get<std::string>() : j["id"].dump())
                    : std::to_string(std::time(nullptr));
                auto it = merged.find(id);
                if (it == merged.end() || isCandidateNewer(j, it->second)) {
                    merged[id] = std::move(j);
                }
                return true;
            }, &readError)) {
            throw std::runtime_error("cannot compact SST " + p.string() + ": " + readError);
        }
    }
    g_compactionRunsTotal.fetch_add(1, std::memory_order_relaxed);

    // Write merged SST to a temporary path first (atomic rename pattern)
    std::string outName = nextSstFileName();
    fs::path outPath = dir / outName;
    fs::path tmpPath = dir / (outName + ".tmp");
    {
        std::vector<std::string> mergedIds;
        mergedIds.reserve(merged.size());
        for (const auto& [id, _] : merged) mergedIds.push_back(id);
        std::sort(mergedIds.begin(), mergedIds.end());
        std::vector<std::reference_wrapper<const json>> rows;
        rows.reserve(mergedIds.size());
        uint64_t tombstonesObserved = 0;
        uint64_t tombstonesReclaimed = 0;
        uint64_t reclaimedBytes = 0;
        for (const auto& id : mergedIds) {
            if (isDeletedDoc(merged[id])) {
                tombstonesObserved++;
                if (fullCoverageCompaction) {
                    reclaimedBytes += merged[id].dump().size() + 1;
                    tombstonesReclaimed++;
                    continue;
                }
            }
            rows.emplace_back(merged[id]);
        }
        pacificdb::storage_v2::SstWriteStats sstStats;
        std::string writeError;
        if (!pacificdb::storage_v2::writeSst(
                tmpPath, rows, sstBlockBytes(), &sstStats, &writeError)) {
            throw std::runtime_error("cannot write compacted SST: " + writeError);
        }
        g_compactionBytesWritten.fetch_add(sstStats.storedBytes, std::memory_order_relaxed);
        g_compactionTombstonesObserved.fetch_add(tombstonesObserved, std::memory_order_relaxed);
        g_compactionTombstonesReclaimed.fetch_add(tombstonesReclaimed, std::memory_order_relaxed);
        g_compactionReclaimedBytes.fetch_add(reclaimedBytes, std::memory_order_relaxed);
    }

    // Build bloom/sparse index for the new merged SST (no lock needed)
    LSM::buildBloomForSST(tmpPath.string());
    buildSparseIndexForSST(tmpPath.string());

    // --- Phase 3: Atomic swap — acquire write lock only for the rename+delete step ---
    // This keeps the critical section minimal: rename, manifest publish, then cleanup.
    {
        auto lk = pacificdb::timing::makeTimedUniqueLock(getCollectionMutex(key));

        std::error_code ec;
        const fs::path tmpBloom = tmpPath.string() + ".bloom";
        const fs::path outBloom = outPath.string() + ".bloom";
        if (fs::exists(tmpBloom)) {
            fs::rename(tmpBloom, outBloom, ec);
            if (ec) {
                std::cerr << "[LSM][COMPACT] Failed to finalize bloom sidecar: "
                          << ec.message() << std::endl;
                return;
            }
        }
        const fs::path tmpSparse = tmpPath.string() + ".sidx";
        const fs::path outSparse = outPath.string() + ".sidx";
        if (fs::exists(tmpSparse)) {
            fs::rename(tmpSparse, outSparse, ec);
            if (ec) {
                fs::remove(outBloom, ec);
                std::cerr << "[LSM][COMPACT] Failed to finalize sparse sidecar: "
                          << ec.message() << std::endl;
                return;
            }
        }

        ec.clear();
        fs::rename(tmpPath, outPath, ec);
        if (ec) {
            fs::remove(outBloom, ec);
            fs::remove(outSparse, ec);
            std::cerr << "[LSM][COMPACT] Failed to finalize merged SST: " << outPath << std::endl;
            return;
        }

        try {
            const auto current = LsmManifestStore::load(dir);
            if (current.status == LsmManifestLoadStatus::ERROR) {
                throw std::runtime_error(current.error);
            }
            auto published = publishedSstsForCollection(dir);
            for (const auto& input : ssts) {
                if (std::find(published.begin(), published.end(), input) == published.end()) {
                    throw std::runtime_error("compaction input is no longer manifest-authoritative");
                }
            }
            published.erase(std::remove_if(published.begin(), published.end(),
                [&](const fs::path& candidate) {
                    return std::find(ssts.begin(), ssts.end(), candidate) != ssts.end();
                }), published.end());
            published.push_back(outPath);
            std::sort(published.begin(), published.end());
            published.erase(std::unique(published.begin(), published.end()), published.end());

            LsmManifest next;
            next.generation = current.status == LsmManifestLoadStatus::OK
                ? current.manifest.generation + 1 : 1;
            next.coveredWalLsn = current.status == LsmManifestLoadStatus::OK
                ? current.manifest.coveredWalLsn : 0;
            for (const auto& path : published) {
                next.sstFiles.push_back(path.filename().string());
            }
            std::string publishError;
            if (!LsmManifestStore::publish(dir, next, {outPath}, &publishError)) {
                throw std::runtime_error(publishError);
            }
        } catch (const std::exception& error) {
            fs::remove(outPath, ec);
            fs::remove(outBloom, ec);
            fs::remove(outSparse, ec);
            std::cerr << "[LSM][COMPACT] Manifest publication failed: "
                      << error.what() << std::endl;
            return;
        }

        // Inputs remain readable until the replacement manifest is durable.
        for (auto& p : ssts) {
            fs::remove(p, ec);
            // Remove associated bloom/sidx artifacts
            std::string ps = p.string();
            fs::remove(ps + ".bloom", ec);
            fs::remove(ps + ".sidx", ec);
        }
        compactionPublishGeneration(key).fetch_add(1, std::memory_order_release);
    }

    // Column indexes are maintained incrementally and compacted by the maintenance
    // loop. Rebuilding them from a separate base-data snapshot here can overwrite
    // newer index batches and also adds an O(collection) allocation to compaction.
    invalidateSessionFloor(key);

    LSM_LOG("[LSM][COMPACT] Merged " << ssts.size() << " SSTs into " << outPath.string());
}

// ---------------- BLOOM FILTER (VERY SIMPLE) ----------------
void LSM::buildBloomForSST(const std::string& sstPath) {
    try {
        if (pacificdb::storage_v2::isBinarySst(sstPath)) {
            std::vector<std::string> keys;
            std::string error;
            if (!scanSstRows(sstPath, [&](json&& row) {
                    if (row.contains("id")) {
                        keys.push_back(row["id"].is_string()
                            ? row["id"].get<std::string>() : row["id"].dump());
                    }
                    return true;
                }, &error) ||
                !pacificdb::storage_v2::buildBloom(sstPath, keys, &error)) {
                throw std::runtime_error("cannot build binary SST bloom: " + error);
            }
            LSM_LOG("[LSM][BLOOM] Built sized binary bloom for " << sstPath);
            return;
        }
        std::ifstream in(sstPath);
        if (!in.is_open()) return;
        std::vector<uint64_t> bits(BLOOM_SIZE/64);
        std::string line;
        while (std::getline(in, line)) {
            try {
                auto j = json::parse(line);
                std::string id = j.contains("id")
                    ? (j["id"].is_string() ? j["id"].get<std::string>() : j["id"].dump())
                    : line;
                uint64_t h = std::hash<std::string>{}(id);
                size_t idx = h % BLOOM_SIZE;
                bits[idx/64] |= (1ULL << (idx%64));
            } catch (...) {}
        }

        // write bloom as json array
        std::string bloomPath = sstPath + std::string(".bloom");
        std::ofstream out(bloomPath, std::ios::trunc);
        json b = json::array();
        for (auto v : bits) b.push_back(v);
        out << b.dump();
        out.close();
        if (out) cacheBloom(bloomPath, std::move(bits));
        LSM_LOG("[LSM][BLOOM] Built legacy bloom for " << sstPath);
    } catch (...) {}
}

bool LSM::mayExistInSST(const std::string& sstPath, const std::string& key) {
    try {
        if (pacificdb::storage_v2::isBinarySst(sstPath)) {
            const auto result = pacificdb::storage_v2::bloomMayContain(sstPath, key);
            if (!result.has_value()) {
                g_bloomCacheMisses.fetch_add(1, std::memory_order_relaxed);
                g_bloomFilterMisses.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            g_bloomCacheHits.fetch_add(1, std::memory_order_relaxed);
            (*result ? g_bloomFilterMisses : g_bloomFilterHits)
                .fetch_add(1, std::memory_order_relaxed);
            return *result;
        }
        std::string bloomPath = sstPath + std::string(".bloom");
        std::shared_ptr<const std::vector<uint64_t>> bits;
        {
            std::shared_lock<std::shared_mutex> lock(g_bloomCacheMutex);
            auto found = g_bloomCache.find(bloomPath);
            if (found != g_bloomCache.end()) bits = found->second;
        }
        if (bits) {
            g_bloomCacheHits.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::ifstream in(bloomPath);
            if (!in.is_open()) {
                g_bloomCacheMisses.fetch_add(1, std::memory_order_relaxed);
                g_bloomFilterMisses.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            json parsed;
            in >> parsed;
            if (!parsed.is_array() || parsed.size() != BLOOM_SIZE / 64) return true;
            std::vector<uint64_t> loaded;
            loaded.reserve(parsed.size());
            for (const auto& word : parsed) loaded.push_back(word.get<uint64_t>());
            cacheBloom(bloomPath, std::move(loaded));
            {
                std::shared_lock<std::shared_mutex> lock(g_bloomCacheMutex);
                bits = g_bloomCache.at(bloomPath);
            }
            g_bloomCacheMisses.fetch_add(1, std::memory_order_relaxed);
        }
        uint64_t h = std::hash<std::string>{}(key);
        size_t idx = h % BLOOM_SIZE;
        const bool mayExist = (((*bits)[idx / 64] >> (idx % 64)) & 1ULL) != 0;
        (mayExist ? g_bloomFilterMisses : g_bloomFilterHits).fetch_add(1, std::memory_order_relaxed);
        return mayExist;
    } catch (...) { return true; }
}

static void buildSparseIndexForSST(const std::string& sstPath) {
    try {
        if (pacificdb::storage_v2::isBinarySst(sstPath)) return; // block index is embedded
        std::ifstream in(sstPath);
        if (!in.is_open()) return;

        const std::string indexPath = sstPath + ".sidx";
        std::ofstream out(indexPath, std::ios::trunc);
        if (!out.is_open()) return;

        std::string line;
        size_t row = 0;
        while (true) {
            std::streampos pos = in.tellg();
            if (!std::getline(in, line)) break;
            if (line.empty()) {
                ++row;
                continue;
            }
            if (row == 0 || (row % SPARSE_INDEX_STRIDE) == 0) {
                try {
                    auto d = json::parse(line);
                    if (d.contains("id") && d["id"].is_string()) {
                        out << d["id"].get<std::string>() << "\t" << static_cast<long long>(pos) << "\n";
                    }
                } catch (...) {}
            }
            ++row;
        }
    } catch (...) {}
}

static std::optional<json> seekDocInSSTById(const std::string& sstPath, const std::string& id) {
    if (pacificdb::storage_v2::isBinarySst(sstPath)) {
        if (!LSM::mayExistInSST(sstPath, id)) return std::nullopt;
        std::string error;
        auto found = pacificdb::storage_v2::findInSst(sstPath, id, &error);
        if (!error.empty()) throw std::runtime_error("cannot read binary SST: " + error);
        if (!found) return std::nullopt;
        uint64_t committedIndex = 0;
        try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) {}
        return docVisibleByCommitIndex(*found, committedIndex) ? found : std::nullopt;
    }
    try {
        if (!LSM::mayExistInSST(sstPath, id)) return std::nullopt;

        long long startOffset = 0;
        const std::string indexPath = sstPath + ".sidx";
        if (fs::exists(indexPath)) {
            std::ifstream idx(indexPath);
            std::string line;
            std::vector<std::pair<std::string, long long>> points;
            while (std::getline(idx, line)) {
                auto tab = line.find('\t');
                if (tab == std::string::npos) continue;
                std::string sid = line.substr(0, tab);
                try {
                    long long off = std::stoll(line.substr(tab + 1));
                    points.push_back({sid, off});
                } catch (...) {}
            }

            if (!points.empty()) {
                size_t lo = 0, hi = points.size();
                while (lo < hi) {
                    size_t mid = lo + (hi - lo) / 2;
                    // Start before the requested key's duplicate run. Seeking
                    // to the last equal sparse entry can skip a newer version
                    // that appears earlier in the same SST.
                    if (points[mid].first < id) lo = mid + 1;
                    else hi = mid;
                }
                if (lo > 0) startOffset = points[lo - 1].second;
            }
        }

        uint64_t committedIndex = 0;
        try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) { committedIndex = 0; }

        std::ifstream sst(sstPath);
        if (!sst.is_open()) return std::nullopt;
        sst.seekg(startOffset, std::ios::beg);

        std::optional<json> newest;
        std::string docLine;
        while (std::getline(sst, docLine)) {
            if (docLine.empty()) continue;
            try {
                auto d = json::parse(docLine);
                if (!d.contains("id") || !d["id"].is_string()) continue;
                const std::string did = d["id"].get<std::string>();
                if (!docVisibleByCommitIndex(d, committedIndex)) continue;
                if (did == id) {
                    if (!newest.has_value() || isCandidateNewer(d, *newest)) {
                        newest = std::move(d);
                    }
                    continue;
                }
                if (did > id) break; // SST is id-sorted, so we can stop early.
            } catch (...) {}
        }
        return newest;
    } catch (...) {}

    return std::nullopt;
}

// ---------------- COLUMNAR INDEX ----------------
static bool shouldIndexColumnField(const std::string& field, const json& value) {
    if (field.empty() || field == "id" || field == "_deleted") return false;
    if (!field.empty() && field[0] == '_') return false;
    return value.is_string() || value.is_number() || value.is_boolean() || value.is_null();
}

static std::string columnIndexValue(const json& value);

static fs::path indexCatalogPath(const fs::path& idxDir) {
    return idxDir / "_catalog.json";
}

static json loadIndexCatalog(const fs::path& idxDir) {
    json catalog = {{"version", 1}, {"indexes", json::array()}};
    try {
        std::ifstream in(indexCatalogPath(idxDir));
        if (in.is_open()) in >> catalog;
    } catch (...) {}
    if (!catalog.is_object()) catalog = json::object();
    if (!catalog.contains("version")) catalog["version"] = 1;
    if (!catalog.contains("indexes") || !catalog["indexes"].is_array()) catalog["indexes"] = json::array();
    return catalog;
}

static void writeIndexCatalog(const fs::path& idxDir, const json& catalog) {
    createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), idxDir);
    fs::path target = indexCatalogPath(idxDir);
    fs::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out.is_open()) throw std::runtime_error("cannot write index catalog");
        out << catalog.dump(2);
        out.flush();
        if (!out.good()) throw std::runtime_error("cannot flush index catalog");
    }
    std::error_code ec;
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(target, ec);
        ec.clear();
        fs::rename(temporary, target, ec);
        if (ec) throw std::runtime_error("cannot replace index catalog");
    }
}

static std::optional<json> readyIndexForField(const fs::path& idxDir, const std::string& field) {
    json catalog = loadIndexCatalog(idxDir);
    for (const auto& def : catalog["indexes"]) {
        if (!def.is_object()) continue;
        if (def.value("field", std::string()) != field) continue;
        if (def.value("status", std::string()) != "ready" || !def.value("complete", false)) continue;
        return std::optional<json>(json(def));
    }
    return std::nullopt;
}

static std::unordered_set<std::string> catalogIndexedFields(const fs::path& idxDir) {
    std::unordered_set<std::string> fields;
    json catalog = loadIndexCatalog(idxDir);
    for (const auto& def : catalog["indexes"]) {
        if (!def.is_object() || def.value("status", std::string()) != "ready") continue;
        std::string field = def.value("field", std::string());
        if (!field.empty()) fields.insert(field);
    }
    return fields;
}

static json buildFieldIndexUnbounded(const std::vector<json>& docs, const std::string& field) {
    json index = json::object();
    for (const auto& doc : docs) {
        if (!doc.is_object() || isDeletedDoc(doc) || !doc.contains("id") || !doc.contains(field)) continue;
        if (!shouldIndexColumnField(field, doc[field])) continue;
        std::string id;
        try { id = doc["id"].is_string() ? doc["id"].get<std::string>() : doc["id"].dump(); } catch (...) { continue; }
        if (id.empty()) continue;
        auto& ids = index[columnIndexValue(doc[field])];
        if (!ids.is_array()) ids = json::array();
        bool exists = false;
        for (const auto& existing : ids) if (existing.is_string() && existing.get<std::string>() == id) { exists = true; break; }
        if (!exists) ids.push_back(id);
    }
    return index;
}

static std::string columnIndexValue(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "null";
    return value.dump();
}

static size_t columnIndexMaxIdsPerValue() {
    return envSizeT("COLUMN_INDEX_MAX_IDS_PER_VALUE", 256);
}

static size_t columnIndexMaxValuesPerField() {
    return envSizeT("COLUMN_INDEX_MAX_VALUES_PER_FIELD", 512);
}

static size_t columnIndexTargetedSeekMaxIds() {
    return envSizeT("COLUMN_INDEX_TARGETED_SEEK_MAX_IDS", 128);
}

static json* getOrCreateColumnIndexArray(json& idx, const std::string& encodedValue) {
    if (idx.contains(encodedValue)) {
        auto& existing = idx[encodedValue];
        if (!existing.is_array()) existing = json::array();
        return &existing;
    }

    const size_t maxValues = columnIndexMaxValuesPerField();
    if (maxValues == 0 || idx.size() >= maxValues) {
        return nullptr;
    }

    auto& created = idx[encodedValue];
    if (!created.is_array()) created = json::array();
    return &created;
}

static bool addIdToColumnIndexArray(json& arr, const std::string& id) {
    if (!arr.is_array()) arr = json::array();
    if (id.empty()) return false;
    for (auto& existing : arr) {
        try {
            if ((existing.is_string() ? existing.get<std::string>() : existing.dump()) == id) {
                return false;
            }
        } catch (...) {}
    }

    const size_t maxIds = columnIndexMaxIdsPerValue();
    if (maxIds == 0) return false;
    arr.insert(arr.begin(), id);
    while (arr.size() > maxIds) {
        arr.erase(std::prev(arr.end()));
    }
    return true;
}

static bool docFieldMatchesColumnValue(const json& doc, const std::string& field, const std::string& encodedValue) {
    if (!doc.is_object() || !doc.contains(field)) return false;
    try {
        return columnIndexValue(doc[field]) == encodedValue;
    } catch (...) {
        return false;
    }
}

static fs::path indexFieldArtifactPath(const fs::path& idxDir,
                                       const std::string& field) {
    const std::string safeField =
        validateStorageIdentifier(field, "indexFieldName");
    return LSM::requireContained(idxDir / (safeField + ".json"));
}

static fs::path indexFieldSegmentDirectory(const fs::path& idxDir,
                                           const std::string& field) {
    const std::string safeField = validateStorageIdentifier(field, "indexFieldName");
    return LSM::requireContained(idxDir / (safeField + ".cidx"));
}

static std::shared_ptr<BinaryColumnIndexState> binaryColumnIndexState(
    const fs::path& directory) {
    const std::string key = directory.string();
    std::lock_guard<std::mutex> lock(g_binaryColIdxCacheMutex);
    auto& state = g_binaryColIdxCache[key];
    if (!state) state = std::make_shared<BinaryColumnIndexState>();
    return state;
}

static void applyBinaryIndexMutation(BinaryColumnIndexState& state,
                                     const pacificdb::storage_v2::IndexMutation& mutation) {
    auto existing = state.valueById.find(mutation.id);
    if (existing != state.valueById.end()) {
        auto value = state.idsByValue.find(existing->second);
        if (value != state.idsByValue.end()) {
            value->second.erase(mutation.id);
            if (value->second.empty()) state.idsByValue.erase(value);
        }
        state.valueById.erase(existing);
    }
    if (mutation.op == pacificdb::storage_v2::IndexMutation::Op::Put) {
        state.valueById[mutation.id] = mutation.value;
        state.idsByValue[mutation.value].insert(mutation.id);
    }
}

static void loadBinaryColumnIndexLocked(const fs::path& directory,
                                        BinaryColumnIndexState& state) {
    if (state.loaded) return;
    state.valueById.clear();
    state.idsByValue.clear();
    for (const auto& path : pacificdb::storage_v2::listIndexSegments(directory)) {
        pacificdb::storage_v2::IndexSegment segment;
        std::string error;
        if (!pacificdb::storage_v2::readIndexSegment(path, segment, &error)) {
            throw std::runtime_error("cannot read column-index segment " + path.string() + ": " + error);
        }
        if (segment.snapshot) {
            state.valueById.clear();
            state.idsByValue.clear();
        }
        for (const auto& mutation : segment.mutations) {
            applyBinaryIndexMutation(state, mutation);
        }
    }
    state.loaded = true;
}

static std::vector<pacificdb::storage_v2::IndexMutation> binaryIndexSnapshotMutations(
    const json& index) {
    std::vector<pacificdb::storage_v2::IndexMutation> mutations;
    if (!index.is_object()) return mutations;
    for (auto value = index.begin(); value != index.end(); ++value) {
        if (!value.value().is_array()) continue;
        for (const auto& rawId : value.value()) {
            try {
                const std::string id = rawId.is_string() ? rawId.get<std::string>() : rawId.dump();
                if (!id.empty()) mutations.push_back({
                    pacificdb::storage_v2::IndexMutation::Op::Put, value.key(), id});
            } catch (...) {}
        }
    }
    return mutations;
}

static void replaceBinaryColumnIndex(const fs::path& idxDir,
                                     const std::string& field,
                                     const json& index) {
    const fs::path directory = indexFieldSegmentDirectory(idxDir, field);
    const auto previous = pacificdb::storage_v2::listIndexSegments(directory);
    fs::path published;
    std::string error;
    auto mutations = binaryIndexSnapshotMutations(index);
    if (!pacificdb::storage_v2::writeIndexSegment(
            directory, mutations, true, &published, &error)) {
        throw std::runtime_error("cannot publish binary column index: " + error);
    }
    std::error_code ec;
    for (const auto& path : previous) {
        if (path != published) fs::remove(path, ec);
    }
    fs::remove(indexFieldArtifactPath(idxDir, field), ec);
    auto state = binaryColumnIndexState(directory);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->loaded = false;
    state->valueById.clear();
    state->idsByValue.clear();
    g_binaryColIdxSegmentsWritten.fetch_add(1, std::memory_order_relaxed);
    std::error_code sizeEc;
    g_binaryColIdxBytesWritten.fetch_add(fs::file_size(published, sizeEc), std::memory_order_relaxed);
}

static bool ensureBinaryColumnIndex(const fs::path& idxDir,
                                    const std::string& field) {
    const fs::path directory = indexFieldSegmentDirectory(idxDir, field);
    if (!pacificdb::storage_v2::listIndexSegments(directory).empty()) return true;
    json legacy = json::object();
    const fs::path legacyPath = indexFieldArtifactPath(idxDir, field);
    if (fs::exists(legacyPath)) {
        std::ifstream in(legacyPath);
        if (in.is_open()) in >> legacy;
    }
    replaceBinaryColumnIndex(idxDir, field, legacy);
    return true;
}

static bool binaryColumnIndexLookup(const fs::path& idxDir,
                                    const std::string& field,
                                    const std::string& value,
                                    size_t candidateLimit,
                                    std::vector<std::string>& idsOut,
                                    bool& fileExists,
                                    bool& valueHit) {
    const fs::path directory = indexFieldSegmentDirectory(idxDir, field);
    if (pacificdb::storage_v2::listIndexSegments(directory).empty()) return false;
    fileExists = true;
    auto state = binaryColumnIndexState(directory);
    std::lock_guard<std::mutex> lock(state->mutex);
    loadBinaryColumnIndexLocked(directory, *state);
    auto found = state->idsByValue.find(value);
    if (found == state->idsByValue.end()) return true;
    valueHit = true;
    idsOut.reserve(found->second.size());
    for (const auto& id : found->second) {
        idsOut.push_back(id);
        if (candidateLimit > 0 && idsOut.size() >= candidateLimit) break;
    }
    return true;
}

static json readColumnIndexAsJson(const fs::path& idxDir,
                                  const std::string& field) {
    const fs::path directory = indexFieldSegmentDirectory(idxDir, field);
    if (!pacificdb::storage_v2::listIndexSegments(directory).empty()) {
        auto state = binaryColumnIndexState(directory);
        std::lock_guard<std::mutex> lock(state->mutex);
        loadBinaryColumnIndexLocked(directory, *state);
        json index = json::object();
        for (const auto& [value, ids] : state->idsByValue) {
            auto& array = index[value];
            array = json::array();
            std::vector<std::string> orderedIds(ids.begin(), ids.end());
            std::sort(orderedIds.begin(), orderedIds.end());
            for (const auto& id : orderedIds) array.push_back(id);
        }
        return index;
    }
    json legacy = json::object();
    std::ifstream in(indexFieldArtifactPath(idxDir, field));
    if (in.is_open()) in >> legacy;
    return legacy;
}

static void applyBinaryColumnIndexBatch(const fs::path& idxDir,
                                        const std::vector<json>& docs) {
    if (docs.empty()) return;
    const auto selectedFields = catalogIndexedFields(idxDir);
    if (selectedFields.empty()) return;

    std::unordered_map<std::string, const json*> latest;
    for (const auto& doc : docs) {
        if (!doc.is_object() || !doc.contains("id")) continue;
        try {
            const std::string id = doc["id"].is_string()
                ? doc["id"].get<std::string>() : doc["id"].dump();
            if (!id.empty()) latest[id] = &doc;
        } catch (...) {}
    }
    for (const auto& field : selectedFields) {
        ensureBinaryColumnIndex(idxDir, field);
        std::vector<pacificdb::storage_v2::IndexMutation> mutations;
        mutations.reserve(latest.size() * 2);
        for (const auto& [id, doc] : latest) {
            mutations.push_back({pacificdb::storage_v2::IndexMutation::Op::Clear, "", id});
            if (!isDeletedDoc(*doc) && doc->contains(field) &&
                shouldIndexColumnField(field, (*doc)[field])) {
                mutations.push_back({pacificdb::storage_v2::IndexMutation::Op::Put,
                                     columnIndexValue((*doc)[field]), id});
            }
        }
        if (mutations.empty()) continue;
        const fs::path directory = indexFieldSegmentDirectory(idxDir, field);
        fs::path published;
        std::string error;
        if (!pacificdb::storage_v2::writeIndexSegment(
                directory, mutations, false, &published, &error)) {
            throw std::runtime_error("cannot append column-index segment: " + error);
        }
        auto state = binaryColumnIndexState(directory);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->loaded) {
                for (const auto& mutation : mutations) applyBinaryIndexMutation(*state, mutation);
            }
        }
        g_binaryColIdxSegmentsWritten.fetch_add(1, std::memory_order_relaxed);
        std::error_code sizeEc;
        g_binaryColIdxBytesWritten.fetch_add(fs::file_size(published, sizeEc), std::memory_order_relaxed);
    }
}

static void compactBinaryColumnIndexes(const fs::path& idxDir) {
    static const size_t threshold = envSizeT("COLUMN_INDEX_COMPACTION_SEGMENTS", 32);
    std::error_code ec;
    if (!fs::is_directory(idxDir, ec)) return;
    for (const auto& entry : fs::directory_iterator(idxDir, ec)) {
        if (ec || !entry.is_directory() || entry.path().extension() != ".cidx") continue;
        const auto segments = pacificdb::storage_v2::listIndexSegments(entry.path());
        if (segments.size() < threshold) continue;
        auto state = binaryColumnIndexState(entry.path());
        std::lock_guard<std::mutex> lock(state->mutex);
        loadBinaryColumnIndexLocked(entry.path(), *state);
        std::vector<pacificdb::storage_v2::IndexMutation> snapshot;
        snapshot.reserve(state->valueById.size());
        for (const auto& [id, value] : state->valueById) {
            snapshot.push_back({pacificdb::storage_v2::IndexMutation::Op::Put, value, id});
        }
        fs::path published;
        std::string error;
        if (!pacificdb::storage_v2::writeIndexSegment(
                entry.path(), std::move(snapshot), true, &published, &error)) {
            throw std::runtime_error("cannot compact column index: " + error);
        }
        for (const auto& path : segments) if (path != published) fs::remove(path, ec);
        g_binaryColIdxCompactions.fetch_add(1, std::memory_order_relaxed);
        g_binaryColIdxSegmentsWritten.fetch_add(1, std::memory_order_relaxed);
        std::error_code sizeEc;
        g_binaryColIdxBytesWritten.fetch_add(fs::file_size(published, sizeEc), std::memory_order_relaxed);
    }
}

static void applyColumnIndexBatch(const fs::path& idxDir, const std::vector<json>& docs) {
    applyBinaryColumnIndexBatch(idxDir, docs);
}

static json rebuildOneCatalogIndex(const std::string& userId, const std::string& dbName,
                                   const std::string& collection, const std::string& name) {
    const std::string key = colKey(userId, dbName, collection);
    fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
    std::unique_lock<std::shared_mutex> lk(getColIdxMutex(key));
    auto docs = LSM::getAll(userId, dbName, collection);
    json catalog = loadIndexCatalog(idxDir);
    json* definition = nullptr;
    for (auto& def : catalog["indexes"]) if (def.value("name", std::string()) == name) { definition = &def; break; }
    if (!definition) return {{"status", "error"}, {"error", "index_not_found"}, {"name", name}};
    const std::string field = definition->value("field", std::string());
    validateStorageIdentifier(field, "indexFieldName");
    (*definition)["status"] = "building";
    (*definition)["complete"] = false;
    writeIndexCatalog(idxDir, catalog);
    json index = buildFieldIndexUnbounded(docs, field);
    replaceBinaryColumnIndex(idxDir, field, index);
    catalog = loadIndexCatalog(idxDir);
    for (auto& def : catalog["indexes"]) {
        if (def.value("name", std::string()) != name) continue;
        def["status"] = "ready"; def["complete"] = true; def["documents"] = docs.size();
        def["updatedAtMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    writeIndexCatalog(idxDir, catalog);
    return {{"status", "ok"}, {"name", name}, {"field", field}, {"documents", docs.size()}, {"complete", true}};
}

nlohmann::json LSM::createSecondaryIndex(const std::string& userId, const std::string& dbName,
                                         const std::string& collection, const json& definition) {
    try {
        std::string name = definition.value("name", std::string());
        std::string field = definition.value("field", std::string());
        std::string type = definition.value("type", std::string("btree"));
        bool unique = definition.value("unique", false);
        if (name.empty()) name = field + "_1";
        validateStorageIdentifier(name, "indexName");
        validateStorageIdentifier(field, "indexFieldName");
        auto valid = [](const std::string& value) { return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; }); };
        if (!valid(name) || !valid(field) || field == "id" || field[0] == '_') return {{"status", "error"}, {"error", "invalid_index_definition"}};
        if (type != "btree" || unique || definition.value("fields", json::array()).size() > 1) return {{"status", "error"}, {"error", "index_type_not_supported"}, {"supported", json::array({"single_field_btree"})}};
        const std::string key = colKey(userId, dbName, collection);
        fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
        {
            std::unique_lock<std::shared_mutex> lk(getColIdxMutex(key));
            json catalog = loadIndexCatalog(idxDir);
            for (const auto& def : catalog["indexes"]) {
                if (def.value("name", std::string()) == name || def.value("field", std::string()) == field) {
                    if (def.value("name", std::string()) == name && def.value("field", std::string()) == field) return def;
                    return {{"status", "error"}, {"error", "index_conflict"}};
                }
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            catalog["indexes"].push_back({{"name", name}, {"type", "btree"}, {"field", field}, {"fields", json::array({{{"field", field}, {"order", definition.value("order", 1) < 0 ? -1 : 1}}})}, {"unique", false}, {"sparse", definition.value("sparse", false)}, {"status", "building"}, {"complete", false}, {"createdAtMs", now}, {"updatedAtMs", now}});
            writeIndexCatalog(idxDir, catalog);
        }
        return rebuildOneCatalogIndex(userId, dbName, collection, name);
    } catch (const std::exception& e) { return {{"status", "error"}, {"error", e.what()}}; }
}

nlohmann::json LSM::listSecondaryIndexes(const std::string& userId, const std::string& dbName, const std::string& collection) {
    fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
    json catalog = loadIndexCatalog(idxDir);
    json indexes = json::array({{{"name", "_id_"}, {"type", "primary"}, {"field", "id"}, {"status", "ready"}, {"complete", true}, {"managed", true}}});
    for (const auto& def : catalog["indexes"]) indexes.push_back(def);
    return {{"status", "ok"}, {"indexes", indexes}};
}

nlohmann::json LSM::dropSecondaryIndex(const std::string& userId, const std::string& dbName,
                                       const std::string& collection, const std::string& name) {
    if (name == "_id_") return {{"status", "error"}, {"error", "primary_index_immutable"}};
    try {
        validateStorageIdentifier(name, "indexName");
        const std::string key = colKey(userId, dbName, collection);
        fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
        std::unique_lock<std::shared_mutex> lk(getColIdxMutex(key));
        json catalog = loadIndexCatalog(idxDir), next = json::array();
        std::string field;
        for (const auto& def : catalog["indexes"]) {
            if (def.value("name", std::string()) == name) field = def.value("field", std::string()); else next.push_back(def);
        }
        if (field.empty()) return {{"status", "ok"}, {"dropped", false}, {"name", name}};
        catalog["indexes"] = next; writeIndexCatalog(idxDir, catalog);
        bool fieldStillUsed = false;
        for (const auto& def : next) if (def.value("field", std::string()) == field) fieldStillUsed = true;
        if (!fieldStillUsed) {
            std::error_code ec;
            fs::remove(indexFieldArtifactPath(idxDir, field), ec);
            fs::remove_all(indexFieldSegmentDirectory(idxDir, field), ec);
            std::lock_guard<std::mutex> cacheLock(g_binaryColIdxCacheMutex);
            g_binaryColIdxCache.erase(indexFieldSegmentDirectory(idxDir, field).string());
        }
        return {{"status", "ok"}, {"dropped", true}, {"name", name}};
    } catch (const std::exception& e) { return {{"status", "error"}, {"error", e.what()}}; }
}

nlohmann::json LSM::rebuildSecondaryIndex(const std::string& userId, const std::string& dbName,
                                          const std::string& collection, const std::string& name) {
    try {
        validateStorageIdentifier(name, "indexName");
        return rebuildOneCatalogIndex(userId, dbName, collection, name);
    }
    catch (const std::exception& e) { return {{"status", "error"}, {"error", e.what()}}; }
}

static json buildExpectedColumnIndexes(const std::vector<json>& docs) {
    json expected = json::object();
    for (const auto& doc : docs) {
        if (!doc.is_object() || isDeletedDoc(doc) || !doc.contains("id")) continue;
        std::string id;
        try {
            id = doc["id"].is_string() ? doc["id"].get<std::string>() : doc["id"].dump();
        } catch (...) {
            continue;
        }
        if (id.empty()) continue;
        for (auto& [field, value] : doc.items()) {
            if (!shouldIndexColumnField(field, value)) continue;
            json* arr = getOrCreateColumnIndexArray(expected[field], columnIndexValue(value));
            if (arr) addIdToColumnIndexArray(*arr, id);
        }
    }
    return expected;
}

void LSM::updateColumnIndexes(const std::string& userId, const std::string& dbName, const std::string& collection, const json& doc) {
    if (!doc.is_object() || !doc.contains("id")) return;
    fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
    createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), idxDir);
    // Serialize per-collection column index file I/O. This lock is separate from the
    // collection write lock so WAL+memtable writers are not blocked during index updates.
    const std::string key = colKey(userId, dbName, collection);
    std::unique_lock<std::shared_mutex> idxLock(getColIdxMutex(key));
    applyColumnIndexBatch(idxDir, {doc});
}

void LSM::updateColumnIndexesBatch(const std::string& userId, const std::string& dbName, const std::string& collection, const std::vector<json>& docs) {
    fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
    applyColumnIndexBatch(idxDir, docs);
}

nlohmann::json LSM::rebuildColumnIndexes(const std::string& userId, const std::string& dbName, const std::string& collection) {
    json result = {
        {"status", "ok"},
        {"userId", userId},
        {"database", dbName},
        {"collection", collection},
        {"indexedFields", json::array()},
        {"documents", 0}
    };
    try {
        fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
        json catalog = loadIndexCatalog(idxDir);
        for (const auto& def : catalog["indexes"]) {
            const std::string name = def.value("name", std::string());
            if (name.empty()) continue;
            auto rebuilt = rebuildOneCatalogIndex(userId, dbName, collection, name);
            if (rebuilt.value("status", std::string()) == "error") return rebuilt;
            result["documents"] = rebuilt.value("documents", 0);
            result["indexedFields"].push_back(rebuilt.value("field", std::string()));
        }
    } catch (const std::exception& e) {
        result["status"] = "error";
        result["error"] = e.what();
    } catch (...) {
        result["status"] = "error";
        result["error"] = "unknown index rebuild failure";
    }
    return result;
}

// V11.4-IDX-001 diagnostics: raw, UNFILTERED base traversal. Deliberately does not
// apply docVisibleByCommitIndex, so the physical set of persisted base records can be
// compared against the physical on-disk index independently of read visibility.
// Diagnostic only: it does not participate in any status decision.
static std::vector<json> rawUnfilteredBaseRecords(const std::string& userId,
                                                  const std::string& dbName,
                                                  const std::string& collection) {
    const std::string key = colKey(userId, dbName, collection);
    std::unordered_map<std::string, json> latestById;
    std::vector<json> noIdDocs;

    fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
    if (fs::exists(dir)) {
        const auto sstFiles = publishedSstsForCollection(dir);
        for (const auto& sstPath : sstFiles) {
            std::string error;
            if (!scanSstRows(sstPath, [&](json&& row) {
                    mergeLatestById(latestById, noIdDocs, row);
                    return true;
                }, &error)) {
                throw std::runtime_error("cannot scan SST " + sstPath.string() + ": " + error);
            }
        }
    }
    {
        auto lk = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(key));
        auto immutable = immutableSnapshotForKey(key);
        for (auto it = immutable.rbegin(); it != immutable.rend(); ++it) {
            if (!*it) continue;
            for (const auto& [id, doc] : **it) { (void)id; mergeLatestById(latestById, noIdDocs, doc); }
        }
        auto mtIt = memtables.find(key);
        if (mtIt != memtables.end()) {
            for (const auto& [id, doc] : mtIt->second) { (void)id; mergeLatestById(latestById, noIdDocs, doc); }
        }
    }
    std::vector<json> out;
    out.reserve(latestById.size() + noIdDocs.size());
    for (auto& [id, doc] : latestById) { (void)id; out.push_back(doc); }
    for (auto& doc : noIdDocs) out.push_back(doc);
    return out;
}

nlohmann::json LSM::validateColumnIndexes(const std::string& userId, const std::string& dbName, const std::string& collection) {
    json result = {
        {"status", "ok"},
        {"userId", userId},
        {"database", dbName},
        {"collection", collection},
        {"missingEntries", 0},
        {"staleEntries", 0},
        {"extraEntries", 0},
        {"invalidPointers", 0},
        {"fields", json::array()},
        {"mismatches", json::array()},
        {"declaredIndexes", 0},
        {"indexedFieldsCovered", 0},
        {"validationComplete", false}
    };
    try {
        fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
        auto docs = LSM::getAll(userId, dbName, collection);

        // V11.4-IDX-001 diagnostics. The visible set (getAll) and the physical set (raw
        // unfiltered base traversal) are two different contracts. Both are reported so the
        // defect can be attributed; STATUS BELOW IS UNCHANGED and still derives from the
        // existing visible-set comparison until attribution is complete.
        const auto rawDocs = rawUnfilteredBaseRecords(userId, dbName, collection);
        std::unordered_set<std::string> physicalDocumentIds;
        for (const auto& doc : rawDocs) {
            if (!doc.is_object() || !doc.contains("id")) continue;
            try {
                physicalDocumentIds.insert(
                    doc["id"].is_string()
                        ? doc["id"].get<std::string>()
                        : doc["id"].dump());
            } catch (...) {}
        }
        result["diagnostics"] = {
            {"visibleBaseRecordCount", docs.size()},
            {"physicalBaseRecordCount", rawDocs.size()},
            {"hiddenBaseRecordCount",
                rawDocs.size() >= docs.size() ? rawDocs.size() - docs.size() : 0},
            {"indexQueue", columnIndexQueueState(colKey(userId, dbName, collection))},
            {"note", "physical set is raw unfiltered base storage; visible set is "
                     "docVisibleByCommitIndex-filtered getAll()"},
        };
        try {
            result["diagnostics"]["raftCommitIndex"] = RaftCore::instance().getCommitIndex();
            result["diagnostics"]["raftLastApplied"] = RaftCore::instance().getLastApplied();
        } catch (...) {}
        json catalog = loadIndexCatalog(idxDir);
        json expected = json::object();
        std::set<std::string> fieldNames;
        for (const auto& def : catalog["indexes"]) {
            const std::string field = def.value("field", std::string());
            if (field.empty()) continue;
            fieldNames.insert(field);
            expected[field] = buildFieldIndexUnbounded(docs, field);
        }
        result["declaredIndexes"] = catalog["indexes"].size();
        result["indexedFieldsCovered"] = fieldNames.size();
        result["rebuildState"] = LSM::indexRecoveryStatus();

        // V11.4-IDX-001 validator contract. A catalog that declares indexes while the
        // validator covers zero fields is NOT clean: the index definitions are gone and
        // the comparison below would be vacuous, reporting 0 missing / 0 stale for a
        // collection that has lost every index.
        if (result["declaredIndexes"].get<size_t>() > 0 && fieldNames.empty()) {
            result["status"] = "COVERAGE_COLLAPSE";
            result["validationComplete"] = true;
            result["error"] = "catalog declares indexes but no indexed field is covered";
            return result;
        }
        // A destroyed catalog reports zero declared indexes, which is otherwise
        // indistinguishable from a collection that legitimately has none. Orphaned
        // per-field index data files are proof that indexes DID exist, so refuse to call
        // this clean: it is the false-clean signature of a lost catalog.
        if (result["declaredIndexes"].get<size_t>() == 0 && fs::exists(idxDir)) {
            std::vector<std::string> orphaned;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(idxDir, ec)) {
                if (ec) break;
                if (e.path().filename() == "_catalog.json") continue;
                if (e.path().extension() != ".json" && e.path().extension() != ".cidx") continue;
                orphaned.push_back(e.path().filename().string());
            }
            if (!orphaned.empty()) {
                result["status"] = "COVERAGE_COLLAPSE";
                result["validationComplete"] = true;
                result["orphanedIndexDataFiles"] = orphaned;
                result["error"] = "index data files exist without any catalog definition; "
                                  "index definitions were lost";
                return result;
            }
        }
        // An index rebuild that is still running is legitimate in-progress work, not a
        // corrupt index: report it distinctly instead of needs_rebuild.
        if (!LSM::indexRecoverySettled()) {
            result["status"] = LSM::indexRecoveryState() == LSM::IndexRecoveryState::FAILED
                ? "INDEX_REBUILD_FAILED" : "INDEX_REBUILDING";
            result["validationComplete"] = false;
            return result;
        }

        size_t physicalIndexEntries = 0;
        size_t invalidPointers = 0;
        for (const auto& fieldName : fieldNames) {
            json expectedField = expected.contains(fieldName) ? expected[fieldName] : json::object();
            json existing = readColumnIndexAsJson(idxDir, fieldName);
            fs::path idxFile = !pacificdb::storage_v2::listIndexSegments(
                    indexFieldSegmentDirectory(idxDir, fieldName)).empty()
                ? indexFieldSegmentDirectory(idxDir, fieldName)
                : indexFieldArtifactPath(idxDir, fieldName);
            size_t fieldMissing = 0;
            size_t fieldStale = 0;
            std::set<std::string> values;
            if (expectedField.is_object()) {
                for (auto valueIt = expectedField.begin(); valueIt != expectedField.end(); ++valueIt) {
                    values.insert(valueIt.key());
                }
            }
            if (existing.is_object()) {
                for (auto valueIt = existing.begin(); valueIt != existing.end(); ++valueIt) {
                    values.insert(valueIt.key());
                }
            }

            for (const auto& encodedValue : values) {
                std::unordered_set<std::string> expectedIds;
                if (expectedField.contains(encodedValue) && expectedField[encodedValue].is_array()) {
                    for (auto& id : expectedField[encodedValue]) {
                        if (id.is_string()) expectedIds.insert(id.get<std::string>());
                    }
                }
                std::unordered_set<std::string> existingIds;
                if (existing.contains(encodedValue) && existing[encodedValue].is_array()) {
                    for (auto& id : existing[encodedValue]) {
                        try {
                            const std::string decodedId =
                                id.is_string() ? id.get<std::string>() : id.dump();
                            if (existingIds.insert(decodedId).second) {
                                ++physicalIndexEntries;
                                if (physicalDocumentIds.find(decodedId)
                                    == physicalDocumentIds.end()) {
                                    ++invalidPointers;
                                }
                            }
                        } catch (...) {}
                    }
                }
                for (const auto& id : expectedIds) {
                    if (existingIds.find(id) == existingIds.end()) {
                        fieldMissing++;
                        // V11.4-IDX-001: report the exact identity of every mismatch.
                        // An aggregate count cannot be attributed to a mechanism.
                        result["mismatches"].push_back({
                            {"classification", "MISSING_ENTRY"},
                            {"database", dbName},
                            {"collection", collection},
                            {"field", fieldName},
                            {"documentId", id},
                            {"expectedEncodedKey", encodedValue},
                            {"observedEncodedKey", nullptr},
                            {"indexFile", idxFile.string()},
                            {"baseSource", "visibility_filtered_getAll"},
                        });
                    }
                }
                for (const auto& id : existingIds) {
                    if (expectedIds.find(id) == expectedIds.end()) {
                        fieldStale++;
                        result["mismatches"].push_back({
                            {"classification", "EXTRA_ENTRY"},
                            {"database", dbName},
                            {"collection", collection},
                            {"field", fieldName},
                            {"documentId", id},
                            {"expectedEncodedKey", nullptr},
                            {"observedEncodedKey", encodedValue},
                            {"indexFile", idxFile.string()},
                            {"baseSource", "visibility_filtered_getAll"},
                        });
                    }
                }
            }
            result["missingEntries"] = result["missingEntries"].get<size_t>() + fieldMissing;
            result["staleEntries"] = result["staleEntries"].get<size_t>() + fieldStale;
            result["fields"].push_back({
                {"field", fieldName},
                {"missingEntries", fieldMissing},
                {"staleEntries", fieldStale}
            });
        }
        result["extraEntries"] = result["staleEntries"];
        result["invalidPointers"] = invalidPointers;
        result["diagnostics"]["physicalIndexEntries"] = physicalIndexEntries;
        result["validationComplete"] = true;
        if (result["missingEntries"].get<size_t>() || result["staleEntries"].get<size_t>()) {
            result["status"] = "needs_rebuild";
        }
    } catch (const std::exception& e) {
        result["status"] = "error";
        result["error"] = e.what();
    } catch (...) {
        result["status"] = "error";
        result["error"] = "unknown index validation failure";
    }
    return result;
}

void LSM::del(const std::string& userId, const std::string& dbName, const std::string& collection, const std::string& id) {
    std::string key = colKey(userId, dbName, collection);

    bool shouldFlush = false;
    uint64_t logicalBytes = 0;
    WalAppendResult walAppend;
    {
        auto lk = pacificdb::timing::makeTimedUniqueLock(getCollectionMutex(key));

        // ensure directory
        fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
        createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), dir);

        // write DELETE to collection WAL
        const fs::path walDir = LSM::requireContained(LSM::databasePath(userId, dbName) / "wal");
        std::string walFile = LSM::requireContained(
            walDir / (validateStorageIdentifier(collection, "collectionName") + ".wal")).string();
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json tomb = {
            {"id", id},
            {"_deleted", true},
            {"version", nowMs},
            {"_mvcc_version", nowMs},
            {"deleted_txn", nowMs},
            {"deleted_at_ms", nowMs}
        };
        uint64_t committedIndex = 0;
        try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) { committedIndex = 0; }
        stampVisibilityMetadata(tomb, committedIndex);
        json walEntry = {
            {"op","DELETE"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"id", id},
            {"data", tomb}
        };
        auto walStart = std::chrono::steady_clock::now();
        walAppend = WAL::log(walFile, walEntry);
        auto walEnd = std::chrono::steady_clock::now();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::WalAppend,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(walEnd - walStart).count()));

        // insert tombstone into memtable
        auto memtableStart = std::chrono::steady_clock::now();
        logicalBytes = json::to_msgpack(tomb).size();
        memtables[key][id] = tomb;
        upsertIdIndex(key, id, tomb);
        markWalApplied(key, walAppend.firstLsn, walAppend.lastLsn);
        LSM::updateColumnIndexes(userId, dbName, collection, tomb);
        auto memtableEnd = std::chrono::steady_clock::now();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::MemtableInsert,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(memtableEnd - memtableStart).count()));
        std::cout << "[LSM][DEL] " << key << " / id=" << id << "\n";
        shouldFlush = memtables[key].size() >= MEMTABLE_LIMIT;
    }
    g_bytesIngested.fetch_add(logicalBytes, std::memory_order_relaxed);

    if (shouldFlush) {
        std::cout << "[LSM] memtable threshold reached, flushing..." << std::endl;
        LSM::flush(userId, dbName, collection);
    }
}

// ---------------- BACKGROUND TASKS ----------------
static void maintenanceLoop() {
    while (bgRunning.load()) {
        g_compactionSweepsTotal.fetch_add(1, std::memory_order_relaxed);
        size_t totalSstFiles = 0;
        try {
            // iterate users and collections and compact when needed
            for (auto& u : fs::directory_iterator(LSM::rootOrThrow())) {
                if (!u.is_directory()) continue;
                std::string userId = u.path().filename().string();
                for (auto& db : fs::directory_iterator(u.path())) {
                    if (!db.is_directory()) continue;
                    std::string dbName = db.path().filename().string();
                    for (auto& c : fs::directory_iterator(db.path())) {
                        if (!c.is_directory()) continue;
                        std::string coll = c.path().filename().string();
                        // only process .lsm dirs
                        if (coll.size() > 4 && coll.substr(coll.size()-4) == ".lsm") {
                            std::string collection = coll.substr(0, coll.size()-4);
                            // Count SST files before compaction
                            totalSstFiles += publishedSstsForCollection(c.path()).size();
                            LSM::compact(userId, dbName, collection);
                            std::unique_lock<std::shared_mutex> indexLock(
                                getColIdxMutex(colKey(userId, dbName, collection)),
                                std::try_to_lock);
                            if (indexLock.owns_lock()) {
                                compactBinaryColumnIndexes(LSM::collectionArtifactPath(
                                    userId, dbName, collection, ".idx"));
                            }
                        }
                    }
                }
            }
        } catch (...) {}

        // v4.4H: Push LSM metrics to MetricsExporter for Prometheus scraping
        try {
            MetricsExporter::recordCustomMetric("lsm_compaction_active", (double)g_compactionActiveCount.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_compaction_sweeps_total", (double)g_compactionSweepsTotal.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_compaction_runs_total", (double)g_compactionRunsTotal.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_compaction_throttled_total", (double)g_compactionThrottledTotal.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_compaction_yield_ms_total", (double)g_compactionYieldMsTotal.load(std::memory_order_relaxed));
            const uint64_t ingested = g_bytesIngested.load(std::memory_order_relaxed);
            const uint64_t compacted = g_compactionBytesWritten.load(std::memory_order_relaxed);
            MetricsExporter::recordCustomMetric("lsm_bytes_ingested_total", (double)ingested);
            MetricsExporter::recordCustomMetric("lsm_compaction_bytes_read_total", (double)g_compactionBytesRead.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_compaction_bytes_written_total", (double)compacted);
            MetricsExporter::recordCustomMetric("lsm_compaction_write_amplification", ingested == 0 ? 0.0 : (double)compacted / (double)ingested);
            MetricsExporter::recordCustomMetric("lsm_sst_files_total", (double)totalSstFiles);
            MetricsExporter::recordCustomMetric("lsm_flush_queue_depth", (double)flushQueueDepth());
            MetricsExporter::recordCustomMetric("lsm_flush_queued_total", (double)g_flushQueuedTotal.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_flush_completed_total", (double)g_flushCompletedTotal.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_write_stalls_total", (double)g_writeStallsTotal.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_write_stall_wait_ms_total", (double)(g_writeStallWaitUs.load(std::memory_order_relaxed) / 1000));
            uint64_t scans = g_sstScansPerReadTotal.load(std::memory_order_relaxed);
            uint64_t cnt = g_sstScansPerReadCount.load(std::memory_order_relaxed);
            MetricsExporter::recordCustomMetric("lsm_read_amplification_avg_sst", cnt > 0 ? (double)scans / (double)cnt : 0.0);
            MetricsExporter::recordCustomMetric("lsm_sst_scans_per_read_total", (double)scans);
            MetricsExporter::recordCustomMetric("lsm_read_queue_depth", (double)currentReadQueueDepth());
            MetricsExporter::recordCustomMetric("lsm_col_idx_cache_hits_total", (double)g_colIdxCacheHits.load(std::memory_order_relaxed));
            MetricsExporter::recordCustomMetric("lsm_col_idx_cache_misses_total", (double)g_colIdxCacheMisses.load(std::memory_order_relaxed));
        } catch (...) {}

        // Keep the 3-second maintenance cadence while allowing bounded shutdown
        // to interrupt the wait promptly.
        for (int slice = 0; slice < 60 && bgRunning.load(); ++slice) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

static void indexBatcherLoop() {
    while (g_idxBatcherRunning.load(std::memory_order_relaxed)) {
        std::vector<IndexPendingEntry> pending;
        {
            std::unique_lock<std::mutex> lk(g_idxPendingMutex);
            // Raft applies serially and waits for index completion. Sleeping for
            // every entry caps catch-up at 200 entries/s before storage work.
            // Keep coalescing asynchronous writes, but wake for a waiting apply.
            g_idxPendingCv.wait_for(lk, std::chrono::milliseconds(5), [] {
                return g_idxCompletionRequested || !g_idxBatcherRunning.load();
            });
            g_idxCompletionRequested = false;
            pending.swap(g_idxPendingQueue);
            for (const auto& entry : pending) {
                g_idxCollectionsProcessing.insert(colKey(entry.userId, entry.dbName, entry.collection));
            }
        }
        if (pending.empty()) continue;

        // Group by collection key so we do ONE exclusive lock + ONE applyColumnIndexBatch per collection.
        std::unordered_map<std::string, IndexPendingEntry> byCol;
        for (auto& e : pending) {
            const std::string key = colKey(e.userId, e.dbName, e.collection);
            auto it = byCol.find(key);
            if (it == byCol.end()) {
                byCol.emplace(key, std::move(e));
            } else {
                auto& merged = it->second.docs;
                merged.insert(merged.end(),
                    std::make_move_iterator(e.docs.begin()),
                    std::make_move_iterator(e.docs.end()));
            }
        }

        for (auto& [key, entry] : byCol) {
            bool applied = false;
            std::string failure;
            try {
                fs::path idxDir = LSM::collectionArtifactPath(
                    entry.userId, entry.dbName, entry.collection, ".idx");
                // Single exclusive lock acquisition per collection per 5ms batch cycle.
                std::unique_lock<std::shared_mutex> lk(getColIdxMutex(key));
                applyColumnIndexBatch(idxDir, entry.docs);
                applied = true;
            } catch (const std::exception& error) {
                failure = error.what();
            } catch (...) {
                failure = "unknown error";
            }
            {
                std::lock_guard<std::mutex> pendingLock(g_idxPendingMutex);
                auto it = g_idxPendingByCollection.find(key);
                if (applied && it != g_idxPendingByCollection.end()) {
                    it->second = it->second > entry.docs.size() ? it->second - entry.docs.size() : 0;
                    if (it->second == 0) g_idxPendingByCollection.erase(it);
                } else if (!applied && it != g_idxPendingByCollection.end()) {
                    g_idxPendingQueue.insert(g_idxPendingQueue.begin(), std::move(entry));
                }
                g_idxCollectionsProcessing.erase(key);
            }
            if (!applied) {
                std::cerr << "[LSM][COLUMN_INDEX] batch failed; retrying collection="
                          << key << " error=" << failure << std::endl;
            }
            g_idxPendingCv.notify_all();
        }
    }
}

bool LSM::dropCollection(const std::string& userId,
                         const std::string& dbName,
                         const std::string& collection) {
    const std::string key = colKey(userId, dbName, collection);

    // Remove work that has not started. A batch already executing is tracked
    // separately and must finish before files are removed, otherwise it could
    // recreate an index after the replicated drop has applied.
    {
        std::lock_guard<std::mutex> lock(g_flushQueueMutex);
        g_flushQueue.erase(std::remove_if(g_flushQueue.begin(), g_flushQueue.end(),
            [&](const FlushTask& task) { return task.key == key; }), g_flushQueue.end());
        g_flushQueuedKeys.erase(key);
    }
    {
        std::unique_lock<std::mutex> lock(g_idxPendingMutex);
        g_idxPendingQueue.erase(std::remove_if(g_idxPendingQueue.begin(), g_idxPendingQueue.end(),
            [&](const IndexPendingEntry& entry) {
                return colKey(entry.userId, entry.dbName, entry.collection) == key;
            }), g_idxPendingQueue.end());
        g_idxPendingByCollection.erase(key);
        if (!g_idxPendingCv.wait_for(lock, std::chrono::seconds(10), [&]() {
                return g_idxCollectionsProcessing.find(key) == g_idxCollectionsProcessing.end();
            })) {
            std::cerr << "[LSM][DROP] timed out waiting for index maintenance: " << key << std::endl;
            return false;
        }
    }

    // A running flush owns only the collection lock while it snapshots and
    // writes. Wait for it to leave, then acquire both destructive locks so no
    // stale in-memory or index state survives this apply.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true) {
        bool flushing = false;
        {
            std::lock_guard<std::mutex> lock(g_flushStateMutex);
            flushing = g_collectionsFlushing.find(key) != g_collectionsFlushing.end();
        }
        if (!flushing) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[LSM][DROP] timed out waiting for flush: " << key << std::endl;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto collectionLock = pacificdb::timing::makeTimedUniqueLock(
        getCollectionMutex(key), "pacificdb_lock_collection_drop");
    std::unique_lock<std::shared_mutex> indexLock(getColIdxMutex(key));
    memtables.erase(key);
    immutableMemtables.erase(key);
    clearIdIndexForCollection(key);
    invalidateSessionFloor(key);

    const fs::path dbRoot = LSM::databasePath(userId, dbName);
    const fs::path idxDir = dbRoot / (collection + ".idx");
    {
        std::lock_guard<std::mutex> cacheLock(g_colIdxCacheMutex);
        const std::string prefix = idxDir.string();
        for (auto it = g_colIdxCache.begin(); it != g_colIdxCache.end();) {
            if (it->first.rfind(prefix, 0) == 0) {
                g_colIdxCacheBytes.fetch_sub(it->second.bytes, std::memory_order_relaxed);
                it = g_colIdxCache.erase(it);
            } else {
                ++it;
            }
        }
    }
    {
        std::lock_guard<std::mutex> cacheLock(g_binaryColIdxCacheMutex);
        const std::string prefix = idxDir.string();
        for (auto it = g_binaryColIdxCache.begin(); it != g_binaryColIdxCache.end();) {
            if (it->first.rfind(prefix, 0) == 0) it = g_binaryColIdxCache.erase(it);
            else ++it;
        }
    }

    std::error_code ec;
    fs::remove_all(dbRoot / (collection + ".lsm"), ec);
    if (ec) return false;
    fs::remove_all(idxDir, ec);
    if (ec) return false;
    fs::remove(dbRoot / "wal" / (collection + ".wal"), ec);
    return !ec;
}

void LSM::startBackgroundTasks() {
    if (bgRunning.exchange(true)) return;
    bgThread = std::thread(maintenanceLoop);
    const size_t workers = flushThreadCount();
    g_flushWorkers.clear();
    g_flushWorkers.reserve(workers);
    for (size_t i = 0; i < workers; ++i) {
        g_flushWorkers.emplace_back(flushWorkerLoop, i);
    }
    // v3.0 perf: start centralized column index batcher thread
    g_idxBatcherRunning.store(true);
    g_idxBatcherThread = std::thread(indexBatcherLoop);
    std::cout << "[LSM] Background maintenance started" << std::endl;
    std::cout << "[LSM] flush_threads=" << workers
              << " max_concurrent_flushes=" << maxConcurrentFlushes() << std::endl;
}

void LSM::stopBackgroundTasks() {
    bgRunning.store(false);
    g_idxBatcherRunning.store(false);
    g_idxPendingCv.notify_all();
    g_flushQueueCv.notify_all();
    if (bgThread.joinable()) bgThread.join();
    for (auto& worker : g_flushWorkers) {
        if (worker.joinable()) worker.join();
    }
    g_flushWorkers.clear();
    if (g_idxBatcherThread.joinable()) g_idxBatcherThread.join();
    std::cout << "[LSM] Background maintenance stopping" << std::endl;
}

void LSM::forceFlush() {
    std::cout << "[LSM] Force flushing all memtables..." << std::endl;

    int flushedCount = 0;
    std::set<std::string> collectionKeys;

    // Collect all collections that need flushing under global shared lock.
    // Take the global lock only for the snapshot, then release immediately.
    {
        auto lk = pacificdb::timing::makeTimedSharedLock(lsm_global_mutex);
        for (auto& [key, table] : memtables) {
            if (!table.empty()) collectionKeys.insert(key);
        }
    }
    {
        std::lock_guard<std::mutex> flushLock(g_flushStateMutex);
        collectionKeys.insert(g_collectionsFlushing.begin(), g_collectionsFlushing.end());
    }

    // A snapshot must include immutable memtables owned by an already-running
    // background flush. Drain each collection completely instead of treating
    // "flush already running" as success.
    for (const auto& key : collectionKeys) {
        std::string userId;
        std::string dbName;
        std::string collection;
        if (!parseColKey(key, userId, dbName, collection)) continue;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool drained = false;
        while (std::chrono::steady_clock::now() < deadline) {
            LSM::flush(userId, dbName, collection);
            bool active = false;
            {
                std::lock_guard<std::mutex> flushLock(g_flushStateMutex);
                active = g_collectionsFlushing.find(key) != g_collectionsFlushing.end();
            }
            bool pending = false;
            {
                auto lk = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(key));
                auto memIt = memtables.find(key);
                auto immIt = immutableMemtables.find(key);
                pending = (memIt != memtables.end() && !memIt->second.empty())
                    || (immIt != immutableMemtables.end() && !immIt->second.empty());
            }
            if (!active && !pending) {
                drained = true;
                flushedCount++;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!drained) {
            throw std::runtime_error("snapshot flush drain timeout for " + key);
        }
    }

    std::cout << "[LSM] Force flush completed - " << flushedCount << " memtables flushed" << std::endl;
}

nlohmann::json LSM::getRuntimeStats() {
    auto lk = pacificdb::timing::makeTimedSharedLock(lsm_global_mutex);
    nlohmann::json collections = nlohmann::json::array();
    size_t totalEntries = 0;
    size_t activeBytes = 0;
    size_t activeMemtables = 0;
    size_t flushInProgress = 0;

    {
        std::lock_guard<std::mutex> flushLock(g_flushStateMutex);
        flushInProgress = g_collectionsFlushing.size();
    }

    for (const auto& [key, table] : memtables) {
        const size_t entries = table.size();
        if (entries == 0) continue;
        activeMemtables++;
        totalEntries += entries;
        activeBytes += approximateMemtableBytes(table);

        nlohmann::json item;
        item["key"] = key;
        item["entries"] = entries;

        std::string userId;
        std::string dbName;
        std::string collection;
        if (parseColKey(key, userId, dbName, collection)) {
            item["user_id"] = userId;
            item["database"] = dbName;
            item["collection"] = collection;
        }

        collections.push_back(item);
    }

    const size_t immutableCount = immutableMemtableCount();
    const size_t immutableEntries = immutableMemtableEntries();
    const size_t immutableBytes = immutableMemtableBytes();

    return {
        {"success", true},
        {"active_memtables", activeMemtables},
        {"total_entries", totalEntries},
        {"active_memtable_bytes", activeBytes},
        {"memtable_limit", MEMTABLE_LIMIT},
        {"memtable_hard_limit", memtableHardLimit()},
        {"flush_in_progress", flushInProgress},
        {"flush_queue_depth", flushQueueDepth()},
        {"flush_worker_count", g_flushWorkers.size()},
        {"flush_wait_ms", g_lastFlushWaitUs.load(std::memory_order_relaxed) / 1000.0},
        {"immutable_memtable_count", immutableCount},
        {"immutable_memtables", immutableCount},
        {"immutable_entries", immutableEntries},
        {"immutable_bytes", immutableBytes},
        {"flush_swap_count", g_flushSwapCount.load(std::memory_order_relaxed)},
        {"flush_swap_micros_total", g_flushSwapUsTotal.load(std::memory_order_relaxed)},
        {"flush_write_count", g_flushWriteCount.load(std::memory_order_relaxed)},
        {"flush_write_micros_total", g_flushWriteUsTotal.load(std::memory_order_relaxed)},
        {"flush_queued_total", g_flushQueuedTotal.load(std::memory_order_relaxed)},
        {"flush_completed_total", g_flushCompletedTotal.load(std::memory_order_relaxed)},
        {"flush_skipped_total", g_flushSkippedTotal.load(std::memory_order_relaxed)},
        {"write_stalls_total", g_writeStallsTotal.load(std::memory_order_relaxed)},
        {"write_stall_wait_ms", g_writeStallWaitUs.load(std::memory_order_relaxed) / 1000.0},
        {"compaction_queue_depth", 0},
        {"index_hits", g_columnIndexHits.load(std::memory_order_relaxed)},
        {"index_authoritative_empty", g_columnIndexAuthoritativeEmpty.load(std::memory_order_relaxed)},
        {"index_fallback_scans", g_columnIndexFallbackScans.load(std::memory_order_relaxed)},
        {"index_stale_reads", g_columnIndexStaleReads.load(std::memory_order_relaxed)},
        {"strong_id_cache_shadow_matches", g_strongIdCacheShadowMatches.load(std::memory_order_relaxed)},
        {"strong_id_cache_shadow_mismatches", g_strongIdCacheShadowMismatches.load(std::memory_order_relaxed)},
        {"strong_id_cache_shadow_misses", g_strongIdCacheShadowMisses.load(std::memory_order_relaxed)},
        {"id_index_entries_total", idIndexEntryCount()},
        {"id_index_collection_limit", ID_INDEX_MAX_PER_COLLECTION},
        {"id_index_per_stripe_collection_limit", idIndexMaxPerStripePerCollection()},
        {"index_single_pass_missing_sidecar", g_columnIndexSinglePassMissingSidecar.load(std::memory_order_relaxed)},
        {"sst_sidecars_repaired", g_sstSidecarsRepaired.load(std::memory_order_relaxed)},
        {"sst_sidecar_repair_failures", g_sstSidecarRepairFailures.load(std::memory_order_relaxed)},
        // v3.0R-G parsed column-index cache metrics
        {"column_index_cache_hit", g_colIdxCacheHits.load(std::memory_order_relaxed)},
        {"column_index_cache_miss", g_colIdxCacheMisses.load(std::memory_order_relaxed)},
        {"column_index_cache_eviction", g_colIdxCacheEvictions.load(std::memory_order_relaxed)},
        {"column_index_cache_bytes", g_colIdxCacheBytes.load(std::memory_order_relaxed)},
        {"column_index_parse_ms_p99", colIdxTimingP99(g_colIdxParseMs)},
        {"index_lookup_ms_p99", colIdxTimingP99(g_colIdxLookupMs)},
        {"column_index_segments_written", g_binaryColIdxSegmentsWritten.load(std::memory_order_relaxed)},
        {"column_index_segment_compactions", g_binaryColIdxCompactions.load(std::memory_order_relaxed)},
        {"column_index_bytes_written", g_binaryColIdxBytesWritten.load(std::memory_order_relaxed)},
        {"collections", collections}
    };
}

// v4.4H: Dedicated LSM profiling metrics endpoint (Phase 5)
nlohmann::json LSM::getLsmMetrics() {
    uint64_t scansTotal = g_sstScansPerReadTotal.load(std::memory_order_relaxed);
    uint64_t scansCount = g_sstScansPerReadCount.load(std::memory_order_relaxed);
    const uint64_t ingestedBytes = g_bytesIngested.load(std::memory_order_relaxed);
    const uint64_t compactedBytes = g_compactionBytesWritten.load(std::memory_order_relaxed);

    // Count total SST files across all collections
    size_t totalSstFiles = 0;
    size_t totalLsmCollections = 0;
    std::error_code ecTop;
    if (fs::exists(LSM_ROOT, ecTop)) {
        for (auto& u : fs::directory_iterator(LSM_ROOT, ecTop)) {
            if (!u.is_directory()) continue;
            for (auto& db : fs::directory_iterator(u.path(), ecTop)) {
                if (!db.is_directory()) continue;
                for (auto& c : fs::directory_iterator(db.path(), ecTop)) {
                    if (!c.is_directory()) continue;
                    std::string cname = c.path().filename().string();
                    if (cname.size() > 4 && cname.substr(cname.size()-4) == ".lsm") {
                        ++totalLsmCollections;
                        totalSstFiles += publishedSstsForCollection(c.path()).size();
                    }
                }
            }
        }
    }

    // Build P95/P99 for SST scans per read
    double sstP95 = 0.0, sstP99 = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_sstScanHistMutex);
        if (!g_sstScanHistogram.empty()) {
            sstP95 = histPercentile(g_sstScanHistogram, 95.0);
            sstP99 = histPercentile(g_sstScanHistogram, 99.0);
        }
    }

    // Build P95/P99 for read execute time
    double readExecP95 = 0.0, readExecP99 = 0.0;
    uint64_t readExecCount = g_readExecuteCount.load(std::memory_order_relaxed);
    uint64_t readExecTotal = g_readExecuteMsTotal.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_readExecHistMutex);
        if (!g_readExecHistogramMs.empty()) {
            readExecP95 = histPercentile(g_readExecHistogramMs, 95.0);
            readExecP99 = histPercentile(g_readExecHistogramMs, 99.0);
        }
    }

    double flushSwapP95 = 0.0, flushSwapP99 = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_flushSwapHistMutex);
        if (!g_flushSwapHistogramUs.empty()) {
            flushSwapP95 = histPercentile(g_flushSwapHistogramUs, 95.0);
            flushSwapP99 = histPercentile(g_flushSwapHistogramUs, 99.0);
        }
    }

    double flushWriteP95 = 0.0, flushWriteP99 = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_flushWriteHistMutex);
        if (!g_flushWriteHistogramUs.empty()) {
            flushWriteP95 = histPercentile(g_flushWriteHistogramUs, 95.0);
            flushWriteP99 = histPercentile(g_flushWriteHistogramUs, 99.0);
        }
    }

    const uint64_t flushSwapCount = g_flushSwapCount.load(std::memory_order_relaxed);
    const uint64_t flushWriteCount = g_flushWriteCount.load(std::memory_order_relaxed);

    return {
        {"version", "v5.5P-R4.2"},
        // Compaction metrics (Phase 5 + 7)
        {"compaction_active", g_compactionActiveCount.load(std::memory_order_relaxed)},
        {"compaction_sweeps_total", g_compactionSweepsTotal.load(std::memory_order_relaxed)},
        {"compaction_runs_total", g_compactionRunsTotal.load(std::memory_order_relaxed)},
        {"compaction_throttled_total", g_compactionThrottledTotal.load(std::memory_order_relaxed)},
        {"compaction_yield_ms_total", g_compactionYieldMsTotal.load(std::memory_order_relaxed)},
        {"compaction_bytes_written", g_compactionBytesWritten.load(std::memory_order_relaxed)},
        {"compaction_bytes_read", g_compactionBytesRead.load(std::memory_order_relaxed)},
        {"bytes_ingested", ingestedBytes},
        {"compaction_write_amplification", ingestedBytes == 0 ? 0.0 :
            (double)compactedBytes / (double)ingestedBytes},
        {"compaction_tombstones_observed", g_compactionTombstonesObserved.load(std::memory_order_relaxed)},
        {"compaction_tombstones_reclaimed", g_compactionTombstonesReclaimed.load(std::memory_order_relaxed)},
        {"compaction_reclaimed_bytes", g_compactionReclaimedBytes.load(std::memory_order_relaxed)},
        {"compaction_read_queue_threshold", compactionReadQueueThreshold()},
        // SST / L0 metrics (Phase 5 + 8)
        {"total_sst_file_count", totalSstFiles},
        {"l0_file_count", totalSstFiles},  // flat LSM: all SSTs are L0-equivalent
        {"lsm_collections_total", totalLsmCollections},
        // Flush metrics (Phase 5 + 9)
        {"flush_active", g_flushActiveCount.load(std::memory_order_relaxed)},
        {"flush_queue_depth", flushQueueDepth()},
        {"flush_queued_total", g_flushQueuedTotal.load(std::memory_order_relaxed)},
        {"flush_completed_total", g_flushCompletedTotal.load(std::memory_order_relaxed)},
        {"flush_bytes_pending", 0},  // tracked via memtable size
        {"flush_bytes_written", g_flushBytesWritten.load(std::memory_order_relaxed)},
        {"flush_wait_ms", g_lastFlushWaitUs.load(std::memory_order_relaxed) / 1000.0},
        {"flush_swap_count", flushSwapCount},
        {"flush_swap_us_avg", flushSwapCount > 0 ? (double)g_flushSwapUsTotal.load(std::memory_order_relaxed) / (double)flushSwapCount : 0.0},
        {"flush_swap_us_p95", flushSwapP95},
        {"flush_swap_us_p99", flushSwapP99},
        {"flush_write_count", flushWriteCount},
        {"flush_write_us_avg", flushWriteCount > 0 ? (double)g_flushWriteUsTotal.load(std::memory_order_relaxed) / (double)flushWriteCount : 0.0},
        {"flush_write_us_p95", flushWriteP95},
        {"flush_write_us_p99", flushWriteP99},
        {"immutable_memtables", immutableMemtableCount()},
        {"immutable_entries", immutableMemtableEntries()},
        {"immutable_bytes", immutableMemtableBytes()},
        // Write stall (Phase 5 + 6)
        {"write_stall_active", g_writeStallsTotal.load(std::memory_order_relaxed) > 0 ? 1 : 0},
        {"write_stalls_total", g_writeStallsTotal.load(std::memory_order_relaxed)},
        {"write_stall_duration_ms", g_writeStallWaitUs.load(std::memory_order_relaxed) / 1000.0},
        // Read metrics (Phase 5 + 6)
        {"read_execute_count", readExecCount},
        {"read_execute_ms_total", readExecTotal},
        {"read_execute_ms_avg", readExecCount > 0 ? (double)readExecTotal / (double)readExecCount : 0.0},
        {"read_execute_ms_p95", readExecP95},
        {"read_execute_ms_p99", readExecP99},
        {"read_memtable_hits", g_readMemtableHits.load(std::memory_order_relaxed)},
        {"read_active_hits", g_readMemtableHits.load(std::memory_order_relaxed)},
        {"read_immutable_hits", g_readImmutableHits.load(std::memory_order_relaxed)},
        {"read_sst_hits", g_readSstHits.load(std::memory_order_relaxed)},
        {"read_queue_depth", currentReadQueueDepth()},
        // SST read amplification (Phase 5 + 8)
        {"read_amplification_avg_sst", scansCount > 0 ? (double)scansTotal / (double)scansCount : 0.0},
        {"sst_files_scanned_per_read_p95", sstP95},
        {"sst_files_scanned_per_read_p99", sstP99},
        {"sst_scans_total", scansTotal},
        {"sst_scans_samples", scansCount},
        // Bloom filter (Phase 8)
        {"bloom_filter_hit", g_bloomFilterHits.load(std::memory_order_relaxed)},
        {"bloom_filter_miss", g_bloomFilterMisses.load(std::memory_order_relaxed)},
        {"bloom_cache_hit", g_bloomCacheHits.load(std::memory_order_relaxed)},
        {"bloom_cache_miss", g_bloomCacheMisses.load(std::memory_order_relaxed)},
        {"bloom_filter_hit_rate", (g_bloomFilterHits + g_bloomFilterMisses) > 0 ?
            (double)g_bloomFilterHits.load() / (double)(g_bloomFilterHits.load() + g_bloomFilterMisses.load()) : 0.0},
        // Column index cache (pre-existing)
        {"col_idx_cache_hits", g_colIdxCacheHits.load(std::memory_order_relaxed)},
        {"col_idx_cache_misses", g_colIdxCacheMisses.load(std::memory_order_relaxed)},
        {"col_idx_cache_bytes", g_colIdxCacheBytes.load(std::memory_order_relaxed)},
        {"col_idx_cache_hit_rate", (g_colIdxCacheHits + g_colIdxCacheMisses) > 0 ?
            (double)g_colIdxCacheHits.load() / (double)(g_colIdxCacheHits.load() + g_colIdxCacheMisses.load()) : 0.0},
        {"col_idx_parse_p99_ms", colIdxTimingP99(g_colIdxParseMs)},
        {"col_idx_lookup_p99_ms", colIdxTimingP99(g_colIdxLookupMs)},
        {"col_idx_segments_written", g_binaryColIdxSegmentsWritten.load(std::memory_order_relaxed)},
        {"col_idx_segment_compactions", g_binaryColIdxCompactions.load(std::memory_order_relaxed)},
        {"col_idx_bytes_written", g_binaryColIdxBytesWritten.load(std::memory_order_relaxed)},
    };
}

// Export LSM snapshot as a JSON payload. This is a best-effort export of SST files
// (text-based .sst) and small metadata so followers can restore DB files atomically.
// ══════════ V11.4-IDX-001: snapshot index recovery state ══════════
namespace {

struct DeclaredIndex {
    std::string userId, dbName, collection;
    nlohmann::json catalog;
};

std::mutex g_idxRecoveryMutex;
LSM::IndexRecoveryState g_idxRecoveryState = LSM::IndexRecoveryState::NONE;
std::vector<DeclaredIndex> g_idxRecoveryDeclared;
std::string g_idxRecoveryError;
std::string g_idxRecoveryCurrent;
uint64_t g_idxRecoverySnapshotIndex = 0;
size_t g_idxRecoveryDeclaredCount = 0;
size_t g_idxRecoveryCompleted = 0;
size_t g_idxRecoveryFailed = 0;
uint64_t g_idxRecoveryDocsScanned = 0;
uint64_t g_idxRecoveryEntriesWritten = 0;
uint64_t g_idxRecoveryStartMs = 0;
uint64_t g_idxRecoveryProgressMs = 0;
bool g_idxRecoveryReadinessPublished = true;

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void setIndexRecoveryState(LSM::IndexRecoveryState s, const std::string& error = {}) {
    std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
    g_idxRecoveryState = s;
    if (s != LSM::IndexRecoveryState::NONE
        && s != LSM::IndexRecoveryState::INDEX_REBUILD_COMPLETE) {
        g_idxRecoveryReadinessPublished = false;
    }
    g_idxRecoveryProgressMs = nowMs();
    if (!error.empty()) g_idxRecoveryError = error;
    std::cout << "[LSM][IDXRECOVERY] state=" << LSM::indexRecoveryStateName(s)
              << (error.empty() ? "" : (" error=" + error)) << std::endl;
}

void publishIndexRecoveryReadiness() {
    std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
    if (g_idxRecoveryState != LSM::IndexRecoveryState::INDEX_REBUILD_COMPLETE) {
        throw std::runtime_error("cannot publish index readiness before rebuild completion");
    }
    g_idxRecoveryReadinessPublished = true;
    g_idxRecoveryProgressMs = nowMs();
    std::cout << "[LSM][IDXRECOVERY] readiness=published" << std::endl;
}

bool scheduleReplayedIndexRecovery(
    const std::set<std::tuple<std::string, std::string, std::string>>& collections) {
    std::vector<DeclaredIndex> declared;
    for (const auto& [userId, database, collection] : collections) {
        if (userId.empty() || database.empty() || collection.empty()) continue;
        const auto catalog = loadIndexCatalog(
            LSM::collectionArtifactPath(userId, database, collection, ".idx"));
        if (catalog.contains("indexes") && catalog["indexes"].is_array() &&
            !catalog["indexes"].empty()) {
            declared.push_back({userId, database, collection, catalog});
        }
    }
    if (declared.empty()) return true;
    {
        std::lock_guard<std::mutex> lock(g_idxRecoveryMutex);
        g_idxRecoveryDeclared = declared;
        g_idxRecoveryDeclaredCount = declared.size();
        g_idxRecoveryError.clear();
    }
    setIndexRecoveryState(LSM::IndexRecoveryState::INDEX_REBUILD_REQUIRED);
    return LSM::runPendingIndexRebuild();
}

std::string catalogChecksumOf(const nlohmann::json& catalog) {
    return std::to_string(pacificdb::durability::ChecksumCalculator::crc32(
        catalog.dump()));
}

// fsync a file or directory path; returns false on failure.
bool fsyncPath(const fs::path& p, bool directory) {
#ifndef _WIN32
    const int fd = ::open(p.c_str(), directory ? (O_RDONLY | O_DIRECTORY) : O_RDONLY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#else
    (void)p; (void)directory;
    return true;
#endif
}

}  // namespace

const char* LSM::indexRecoveryStateName(IndexRecoveryState state) {
    switch (state) {
        case IndexRecoveryState::NONE: return "NONE";
        case IndexRecoveryState::SNAPSHOT_BASE_RESTORING: return "SNAPSHOT_BASE_RESTORING";
        case IndexRecoveryState::SNAPSHOT_BASE_RESTORED: return "SNAPSHOT_BASE_RESTORED";
        case IndexRecoveryState::INDEX_CATALOG_RESTORED: return "INDEX_CATALOG_RESTORED";
        case IndexRecoveryState::INDEX_REBUILD_REQUIRED: return "INDEX_REBUILD_REQUIRED";
        case IndexRecoveryState::INDEX_REBUILDING: return "INDEX_REBUILDING";
        case IndexRecoveryState::INDEX_REBUILD_VERIFYING: return "INDEX_REBUILD_VERIFYING";
        case IndexRecoveryState::INDEX_REBUILD_COMPLETE: return "INDEX_REBUILD_COMPLETE";
        case IndexRecoveryState::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

LSM::IndexRecoveryState LSM::indexRecoveryState() {
    std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
    return g_idxRecoveryState;
}

bool LSM::indexRecoverySettled() {
    std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
    return g_idxRecoveryState == IndexRecoveryState::NONE
        || (g_idxRecoveryState == IndexRecoveryState::INDEX_REBUILD_COMPLETE
            && g_idxRecoveryReadinessPublished);
}

nlohmann::json LSM::indexRecoveryStatus() {
    std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
    return nlohmann::json{
        {"indexRecoveryState", indexRecoveryStateName(g_idxRecoveryState)},
        {"snapshotIndex", g_idxRecoverySnapshotIndex},
        {"declaredIndexCount", g_idxRecoveryDeclaredCount},
        {"rebuildingIndexCount", g_idxRecoveryState == IndexRecoveryState::INDEX_REBUILDING ? 1 : 0},
        {"completedIndexCount", g_idxRecoveryCompleted},
        {"failedIndexCount", g_idxRecoveryFailed},
        {"indexRebuildDocumentsScanned", g_idxRecoveryDocsScanned},
        {"indexRebuildEntriesWritten", g_idxRecoveryEntriesWritten},
        {"currentIndex", g_idxRecoveryCurrent},
        {"rebuildStartTime", g_idxRecoveryStartMs},
        {"rebuildLastProgressTime", g_idxRecoveryProgressMs},
        {"rebuildError", g_idxRecoveryError},
        {"readinessPublished", g_idxRecoveryReadinessPublished},
        {"settled", g_idxRecoveryState == IndexRecoveryState::NONE
                    || (g_idxRecoveryState == IndexRecoveryState::INDEX_REBUILD_COMPLETE
                        && g_idxRecoveryReadinessPublished)},
    };
}

// Stream the latest physical row for each id into bounded, verified index segments.
// Recovery runs before normal visibility, so commit-index-filtered readers are invalid here.
bool LSM::runPendingIndexRebuild() {
    std::vector<DeclaredIndex> declared;
    {
        std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
        if (g_idxRecoveryState != IndexRecoveryState::INDEX_REBUILD_REQUIRED) return true;
        declared = g_idxRecoveryDeclared;
        g_idxRecoveryStartMs = nowMs();
        g_idxRecoveryCompleted = 0;
        g_idxRecoveryFailed = 0;
        g_idxRecoveryDocsScanned = 0;
        g_idxRecoveryEntriesWritten = 0;
    }
    setIndexRecoveryState(IndexRecoveryState::INDEX_REBUILDING);

    const std::string rebuildId = std::to_string(nowMs());
    for (const auto& d : declared) {
        try {
            const fs::path idxDir =
                LSM::collectionArtifactPath(d.userId, d.dbName, d.collection, ".idx");
            const fs::path lsmDir =
                LSM::collectionArtifactPath(d.userId, d.dbName, d.collection, ".lsm");
            const fs::path stageIdx = LSM::requireContained(
                fs::path(LSM::rootOrThrow()) / ".idx-rebuild" / rebuildId /
                d.userId / d.dbName / (d.collection + ".idx"));
            createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), stageIdx);
            const auto ssts = publishedSstsForCollection(lsmDir);
            const size_t batchBytes = envSizeT("INDEX_REBUILD_BATCH_BYTES", 8ULL * 1024 * 1024);
            json stagedCatalog = d.catalog;
            size_t documentsScanned = 0;
            size_t entriesWritten = 0;
            {
                std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
                g_idxRecoveryCurrent = d.userId + "/" + d.dbName + "/" + d.collection;
            }

            for (auto& def : stagedCatalog["indexes"]) {
                const std::string field = def.value("field", std::string());
                if (field.empty()) continue;
                validateStorageIdentifier(field, "indexFieldName");
                size_t fieldDocuments = 0;
                size_t fieldEntries = 0;
                std::string indexError;
                if (!pacificdb::storage_v2::visitLatestSstRows(
                        ssts, [&](const pacificdb::storage_v2::MsgpackRowView& row) {
                            ++fieldDocuments;
                            const auto id = row.field("id");
                            const auto value = row.field(field);
                            if (id && value && shouldIndexColumnField(field, *value)) ++fieldEntries;
                            return true;
                        }, &indexError)) {
                    throw std::runtime_error("cannot scan base SSTs for " + field + ": " + indexError);
                }
                documentsScanned = std::max(documentsScanned, fieldDocuments);
                const size_t q25 = fieldEntries == 0 ? 0 : (fieldEntries + 3) / 4;
                const size_t q50 = fieldEntries == 0 ? 0 : (fieldEntries + 1) / 2;
                const size_t q75 = fieldEntries == 0 ? 0 : (3 * fieldEntries + 3) / 4;
                const fs::path stageDirectory = indexFieldSegmentDirectory(stageIdx, field);
                std::vector<pacificdb::storage_v2::IndexMutation> chunk;
                size_t chunkBytes = 0;
                size_t fieldWritten = 0;
                bool snapshot = true;
                auto flushChunk = [&]() {
                    std::sort(chunk.begin(), chunk.end(), [](const auto& left, const auto& right) {
                        if (left.value != right.value) return left.value < right.value;
                        if (left.id != right.id) return left.id < right.id;
                        return static_cast<unsigned>(left.op) < static_cast<unsigned>(right.op);
                    });
                    fs::path writtenPath;
                    std::string writeError;
                    if (!pacificdb::storage_v2::writeIndexSegment(
                            stageDirectory, chunk, snapshot, &writtenPath, &writeError)) {
                        throw std::runtime_error("cannot write staged index " + field + ": " + writeError);
                    }
                    setIndexRecoveryState(IndexRecoveryState::INDEX_REBUILD_VERIFYING);
                    pacificdb::storage_v2::IndexSegment readBack;
                    if (!pacificdb::storage_v2::readIndexSegment(writtenPath, readBack, &writeError) ||
                        readBack.snapshot != snapshot || readBack.mutations.size() != chunk.size()) {
                        throw std::runtime_error("staged index verification failed for " + field +
                                                 ": " + writeError);
                    }
                    for (size_t i = 0; i < chunk.size(); ++i) {
                        if (readBack.mutations[i].op != chunk[i].op ||
                            readBack.mutations[i].value != chunk[i].value ||
                            readBack.mutations[i].id != chunk[i].id) {
                            throw std::runtime_error("staged index verification mismatch for " + field);
                        }
                    }
                    snapshot = false;
                    chunk.clear();
                    chunkBytes = 0;
                    pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_INDEX_FILE_FSYNC", 1);
                    pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_DATA_FSYNC", 1);
                };
                setIndexRecoveryState(IndexRecoveryState::INDEX_REBUILDING);
                if (!pacificdb::storage_v2::visitLatestSstRows(
                        ssts, [&](const pacificdb::storage_v2::MsgpackRowView& row) {
                            const auto idValue = row.field("id");
                            const auto value = row.field(field);
                            if (!idValue || !value || !shouldIndexColumnField(field, *value)) return true;
                            const std::string id = idValue->is_string()
                                ? idValue->get<std::string>() : idValue->dump();
                            if (id.empty()) return true;
                            if (fieldWritten == 0) {
                                pacificdb::test::hitFailpoint("FP_IDX_REBUILD_BEFORE_FIRST_ENTRY", 1);
                            }
                            const std::string encoded = columnIndexValue(*value);
                            const size_t encodedBytes = 16 + id.size() + encoded.size();
                            if (!chunk.empty() && chunkBytes + encodedBytes > batchBytes) flushChunk();
                            chunk.push_back({pacificdb::storage_v2::IndexMutation::Op::Put,
                                             encoded, id});
                            chunkBytes += encodedBytes;
                            ++fieldWritten;
                            if (fieldWritten == 1) pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_FIRST_ENTRY", 1);
                            if (fieldWritten == q25) pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_25_PERCENT", 1);
                            if (fieldWritten == q50) pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_50_PERCENT", 1);
                            if (fieldWritten == q75) pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_75_PERCENT", 1);
                            if (fieldWritten == fieldEntries) pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_FINAL_ENTRY", 1);
                            return true;
                        }, &indexError)) {
                    throw std::runtime_error("cannot build staged index " + field + ": " + indexError);
                }
                if (!chunk.empty() || snapshot) flushChunk();
                if (fieldWritten != fieldEntries) {
                    throw std::runtime_error("base SST changed during index rebuild for " + field);
                }
                entriesWritten += fieldWritten;
                def["status"] = "ready";
                def["complete"] = true;
                def["documents"] = fieldDocuments;
                def["updatedAtMs"] = nowMs();
            }

            writeIndexCatalog(stageIdx, stagedCatalog);
            if (!fsyncPath(indexCatalogPath(stageIdx), false)) {
                throw std::runtime_error("cannot fsync staged index catalog");
            }
            const fs::path manifestPath = stageIdx / "_generation.json";
            {
                const json manifest = {
                    {"rebuildId", rebuildId},
                    {"userId", d.userId}, {"dbName", d.dbName}, {"collection", d.collection},
                    {"entriesWritten", entriesWritten},
                    {"documentsScanned", documentsScanned},
                    {"catalogChecksum", catalogChecksumOf(stagedCatalog)},
                    {"createdAtMs", nowMs()},
                };
                std::ofstream out(manifestPath.string(), std::ios::trunc);
                if (!out) throw std::runtime_error("cannot write generation manifest");
                out << manifest.dump(2);
                out.flush();
            }
            if (!fsyncPath(manifestPath, false)) {
                throw std::runtime_error("cannot fsync generation manifest");
            }
            pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_MANIFEST_FSYNC", 1);
            pacificdb::test::hitFailpoint("FP_IDX_REBUILD_BEFORE_PUBLISH", 1);

            const fs::path backup = LSM::requireContained(
                fs::path(idxDir.string() + ".rebuild-backup-" + rebuildId));
            std::unique_lock<std::shared_mutex> indexLock(
                getColIdxMutex(colKey(d.userId, d.dbName, d.collection)));
            std::error_code ec;
            fs::remove_all(backup, ec);
            const bool hadPrevious = fs::exists(idxDir);
            if (hadPrevious) {
                ec.clear();
                fs::rename(idxDir, backup, ec);
                if (ec) throw std::runtime_error("cannot preserve previous index generation: " + ec.message());
            }
            ec.clear();
            fs::rename(stageIdx, idxDir, ec);
            if (ec || !fsyncPath(idxDir.parent_path(), true)) {
                std::error_code rollbackError;
                if (!ec) fs::rename(idxDir, stageIdx, rollbackError);
                if (hadPrevious) fs::rename(backup, idxDir, rollbackError);
                throw std::runtime_error("cannot publish index generation: " + ec.message());
            }
            fs::remove_all(backup, ec);
            fsyncPath(idxDir.parent_path(), true);
            {
                std::lock_guard<std::mutex> cacheLock(g_binaryColIdxCacheMutex);
                for (auto it = g_binaryColIdxCache.begin(); it != g_binaryColIdxCache.end();) {
                    if (it->first.rfind(idxDir.string(), 0) == 0 ||
                        it->first.rfind(stageIdx.string(), 0) == 0) {
                        it = g_binaryColIdxCache.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_PUBLISH", 1);

            {
                std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
                g_idxRecoveryCompleted += 1;
                g_idxRecoveryDocsScanned += documentsScanned;
                g_idxRecoveryEntriesWritten += entriesWritten;
                g_idxRecoveryProgressMs = nowMs();
            }
            std::cout << "[LSM][IDXRECOVERY] rebuilt " << d.userId << "/" << d.dbName << "/"
                      << d.collection << " entries=" << entriesWritten
                      << " documents=" << documentsScanned << std::endl;
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
                g_idxRecoveryFailed += 1;
            }
            setIndexRecoveryState(IndexRecoveryState::FAILED, e.what());
            return false;
        }
    }

    pacificdb::test::hitFailpoint("FP_IDX_REBUILD_BEFORE_COMPLETE_MARKER", 1);
    setIndexRecoveryState(IndexRecoveryState::INDEX_REBUILD_COMPLETE);
    pacificdb::test::hitFailpoint("FP_IDX_REBUILD_AFTER_COMPLETE_MARKER", 1);
    pacificdb::test::hitFailpoint("FP_IDX_BEFORE_READINESS_PUBLICATION", 1);
    publishIndexRecoveryReadiness();
    return true;
}

std::filesystem::path LSM::pinSnapshotFiles(uint64_t lastIncludedIndex) {
    if (lastIncludedIndex == 0) {
        throw std::runtime_error("snapshot boundary must be non-zero");
    }

    // No apply may run while forceFlush drains the boundary. Once every SST is
    // hard-linked below, compaction may unlink the live name without changing
    // the pinned inode and apply can resume immediately.
    LSM::forceFlush();
    const fs::path liveRoot(LSM::rootOrThrow());
    static std::atomic<uint64_t> sequence{0};
    const fs::path pinnedRoot = LSM::requireContained(
        liveRoot / ".snapshot-pins" /
        (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
         + "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed))));

    auto pinFile = [&](const fs::path& source, const fs::path& relative, bool hardLink) {
        const fs::path target = LSM::requireContained(pinnedRoot / relative);
        createContainedStorageDirectories(liveRoot, target.parent_path());
        std::error_code ec;
        if (hardLink) {
            fs::create_hard_link(source, target, ec);
        } else {
            fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            throw std::runtime_error(
                "snapshot cannot pin " + source.string() + ": " + ec.message());
        }
    };

    try {
        createContainedStorageDirectories(liveRoot, pinnedRoot);
        for (const auto& userDir : fs::directory_iterator(liveRoot)) {
            const std::string userId = userDir.path().filename().string();
            if (!userDir.is_directory() || userId == "raft" || userId == "logs" ||
                userId == "backups" || userId == "restores" ||
                (!userId.empty() && userId.front() == '.')) continue;
            validateContainedStoragePath(liveRoot, userDir.path());
            validateStorageIdentifier(userId, "snapshotUserIdentifier");
            for (const auto& dbDir : fs::directory_iterator(userDir.path())) {
                if (!dbDir.is_directory()) continue;
                validateContainedStoragePath(liveRoot, dbDir.path());
                const std::string dbName = validateStorageIdentifier(
                    dbDir.path().filename().string(), "snapshotDatabaseIdentifier");
                for (const auto& entry : fs::directory_iterator(dbDir.path())) {
                    validateContainedStoragePath(liveRoot, entry.path());
                    const std::string name = entry.path().filename().string();
                    if (entry.is_regular_file() && name == "db.meta") {
                        pinFile(entry.path(), fs::relative(entry.path(), liveRoot), false);
                        continue;
                    }
                    if (!entry.is_directory() || name.size() <= 4) continue;
                    const std::string suffix = name.substr(name.size() - 4);
                    if (suffix != ".lsm" && suffix != ".idx") continue;
                    const std::string collection = validateStorageIdentifier(
                        name.substr(0, name.size() - 4), "snapshotCollectionIdentifier");
                    const std::string key = colKey(userId, dbName, collection);
                    if (suffix == ".lsm") {
                        auto lock = pacificdb::timing::makeTimedSharedLock(
                            getCollectionMutex(key));
                        for (const auto& sst : publishedSstsForCollection(entry.path())) {
                            validateContainedStoragePath(liveRoot, sst);
                            validateStorageIdentifier(
                                sst.filename().string(), "snapshotSstIdentifier");
                            pinFile(sst, fs::relative(sst, liveRoot), true);
                        }
                        const fs::path manifest = entry.path() / "_manifest.json";
                        if (fs::is_regular_file(manifest)) {
                            pinFile(manifest, fs::relative(manifest, liveRoot), false);
                        }
                    } else if (suffix == ".idx") {
                        const fs::path catalog = entry.path() / "_catalog.json";
                        if (fs::is_regular_file(catalog)) {
                            auto lock = pacificdb::timing::makeTimedSharedLock(
                                getColIdxMutex(key));
                            pinFile(catalog, fs::relative(catalog, liveRoot), false);
                        }
                    }
                }
            }
        }
        return pinnedRoot;
    } catch (...) {
        discardSnapshotFiles(pinnedRoot);
        throw;
    }
}

static fs::path requireSnapshotPinRoot(const fs::path& candidate) {
    const fs::path liveRoot = fs::path(LSM::rootOrThrow()).lexically_normal();
    const fs::path pinnedRoot = validateContainedStoragePath(
        liveRoot, candidate).lexically_normal();
    if (pinnedRoot.parent_path() != liveRoot / ".snapshot-pins" ||
        pinnedRoot.filename().empty()) {
        throw std::runtime_error("snapshot pin path is outside the pin namespace");
    }
    return pinnedRoot;
}

void LSM::discardSnapshotFiles(const std::filesystem::path& pinnedRoot) noexcept {
    if (pinnedRoot.empty()) return;
    try {
        std::error_code ec;
        fs::remove_all(requireSnapshotPinRoot(pinnedRoot), ec);
    } catch (...) {}
}

nlohmann::json LSM::exportSnapshotJson(uint64_t lastIncludedIndex) {
    const fs::path pinnedRoot = pinSnapshotFiles(lastIncludedIndex);
    try {
        nlohmann::json snapshot = exportSnapshotJson(lastIncludedIndex, pinnedRoot);
        discardSnapshotFiles(pinnedRoot);
        return snapshot;
    } catch (...) {
        discardSnapshotFiles(pinnedRoot);
        throw;
    }
}

nlohmann::json LSM::exportSnapshotJson(
    uint64_t lastIncludedIndex,
    const std::filesystem::path& pinnedRoot) {
    return exportSnapshotJson(lastIncludedIndex, pinnedRoot, nullptr);
}

nlohmann::json LSM::exportSnapshotBundleManifest(
    uint64_t lastIncludedIndex,
    const std::filesystem::path& pinnedRoot,
    std::vector<std::filesystem::path>& artifactSources) {
    artifactSources.clear();
    return exportSnapshotJson(lastIncludedIndex, pinnedRoot, &artifactSources);
}

nlohmann::json LSM::exportSnapshotJson(
    uint64_t lastIncludedIndex,
    const std::filesystem::path& pinnedRoot,
    std::vector<std::filesystem::path>* artifactSources) {
    nlohmann::json out;
    out["version"] = artifactSources ? 3 : 2;
    // Ensure snapshot does not claim visibility beyond quorum-committed index
    uint64_t raftCommit = 0;
    try {
        raftCommit = RaftCore::instance().getCommitIndex();
    } catch (...) { raftCommit = lastIncludedIndex; }
    if (lastIncludedIndex > raftCommit) lastIncludedIndex = raftCommit;
    out["lastIncludedIndex"] = lastIncludedIndex;
    out["snapshot_max_visible_commit_index"] = lastIncludedIndex;
    out["createdAt"] = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    // Annotate snapshot with commit/term/visibility floor so consumers can enforce fences
    try {
        out["snapshot_commit_index"] = lastIncludedIndex;
        out["snapshot_term"] = RaftCore::instance().getCurrentTerm();
        out["snapshot_visibility_floor"] = lastIncludedIndex;
    } catch (...) {
        out["snapshot_commit_index"] = lastIncludedIndex;
        out["snapshot_term"] = 0;
        out["snapshot_visibility_floor"] = lastIncludedIndex;
    }

    nlohmann::json files = nlohmann::json::array();
    try {
        if (LSM_ROOT.empty() || !fs::is_directory(pinnedRoot)) {
            throw std::runtime_error("snapshot pinned file set is unavailable");
        }
        const fs::path snapshotRoot = requireSnapshotPinRoot(pinnedRoot);
        for (auto& userDir : fs::directory_iterator(snapshotRoot)) {
            const std::string userId = userDir.path().filename().string();
            if (userId == "raft" || userId == "logs" || userId == "backups" ||
                userId == "restores" ||
                (!userId.empty() && userId.front() == '.')) {
                continue;
            }
            validateContainedStoragePath(snapshotRoot, userDir.path());
            if (!userDir.is_directory()) continue;
            validateStorageIdentifier(userId, "snapshotUserIdentifier");
            for (auto& dbDir : fs::directory_iterator(userDir)) {
                validateContainedStoragePath(snapshotRoot, dbDir.path());
                if (!dbDir.is_directory()) continue;
                const std::string dbName = validateStorageIdentifier(
                    dbDir.path().filename().string(),
                    "snapshotDatabaseIdentifier");
                for (auto& entry : fs::directory_iterator(dbDir)) {
                    validateContainedStoragePath(snapshotRoot, entry.path());
                    // capture .lsm directories (SST files) and db.meta
                    if (entry.is_directory()) {
                        std::string dirName = entry.path().filename().string();
                        if (dirName.size() > 4 && dirName.substr(dirName.size()-4) == ".lsm") {
                            const std::string collection = validateStorageIdentifier(
                                dirName.substr(0, dirName.size()-4),
                                "snapshotCollectionIdentifier");
                            for (const auto& sstPath : publishedSstsForCollection(entry.path())) {
                                validateContainedStoragePath(snapshotRoot, sstPath);
                                    validateStorageIdentifier(
                                        sstPath.filename().string(),
                                        "snapshotSstIdentifier");
                                    nlohmann::json f;
                                    // Restore under the same relative data path.
                                    std::string rel = std::string(std::filesystem::relative(sstPath, snapshotRoot).string());
                                    f["path"] = rel;
                                    bool copyBinarySst = pacificdb::storage_v2::isBinarySst(sstPath);
                                    std::string scanError;
                                    if (copyBinarySst && !scanSstRows(sstPath, [&](json&& doc) {
                                            if (!docVisibleByCommitIndex(doc, lastIncludedIndex)) {
                                                copyBinarySst = false;
                                            }
                                            return true;
                                        }, &scanError)) {
                                        throw std::runtime_error("snapshot cannot read " +
                                            sstPath.string() + ": " + scanError);
                                    }
                                    if (artifactSources && !copyBinarySst) {
                                        throw std::runtime_error(
                                            "snapshot boundary does not cover immutable SST " +
                                            sstPath.string());
                                    }
                                    if (artifactSources) {
                                        f["contentEncoding"] = "raw";
                                        artifactSources->push_back(sstPath);
                                    } else if (copyBinarySst) {
                                        std::ifstream input(sstPath, std::ios::binary);
                                        if (!input) {
                                            throw std::runtime_error(
                                                "snapshot cannot read " + sstPath.string());
                                        }
                                        const std::string bytes{
                                            std::istreambuf_iterator<char>(input),
                                            std::istreambuf_iterator<char>()};
                                        f["content"] = encodeBase64(bytes);
                                        f["contentEncoding"] = "pdb2-sst-base64";
                                    } else {
                                        json visibleRows = json::array();
                                        scanError.clear();
                                        if (!scanSstRows(sstPath, [&](json&& doc) {
                                                if (!docVisibleByCommitIndex(doc, lastIncludedIndex)) return true;
                                                visibleRows.push_back(std::move(doc));
                                                return true;
                                            }, &scanError)) {
                                            throw std::runtime_error("snapshot cannot read " +
                                                sstPath.string() + ": " + scanError);
                                        }
                                        const auto packed = json::to_msgpack(visibleRows);
                                        f["content"] = encodeBase64(std::string(
                                            reinterpret_cast<const char*>(packed.data()), packed.size()));
                                        f["contentEncoding"] = "msgpack-base64";
                                    }
                                    f["storageFormatVersion"] = 2;
                                    files.push_back(std::move(f));
                            }
                        }
                    } else {
                        // include db.meta files and small metadata files
                        if (entry.path().filename() == "db.meta") {
                            validateContainedStoragePath(snapshotRoot, entry.path());
                            nlohmann::json f;
                            std::string rel = std::string(std::filesystem::relative(entry.path(), snapshotRoot).string());
                            f["path"] = rel;
                            if (artifactSources) {
                                f["contentEncoding"] = "raw";
                                artifactSources->push_back(entry.path());
                            } else {
                                std::ifstream in(entry.path().string());
                                if (!in.is_open()) {
                                    throw std::runtime_error(
                                        "snapshot cannot read " + entry.path().string());
                                }
                                f["content"] = std::string(
                                    std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
                            }
                            files.push_back(std::move(f));
                        }
                    }
                }
            }
        }
    } catch (...) {
        throw;
    }

    out["files"] = std::move(files);

    // V11.4-IDX-001: carry column-index DEFINITIONS in the snapshot. Contents are not
    // shipped — they are rebuilt locally from the restored base — but without the
    // definitions a receiving node silently loses every index and then validates as
    // healthy, because an empty catalog makes the validator iterate zero fields.
    nlohmann::json catalogs = nlohmann::json::array();
    try {
        const fs::path snapshotRoot = requireSnapshotPinRoot(pinnedRoot);
        for (auto& userDir : fs::directory_iterator(snapshotRoot)) {
            const std::string userId = userDir.path().filename().string();
            if (!userDir.is_directory() || userId == "raft" || userId == "logs"
                || userId == "backups" || userId == "restores"
                || (!userId.empty() && userId.front() == '.')) continue;
            for (auto& dbDir : fs::directory_iterator(userDir)) {
                if (!dbDir.is_directory()) continue;
                const std::string dbName = dbDir.path().filename().string();
                for (auto& entry : fs::directory_iterator(dbDir)) {
                    if (!entry.is_directory()) continue;
                    const std::string dirName = entry.path().filename().string();
                    if (dirName.size() <= 4 || dirName.substr(dirName.size() - 4) != ".idx") continue;
                    const std::string collection = dirName.substr(0, dirName.size() - 4);
                    const json catalog = loadIndexCatalog(entry.path());
                    if (!catalog.contains("indexes") || !catalog["indexes"].is_array()
                        || catalog["indexes"].empty()) continue;
                    catalogs.push_back({
                        {"userId", userId}, {"dbName", dbName}, {"collection", collection},
                        {"catalog", catalog},
                        {"catalogEntryChecksum", catalogChecksumOf(catalog)},
                        {"indexCount", catalog["indexes"].size()},
                    });
                }
            }
        }
    } catch (...) {
        throw;   // a partial catalog must never be shipped as a complete one
    }
    out["index_catalog_schema_version"] = 1;
    out["index_catalog"] = catalogs;
    out["index_catalog_count"] = catalogs.size();
    out["index_catalog_checksum"] = std::to_string(
        pacificdb::durability::ChecksumCalculator::crc32(catalogs.dump()));
    return out;
}

// Apply snapshot payload exported by exportSnapshotJson. This will write files
// into the LSM_ROOT atomically (best-effort) and clear memtables/WALs so the
// follower is consistent with the snapshot state.
bool LSM::applySnapshot(const nlohmann::json& payload,
                        const std::filesystem::path& bundlePath) {
    try {
        if (!payload.is_object()) return false;
        if (LSM_ROOT.empty()) return false;
        const bool bundled = payload.value("version", 0) == 3;
        std::optional<pacificdb::snapshot_bundle::Bundle> bundle;
        if (bundled) {
            if (bundlePath.empty()) {
                throw std::runtime_error("snapshot bundle path is missing");
            }
            bundle = pacificdb::snapshot_bundle::read(bundlePath);
            if (!bundle->manifest.contains("lsm_payload") ||
                bundle->manifest["lsm_payload"] != payload) {
                throw std::runtime_error("snapshot bundle payload does not match manifest");
            }
        }

        // Prepare temporary directory for atomic switch
        fs::path tmpRoot = LSM::requireContained(
            fs::path(LSM::rootOrThrow()) / ".snapshot_tmp");
        if (fs::exists(tmpRoot)) fs::remove_all(tmpRoot);
        createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), tmpRoot);

        std::set<fs::path> restoredCollections;
        // Write each file from payload into tmpRoot preserving relative paths
        if (payload.contains("files") && payload["files"].is_array()) {
            size_t artifactIndex = 0;
            for (auto& f : payload["files"]) {
                std::string rel = f.value("path", std::string());
                if (!bundled && (!f.contains("content") || !f["content"].is_string())) {
                    throw std::runtime_error("snapshot artifact content is malformed");
                }
                const fs::path relativePath =
                    validateStorageRelativePath(fs::path(rel), "snapshotArtifact");
                fs::path target = LSM::requireContained(tmpRoot / relativePath);
                createContainedStorageDirectories(
                    fs::path(LSM::rootOrThrow()), target.parent_path());
                const std::string encoding = f.value("contentEncoding", std::string());
                const bool encodedSst = target.extension() == ".sst";
                if (bundled) {
                    pacificdb::snapshot_bundle::extract(
                        bundlePath, *bundle, artifactIndex, target);
                } else if (encodedSst && encoding == "pdb2-sst-base64") {
                    const std::string& content = f["content"].get_ref<const std::string&>();
                    const std::string bytes = decodeBase64(content);
                    std::ofstream out(target, std::ios::binary | std::ios::trunc);
                    if (!out) throw std::runtime_error("snapshot cannot write " + target.string());
                    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                    out.flush();
                    out.close();
                    if (!out || !pacificdb::storage_v2::isBinarySst(target)) {
                        throw std::runtime_error("snapshot binary SST is malformed: " + target.string());
                    }
                } else if (encodedSst && encoding == "msgpack-base64") {
                    const std::string& content = f["content"].get_ref<const std::string&>();
                    const std::string packedBytes = decodeBase64(content);
                    json rows = json::from_msgpack(
                        packedBytes.begin(), packedBytes.end(), true, false);
                    if (!rows.is_array()) throw std::runtime_error("snapshot SST rows are malformed");
                    std::vector<std::reference_wrapper<const json>> refs;
                    refs.reserve(rows.size());
                    for (const auto& row : rows) refs.emplace_back(row);
                    std::string writeError;
                    if (!pacificdb::storage_v2::writeSst(
                            target, refs, sstBlockBytes(), nullptr, &writeError)) {
                        throw std::runtime_error("snapshot cannot write binary SST " +
                            target.string() + ": " + writeError);
                    }
                } else {
                    const std::string& content = f["content"].get_ref<const std::string&>();
                    std::ofstream out(target.string(), std::ios::binary | std::ios::trunc);
                    if (!out.is_open()) throw std::runtime_error("snapshot cannot write " + target.string());
                    out.write(content.data(), static_cast<std::streamsize>(content.size()));
                    out.flush(); out.close();
                    if (!out) throw std::runtime_error("snapshot write failed " + target.string());
                }
                // Bloom and sparse-ID sidecars are derived locally from the
                // checksummed snapshot SST. Build them in the unpublished
                // staging tree so the restored SST is never exposed without
                // the structures required for bounded indexed reads.
                if (target.extension() == ".sst") {
                    restoredCollections.insert(target.parent_path());
                    LSM::buildBloomForSST(target.string());
                    buildSparseIndexForSST(target.string());
                    if (!fs::exists(target.string() + ".bloom") ||
                        (!pacificdb::storage_v2::isBinarySst(target) &&
                         !fs::exists(target.string() + ".sidx"))) {
                        throw std::runtime_error(
                            "snapshot SST sidecar construction failed for " + target.string());
                    }
                }
                ++artifactIndex;
            }
        }

        for (const auto& directory : restoredCollections) {
            publishMissingSstManifest(directory);
        }

        // Now move existing LSM files to backup and replace with tmpRoot contents
        // Use a move-by-child strategy to avoid renaming the root dir (which fails on Windows)
        fs::path rootPath = fs::path(LSM::rootOrThrow());
        fs::path backup = LSM::requireContained(rootPath / ".snapshot_backup");
        if (fs::exists(backup)) fs::remove_all(backup);
        createContainedStorageDirectories(rootPath, backup);

        // move children of LSM_ROOT into backup (skip if they are tmp/backup by name)
        for (auto& e : fs::directory_iterator(rootPath)) {
            fs::path src = e.path();
            fs::path dest = backup / src.filename();
            if (src == tmpRoot || src == backup) continue;
            const std::string name = src.filename().string();
            // Raft progress/logs and engine-owned metadata are not LSM state.
            // Moving them during an LSM snapshot creates a crash window in which
            // the state machine has advanced but its consensus proof is missing.
            if (name == "raft" || name == "logs" || name == "backups" ||
                (!name.empty() && name.front() == '.')) continue;
            validateStorageIdentifier(name, "snapshotRootIdentifier");
            validateContainedStoragePath(rootPath, src);
            validateContainedStoragePath(rootPath, dest);
            try {
                fs::rename(src, dest);
            } catch (...) {
                fs::copy(src, dest, fs::copy_options::recursive);
                fs::remove_all(src);
            }
        }

        // move tmpRoot contents into LSM_ROOT
        for (auto& p : fs::directory_iterator(tmpRoot)) {
            fs::path src = p.path();
            fs::path dest = rootPath / src.filename();
            validateStorageIdentifier(
                src.filename().string(), "snapshotRootIdentifier");
            validateContainedStoragePath(rootPath, src);
            validateContainedStoragePath(rootPath, dest);
            try {
                fs::rename(src, dest);
            } catch (...) {
                fs::copy(src, dest, fs::copy_options::recursive);
                fs::remove_all(src);
            }
        }

        // cleanup tmpRoot
        if (fs::exists(tmpRoot)) fs::remove_all(tmpRoot);

        // clear in-memory memtables
        {
            std::unique_lock<std::shared_mutex> lk(lsm_global_mutex);
            memtables.clear();
            immutableMemtables.clear();
        }
        clearAllIdIndex();

        // remove WAL files to avoid replaying old operations that predate snapshot
        for (auto& u : fs::directory_iterator(rootPath)) {
            const std::string userId = u.path().filename().string();
            if (userId == "raft" || userId == "logs" || userId == "backups" ||
                userId == "restores" ||
                (!userId.empty() && userId.front() == '.')) {
                continue;
            }
            validateContainedStoragePath(rootPath, u.path());
            if (!u.is_directory()) continue;
            validateStorageIdentifier(userId, "snapshotUserIdentifier");
            for (auto& db : fs::directory_iterator(u.path())) {
                validateContainedStoragePath(rootPath, db.path());
                if (!db.is_directory()) continue;
                validateStorageIdentifier(
                    db.path().filename().string(),
                    "snapshotDatabaseIdentifier");
                fs::path walDir = db.path() / "wal";
                if (fs::exists(walDir)) {
                    validateContainedStoragePath(rootPath, walDir);
                    for (auto& w : fs::directory_iterator(walDir)) {
                        validateContainedStoragePath(rootPath, w.path());
                        fs::remove(w.path());
                    }
                }
            }
        }

        // ══ V11.4-IDX-001: restore index DEFINITIONS and schedule the content rebuild ══
        // The snapshot restored base data only. Without this the collection silently
        // loses every index and the validator then reports a false clean.
        {
            std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
            g_idxRecoverySnapshotIndex = payload.value("lastIncludedIndex", static_cast<uint64_t>(0));
            g_idxRecoveryDeclared.clear();
            g_idxRecoveryError.clear();
        }
        setIndexRecoveryState(IndexRecoveryState::SNAPSHOT_BASE_RESTORED);

        if (payload.contains("index_catalog")) {
            if (!payload["index_catalog"].is_array()) {
                throw std::runtime_error("snapshot index_catalog section is malformed");
            }
            const auto& catalogs = payload["index_catalog"];
            const std::string expected = payload.value("index_catalog_checksum", std::string());
            const std::string actual = std::to_string(
                pacificdb::durability::ChecksumCalculator::crc32(catalogs.dump()));
            if (expected.empty() || expected != actual) {
                // Fail closed: a snapshot that claims indexes must never be silently
                // converted into a zero-index collection.
                throw std::runtime_error("snapshot index_catalog checksum invalid");
            }
            if (payload.contains("index_catalog_count")
                && payload["index_catalog_count"].get<size_t>() != catalogs.size()) {
                throw std::runtime_error("snapshot index_catalog count mismatch");
            }

            std::vector<DeclaredIndex> declared;
            for (const auto& c : catalogs) {
                const std::string userId = c.value("userId", std::string());
                const std::string dbName = c.value("dbName", std::string());
                const std::string collection = c.value("collection", std::string());
                if (userId.empty() || dbName.empty() || collection.empty()
                    || !c.contains("catalog") || !c["catalog"].is_object()) {
                    throw std::runtime_error("snapshot index definition is malformed");
                }
                const json& catalog = c["catalog"];
                if (!catalog.contains("indexes") || !catalog["indexes"].is_array()) {
                    throw std::runtime_error("snapshot index definition has no index array");
                }
                const std::string entryChecksum = c.value("catalogEntryChecksum", std::string());
                if (entryChecksum.empty() || entryChecksum != catalogChecksumOf(catalog)) {
                    throw std::runtime_error("snapshot index catalog entry checksum invalid");
                }
                validateStorageIdentifier(userId, "snapshotIndexUserIdentifier");
                validateStorageIdentifier(dbName, "snapshotIndexDatabaseIdentifier");
                validateStorageIdentifier(collection, "snapshotIndexCollectionIdentifier");

                const fs::path idxDir =
                    LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
                createContainedStorageDirectories(fs::path(LSM::rootOrThrow()), idxDir);
                writeIndexCatalog(idxDir, catalog);
                declared.push_back(DeclaredIndex{userId, dbName, collection, catalog});
            }
            {
                std::lock_guard<std::mutex> lk(g_idxRecoveryMutex);
                g_idxRecoveryDeclared = declared;
                g_idxRecoveryDeclaredCount = declared.size();
            }
            setIndexRecoveryState(IndexRecoveryState::INDEX_CATALOG_RESTORED);
            pacificdb::test::hitFailpoint("FP_IDX_AFTER_CATALOG_RESTORE", 1);
            if (declared.empty()) {
                setIndexRecoveryState(IndexRecoveryState::INDEX_REBUILD_COMPLETE);
                publishIndexRecoveryReadiness();   // nothing declared: valid
            } else {
                setIndexRecoveryState(IndexRecoveryState::INDEX_REBUILD_REQUIRED);
            }
        } else {
            // A snapshot with no catalog section declares no indexes; zero coverage is
            // legitimate and recovery is settled.
            setIndexRecoveryState(IndexRecoveryState::INDEX_REBUILD_COMPLETE);
            publishIndexRecoveryReadiness();
        }

        std::cout << "[LSM] Applied snapshot payload (lastIncludedIndex=" << payload.value("lastIncludedIndex", 0) << ")" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[LSM] applySnapshot failed: " << e.what() << std::endl;
        setIndexRecoveryState(IndexRecoveryState::FAILED, e.what());
        return false;
    } catch (...) {
        setIndexRecoveryState(
            IndexRecoveryState::FAILED,
            "unknown snapshot application failure");
        return false;
    }
}

std::vector<json> LSM::getAll(const std::string& userId, const std::string& dbName, const std::string& collection) {
    auto t0_exec = std::chrono::steady_clock::now();
    std::string key = colKey(userId, dbName, collection);
    FIND_STEP(1, "start stage=getAll key=" << key);
    std::vector<json> memtableDocs;
    std::vector<std::shared_ptr<Memtable>> immutableSnapshots;
    bool hadMemtableDocs = false;
    bool hadImmutableDocs = false;

    std::unique_ptr<std::shared_lock<std::shared_mutex>> lk;
    auto lockWaitStart = std::chrono::steady_clock::now();
    FIND_LOG("[LOCK WAIT] start stage=getAll key=" << key);
    FIND_STEP(2, "before lock stage=getAll key=" << key);
    if (!isFindLockBypassEnabled()) {
        lk = std::make_unique<std::shared_lock<std::shared_mutex>>(getCollectionMutex(key));
        auto lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lockWaitStart).count();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::LockWait,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lockWaitStart).count()));
        FIND_LOG("[LOCK WAIT] end stage=getAll key=" << key << " wait_ms=" << lockWaitMs);
        FIND_STEP(3, "after lock stage=getAll key=" << key);
    } else {
        FIND_LOG("[LOCK WAIT] bypass stage=getAll key=" << key);
        FIND_STEP(3, "after lock stage=getAll key=" << key << " (LOCK BYPASS ENABLED)");
    }

    uint64_t committedIndex = 0;
    try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) { committedIndex = 0; }

    auto mtIt = memtables.find(key);
    if (mtIt != memtables.end()) {
        memtableDocs.reserve(mtIt->second.size());
        for (auto& [id, doc] : mtIt->second) {
            (void)id;
            if (!docVisibleByCommitIndex(doc, committedIndex)) continue;
            memtableDocs.push_back(doc);
        }
        hadMemtableDocs = !mtIt->second.empty();
    }
    immutableSnapshots = immutableSnapshotForKey(key);
    for (const auto& imm : immutableSnapshots) {
        if (imm && !imm->empty()) {
            hadImmutableDocs = true;
            break;
        }
    }
    lk.reset();

    std::unordered_map<std::string, json> latestById;
    std::vector<json> noIdDocs;
    fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");

    FIND_STEP(4, "before index lookup stage=getAll key=" << key << " (n/a path)");
    FIND_STEP(5, "after index lookup stage=getAll key=" << key << " (n/a path)");

    if (fs::exists(dir)) {
        // Keep the file set stable from directory enumeration through the last
        // open. Compaction publishes and unlinks its input SSTs under this same
        // collection mutex.
        auto sstFileSetLock = stableCollectionReadLock(key);
        FIND_LOG("[DISK READ] start stage=getAll key=" << key);
        FIND_STEP(6, "before disk read stage=getAll key=" << key);
        const auto sstFiles = publishedSstsForCollection(dir);

        // v4.4H: track read amplification (SSTs scanned per getAll) + bloom metrics
        g_sstScansPerReadTotal.fetch_add(sstFiles.size(), std::memory_order_relaxed);
        g_sstScansPerReadCount.fetch_add(1, std::memory_order_relaxed);
        recordSstScanSample(sstFiles.size());
        if (!sstFiles.empty()) g_readSstHits.fetch_add(1, std::memory_order_relaxed);

        for (const auto& sstPath : sstFiles) {
            // getAll has no lookup key, so a Bloom filter cannot safely skip an SST.
            std::string error;
            if (!scanSstRows(sstPath, [&](json&& doc) {
                    if (!docVisibleByCommitIndex(doc, committedIndex)) return true;
                    mergeLatestById(latestById, noIdDocs, doc);
                    return true;
                }, &error)) {
                throw std::runtime_error("cannot scan SST " + sstPath.string() + ": " + error);
            }
        }
        FIND_LOG("[DISK READ] end stage=getAll key=" << key << " docs=" << (latestById.size() + noIdDocs.size()));
        FIND_STEP(7, "after disk read stage=getAll key=" << key << " docs=" << (latestById.size() + noIdDocs.size()));
    } else {
        FIND_LOG("[DISK READ] start stage=getAll key=" << key << " (no sst dir)");
        FIND_LOG("[DISK READ] end stage=getAll key=" << key << " docs=0");
        FIND_STEP(6, "before disk read stage=getAll key=" << key << " (no sst dir)");
        FIND_STEP(7, "after disk read stage=getAll key=" << key << " docs=0");
    }

    // overlay immutable memtables oldest -> newest, then active memtable.
    // Active is newest; immutable deque stores newest at front.
    for (auto it = immutableSnapshots.rbegin(); it != immutableSnapshots.rend(); ++it) {
        if (*it) mergeVisibleMemtableInto(latestById, noIdDocs, **it, committedIndex);
    }

    // overlay active memtable snapshot (newest entries)
    for (const auto& doc : memtableDocs) {
        mergeLatestById(latestById, noIdDocs, doc);
    }

    std::vector<json> outDocs;
    outDocs.reserve(latestById.size() + noIdDocs.size());
    for (const auto& doc : noIdDocs) {
        if (!isDeletedDoc(doc)) {
            outDocs.push_back(doc);
        }
    }
    for (const auto& [id, doc] : latestById) {
        (void)id;
        if (!isDeletedDoc(doc)) {
            outDocs.push_back(doc);
        }
    }

    if ((hadMemtableDocs || hadImmutableDocs) && fs::exists(dir)) {
        LSM::setLastReadVisibilitySource("MERGED");
    } else if (hadMemtableDocs || hadImmutableDocs) {
        LSM::setLastReadVisibilitySource("MEMTABLE");
    } else if (fs::exists(dir)) {
        LSM::setLastReadVisibilitySource("SST_L0");
    } else {
        LSM::setLastReadVisibilitySource("MEMTABLE");
    }

    // v4.4H: record memtable hit and total read execute time
    if (hadMemtableDocs) g_readMemtableHits.fetch_add(1, std::memory_order_relaxed);
    if (hadImmutableDocs) g_readImmutableHits.fetch_add(1, std::memory_order_relaxed);
    uint64_t execMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0_exec).count();
    g_readExecuteMsTotal.fetch_add(execMs, std::memory_order_relaxed);
    g_readExecuteCount.fetch_add(1, std::memory_order_relaxed);
    recordReadExecSample(execMs);

    LSM_LOG("[LSM][GETALL] returning " << outDocs.size() << " docs for " << key);
    FIND_STEP(8, "before return stage=getAll key=" << key << " docs=" << outDocs.size());
    return outDocs;
}

std::optional<json> LSM::LatestRowView::field(std::string_view name) const {
    if (msgpack_) return msgpack_->field(name);
    if (!json_ || !json_->is_object()) return std::nullopt;
    auto found = json_->find(std::string(name));
    return found == json_->end() ? std::nullopt : std::optional<json>(*found);
}

json LSM::LatestRowView::materialize() const {
    if (msgpack_) return msgpack_->materialize();
    return json_ ? *json_ : json::object();
}

bool LSM::visitLatest(const std::string& userId,
                      const std::string& dbName,
                      const std::string& collection,
                      const std::function<bool(const LatestRowView&)>& visitor) {
    struct Candidate {
        json materialized;
        bool hasVersion{false};
        long long version{std::numeric_limits<long long>::min()};
        uint64_t order{0};
    };

    const std::string key = colKey(userId, dbName, collection);
    uint64_t committedIndex = 0;
    try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) {}

    auto versionOf = [](const LatestRowView& row, long long& version) {
        for (const char* field : {"_mvcc_version", "version", "deleted_txn",
                                  "created_txn", "_timestamp"}) {
            const auto value = row.field(field);
            if (!value) continue;
            try {
                if (value->is_number_integer() || value->is_number_unsigned()) {
                    version = value->get<long long>();
                    return true;
                }
                if (value->is_string()) {
                    version = std::stoll(value->get<std::string>());
                    return true;
                }
            } catch (...) {}
        }
        return false;
    };
    auto visible = [&](const LatestRowView& row) {
        json metadata = json::object();
        for (const char* field : {"_visibility_state", "visibility_state", "committed",
                                  "_raft_commit_index", "commit_index", "_visibility_floor"}) {
            if (auto value = row.field(field)) metadata[field] = std::move(*value);
        }
        return docVisibleByCommitIndex(metadata, committedIndex);
    };

    std::unordered_map<std::string, Candidate> latest;
    std::vector<Candidate> noId;
    uint64_t order = 0;
    auto considerMemory = [&](const LatestRowView& row) {
        ++order;
        if (!visible(row)) return;
        const auto idValue = row.field("id");
        std::string id;
        if (idValue) {
            try { id = idValue->is_string() ? idValue->get<std::string>() : idValue->dump(); }
            catch (...) {}
        }
        Candidate incoming;
        incoming.hasVersion = versionOf(row, incoming.version);
        incoming.order = order;
        incoming.materialized = row.materialize();
        if (id.empty()) {
            noId.push_back(std::move(incoming));
            return;
        }
        auto current = latest.find(id);
        const bool replace = current == latest.end() ||
            (incoming.hasVersion && current->second.hasVersion &&
             incoming.version > current->second.version) ||
            (incoming.hasVersion != current->second.hasVersion && incoming.hasVersion) ||
            (incoming.hasVersion == current->second.hasVersion &&
             incoming.version == current->second.version && incoming.order > current->second.order);
        if (replace) latest[id] = std::move(incoming);
    };

    // Capture in-memory state first. A flush that starts after this snapshot may
    // publish another SST, but its rows are already present in `active` or the
    // retained immutable snapshots, so no row can fall between the two views.
    std::vector<std::shared_ptr<Memtable>> immutable;
    std::vector<json> active;
    {
        auto lock = stableCollectionReadLock(key);
        immutable = immutableSnapshotForKey(key);
        auto table = memtables.find(key);
        if (table != memtables.end()) {
            active.reserve(table->second.size());
            for (const auto& [id, doc] : table->second) { (void)id; active.push_back(doc); }
        }
    }

    for (auto it = immutable.rbegin(); it != immutable.rend(); ++it) {
        if (!*it) continue;
        for (const auto& [id, doc] : **it) {
            (void)id;
            considerMemory(LatestRowView(doc));
        }
    }
    for (const auto& doc : active) considerMemory(LatestRowView(doc));

    auto visitCandidate = [&](const Candidate& candidate) {
        const LatestRowView row(candidate.materialized);
        const auto deleted = row.field("_deleted");
        return deleted && deleted->is_boolean() && deleted->get<bool>()
            ? true : visitor(row);
    };
    std::vector<std::string> memoryIds;
    memoryIds.reserve(latest.size());
    for (const auto& [id, candidate] : latest) {
        (void)candidate;
        memoryIds.push_back(id);
    }
    std::sort(memoryIds.begin(), memoryIds.end());
    size_t memoryPosition = 0;

    bool hadSst = false;
    bool keepVisiting = true;
    const fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
    if (fs::exists(dir)) {
        auto fileSetLock = stableCollectionReadLock(key);
        const auto files = publishedSstsForCollection(dir);
        hadSst = !files.empty();
        g_sstScansPerReadTotal.fetch_add(files.size(), std::memory_order_relaxed);
        g_sstScansPerReadCount.fetch_add(1, std::memory_order_relaxed);
        recordSstScanSample(files.size());
        std::string error;
        if (!pacificdb::storage_v2::visitLatestSstRows(
                files, [&](const pacificdb::storage_v2::MsgpackRowView& encoded) {
                    const LatestRowView row(encoded);
                    const auto idValue = row.field("id");
                    const std::string id = idValue
                        ? (idValue->is_string() ? idValue->get<std::string>() : idValue->dump())
                        : std::string();
                    while (memoryPosition < memoryIds.size() &&
                           memoryIds[memoryPosition] < id) {
                        keepVisiting = visitCandidate(latest.at(memoryIds[memoryPosition++]));
                        if (!keepVisiting) return false;
                    }
                    if (memoryPosition < memoryIds.size() &&
                        memoryIds[memoryPosition] == id) {
                        keepVisiting = visitCandidate(latest.at(memoryIds[memoryPosition++]));
                        return keepVisiting;
                    }
                    if (!visible(row)) return true;
                    keepVisiting = visitor(row);
                    return keepVisiting;
                }, &error)) {
            throw std::runtime_error("cannot stream published SSTs: " + error);
        }
    }
    if (!keepVisiting) return true;
    LSM::setLastReadVisibilitySource((hadSst && (!active.empty() || !immutable.empty()))
        ? "MERGED" : (hadSst ? "SST_L0" : "MEMTABLE"));
    while (memoryPosition < memoryIds.size()) {
        if (!visitCandidate(latest.at(memoryIds[memoryPosition++]))) return true;
    }
    for (const auto& candidate : noId) if (!visitCandidate(candidate)) return true;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════════════════
// VISIBILITY CLOSURE: Snapshot-bound strong read (memtable-first, newest-SST-first)
// ════════════════════════════════════════════════════════════════════════════════════════
static std::atomic<uint64_t> g_lsmIteratorId{1};
static std::atomic<uint64_t> g_lsmSnapshotId{1};

nlohmann::json LSM::findStrongSnapshotBound(const std::string& userId,
                                            const std::string& dbName,
                                            const std::string& collection,
                                            const std::string& id,
                                            uint64_t requiredFloorVersion) {
    nlohmann::json out;
    out["found"] = false;
    // After the caller's linearizable Raft barrier, an authoritative miss is a
    // valid result when no key-specific visibility floor was requested. A
    // positive floor still means a prior write must become visible, so misses
    // continue to retry in that case.
    out["floor_satisfied"] = (requiredFloorVersion == 0);
    out["visibility_source"] = "UNKNOWN";

    // memtables are keyed with colKey's collision-safe length-prefixed format.
    // The legacy slash key made committed, unflushed documents invisible to
    // snapshot-bound strong reads until an SST flush happened.
    std::string key = colKey(userId, dbName, collection);
    uint64_t raftCommit = 0;
    try { raftCommit = RaftCore::instance().getCommitIndex(); } catch (...) { raftCommit = 0; }

    auto resultFor = [&](const json& candidate,
                         const std::string& source,
                         const std::string& sstFile = std::string(),
                         uint64_t sstGeneration = 0,
                         uint64_t iteratorId = 0,
                         uint64_t snapshotId = 0) {
        json result = out;
        long long version = std::numeric_limits<long long>::min();
        const bool hasVersion = extractDocVersionScore(candidate, version);
        const bool floorOk = requiredFloorVersion == 0
            || (hasVersion && version >= 0 && static_cast<uint64_t>(version) >= requiredFloorVersion);
        const bool deleted = isDeletedDoc(candidate);
        result["found"] = !deleted;
        result["floor_satisfied"] = floorOk;
        if (hasVersion) {
            result["returned_version"] = version;
            result["snapshot_seq"] = version;
        }
        result["visibility_source"] = source;
        result["sst_file"] = sstFile;
        result["sst_generation"] = sstGeneration;
        result["iterator_id"] = iteratorId;
        result["snapshot_id"] = snapshotId;
        result["documents"] = deleted ? json::array() : json::array({candidate});
        return result;
    };

    // The ID index is updated in the same collection critical section as every
    // memtable mutation. A visible entry at or above the requested key floor is
    // therefore already the authoritative point-read result. Cache misses and
    // unsatisfied floors retain the complete storage-layer comparison below.
    if (auto cached = lookupIdIndex(key, id);
        cached.has_value() && docVisibleByCommitIndex(*cached, raftCommit)) {
        json cachedResult = resultFor(*cached, "CACHE");
        if (cachedResult.value("floor_satisfied", false)) return cachedResult;
    }

    std::optional<json> newest;
    std::string newestSource = "UNKNOWN";
    std::string newestSstFile;
    uint64_t newestSstGeneration = 0;
    uint64_t newestIteratorId = 0;
    uint64_t newestSnapshotId = 0;
    auto consider = [&](const json& candidate,
                        const std::string& source,
                        const std::string& sstFile = std::string(),
                        uint64_t sstGeneration = 0,
                        uint64_t iteratorId = 0,
                        uint64_t snapshotId = 0) {
        if (!docVisibleByCommitIndex(candidate, raftCommit)) return;
        if (newest.has_value() && !isCandidateNewer(candidate, *newest)) return;
        newest = candidate;
        newestSource = source;
        newestSstFile = sstFile;
        newestSstGeneration = sstGeneration;
        newestIteratorId = iteratorId;
        newestSnapshotId = snapshotId;
    };

    // Compare every storage layer. Recovery can temporarily leave an older
    // replayed value in a memtable while a newer committed value is already in
    // an SST, so layer order alone cannot establish recency.
    {
        auto lock = pacificdb::timing::makeTimedSharedLock(getCollectionMutex(key));
        auto mtIt = memtables.find(key);
        if (mtIt != memtables.end()) {
            auto it = mtIt->second.find(id);
            if (it != mtIt->second.end()) {
                consider(it->second, "MEMTABLE");
            }
        }
        auto imm = immutableSnapshotForKey(key);
        for (const auto& table : imm) {
            if (!table) continue;
            auto it = table->find(id);
            if (it == table->end()) continue;
            consider(it->second, "IMMUTABLE_MEMTABLE");
            g_readImmutableHits.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // SSTs may contain multiple historical rows for one ID. The targeted seek
    // returns the newest row within each file; compare those rows globally.
    fs::path dir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
    if (fs::exists(dir)) {
        const auto sstFiles = publishedSstsForCollection(dir);
        for (const auto& sst : sstFiles) {
            uint64_t iteratorId = g_lsmIteratorId.fetch_add(1, std::memory_order_release);
            uint64_t snapshotId = g_lsmSnapshotId.fetch_add(1, std::memory_order_release);
            auto found = seekDocInSSTById(sst.string(), id);
            if (!found.has_value()) continue;
            uint64_t generation = 0;
            try {
                const std::string name = sst.filename().string();
                const size_t pos = name.find('_');
                if (pos != std::string::npos) generation = std::stoull(name.substr(pos + 1));
            } catch (...) {}
            consider(*found, "SST_L0", sst.filename().string(), generation, iteratorId, snapshotId);
        }
    }

    if (!newest.has_value()) return out;

    upsertIdIndex(key, id, *newest);
    return resultFor(*newest, newestSource, newestSstFile, newestSstGeneration,
                     newestIteratorId, newestSnapshotId);
}

// ⚡ FAST FIELD LOOKUP - Uses index files for 40x+ speedup
std::vector<json> LSM::findByField(const std::string& userId, const std::string& dbName, const std::string& collection, const std::string& field, const std::string& value, size_t maxCandidates) {
    std::vector<json> results;
    std::string colKey_str = colKey(userId, dbName, collection);
    uint64_t committedIndex = 0;
    try { committedIndex = RaftCore::instance().getCommitIndex(); } catch (...) { committedIndex = 0; }
    FIND_STEP(1, "start stage=findByField key=" << colKey_str << " field=" << field << " value=" << value);
    const size_t candidateLimit = maxCandidates > 0 ? std::max<size_t>(maxCandidates * 2, maxCandidates + 8) : 0;

    // Fast O(1) path for id lookup from in-memory index/cache.
    if (field == "id") {
        const bool shadowStrongIdCache = g_skipIdCacheForStrongRead;
        const std::optional<json> shadowCandidate = shadowStrongIdCache
            ? lookupIdIndex(colKey_str, value)
            : std::nullopt;
        auto recordStrongIdShadow = [&](const std::optional<json>& authoritative) {
            if (!shadowStrongIdCache) return;
            if (!shadowCandidate.has_value()) {
                g_strongIdCacheShadowMisses.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const bool candidateVisible = docVisibleByCommitIndex(*shadowCandidate, committedIndex);
            if (candidateVisible && authoritative.has_value() && *shadowCandidate == *authoritative) {
                g_strongIdCacheShadowMatches.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_strongIdCacheShadowMismatches.fetch_add(1, std::memory_order_relaxed);
            }
        };
        // Visibility closure: skip inMemoryIdIndex for strong reads to prevent
        // returning stale cached versions that don't respect read fences.
        if (!g_skipIdCacheForStrongRead) {
            auto cached = lookupIdIndex(colKey_str, value);
            if (cached.has_value()) {
                if (!docVisibleByCommitIndex(*cached, committedIndex)) {
                    removeIdIndex(colKey_str, value);
                    LSM::setLastReadVisibilitySource("CACHE_VISIBILITY_REJECTED");
                    FIND_LOG("[INDEX LOOKUP END] stage=memory_id_cache key=" << colKey_str << " hit=invisible");
                    return {};
                }
                if (!(cached->contains("_deleted") && (*cached)["_deleted"].is_boolean() && (*cached)["_deleted"].get<bool>())) {
                    LSM::setLastReadVisibilitySource("CACHE");
                    FIND_LOG("[INDEX LOOKUP START] stage=memory_id_cache key=" << colKey_str << " id=" << value);
                    FIND_LOG("[INDEX LOOKUP END] stage=memory_id_cache key=" << colKey_str << " hit=true");
                    FIND_STEP(8, "before return stage=memory_id_cache key=" << colKey_str << " results=1");
                    return { *cached };
                }
                LSM::setLastReadVisibilitySource("CACHE");
                FIND_LOG("[INDEX LOOKUP END] stage=memory_id_cache key=" << colKey_str << " hit=deleted");
                FIND_STEP(8, "before return stage=memory_id_cache key=" << colKey_str << " results=0");
                return {};
            }
        } else {
            FIND_LOG("[VISIBILITY] Bypassing inMemoryIdIndex for strong read id=" << value);
        }

        // If not in memory cache/index, perform direct targeted lookup across
        // SSTs plus the live memtable. Strong reads deliberately invalidate the
        // cache before coming here, so the memtable overlay is required for
        // read-your-writes visibility when a newer value has not flushed yet.
        FIND_LOG("[INDEX LOOKUP START] stage=memory_id_cache key=" << colKey_str << " id=" << value);
        FIND_LOG("[INDEX LOOKUP END] stage=memory_id_cache key=" << colKey_str << " hit=false");
        fs::path sstDir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
        FIND_LOG("[DISK READ] start stage=id_direct_scan key=" << colKey_str << " id=" << value);
        std::optional<json> latestFound;
        if (fs::exists(sstDir)) {
            auto sstFileSetLock = stableCollectionReadLock(colKey_str);
            const auto sstFiles = publishedSstsForCollection(sstDir);

            for (const auto& sstPath : sstFiles) {
                auto found = seekDocInSSTById(sstPath.string(), value);
                if (!found.has_value()) continue;
                if (!docVisibleByCommitIndex(*found, committedIndex)) continue;
                if (!latestFound.has_value() || isCandidateNewer(*found, *latestFound)) {
                    latestFound = *found;
                }
            }
        }
        {
            std::unique_ptr<std::shared_lock<std::shared_mutex>> lk;
            if (!isFindLockBypassEnabled()) {
                auto lockWaitStart = std::chrono::steady_clock::now();
                lk = std::make_unique<std::shared_lock<std::shared_mutex>>(getCollectionMutex(colKey_str));
                pacificdb::timing::recordStage(pacificdb::timing::Stage::LockWait,
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lockWaitStart).count()));
            }
            auto mtIt = memtables.find(colKey_str);
            if (mtIt != memtables.end()) {
                auto docIt = mtIt->second.find(value);
                if (docIt != mtIt->second.end()) {
                    if (docVisibleByCommitIndex(docIt->second, committedIndex) &&
                        (!latestFound.has_value() || isCandidateNewer(docIt->second, *latestFound))) {
                        latestFound = docIt->second;
                    }
                }
            }
            auto imm = immutableSnapshotForKey(colKey_str);
            for (const auto& table : imm) {
                if (!table) continue;
                auto docIt = table->find(value);
                if (docIt == table->end()) continue;
                if (docVisibleByCommitIndex(docIt->second, committedIndex) &&
                    (!latestFound.has_value() || isCandidateNewer(docIt->second, *latestFound))) {
                    latestFound = docIt->second;
                    g_readImmutableHits.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        }

        if (latestFound.has_value()) {
            recordStrongIdShadow(latestFound);
            if (isDeletedDoc(*latestFound)) {
                upsertIdIndex(colKey_str, value, *latestFound);
                LSM::setLastReadVisibilitySource("REPAIRED");
                FIND_LOG("[DISK READ] end stage=id_direct_scan key=" << colKey_str << " id=" << value << " deleted=true");
                FIND_STEP(8, "before return stage=id_direct_scan key=" << colKey_str << " results=0");
                return {};
            }

            upsertIdIndex(colKey_str, value, *latestFound);
            LSM::setLastReadVisibilitySource((latestFound->contains("_deleted") && latestFound->at("_deleted").is_boolean()) ? "REPAIRED" : (fs::exists(sstDir) ? "MERGED" : "MEMTABLE"));
            FIND_LOG("[DISK READ] end stage=id_direct_scan key=" << colKey_str << " id=" << value << " hit=true");
            FIND_STEP(8, "before return stage=id_direct_scan key=" << colKey_str << " results=1");
            return {*latestFound};
        }
        recordStrongIdShadow(std::nullopt);
        FIND_LOG("[DISK READ] end stage=id_direct_scan key=" << colKey_str << " id=" << value << " hit=false");
        FIND_STEP(8, "before return stage=id_direct_scan key=" << colKey_str << " results=0");
        return {};
    }

    bool indexAvailable = false;
    bool indexValueHit = false;
    std::unordered_set<std::string> pendingIds;

    // Step 1: Try index lookup (40x faster) under lock
    {
        std::unique_ptr<std::shared_lock<std::shared_mutex>> lk;
        auto lockWaitStart = std::chrono::steady_clock::now();
        FIND_LOG("[LOCK WAIT] start stage=index key=" << colKey_str);
        FIND_STEP(2, "before lock stage=index key=" << colKey_str);
        if (!isFindLockBypassEnabled()) {
            lk = std::make_unique<std::shared_lock<std::shared_mutex>>(getCollectionMutex(colKey_str));
            auto lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lockWaitStart).count();
            pacificdb::timing::recordStage(pacificdb::timing::Stage::LockWait,
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lockWaitStart).count()));
            FIND_LOG("[LOCK WAIT] end stage=index key=" << colKey_str << " wait_ms=" << lockWaitMs);
            FIND_STEP(3, "after lock stage=index key=" << colKey_str);
        } else {
            FIND_LOG("[LOCK WAIT] bypass stage=index key=" << colKey_str);
            FIND_STEP(3, "after lock stage=index key=" << colKey_str << " (LOCK BYPASS ENABLED)");
        }

        FIND_LOG("[INDEX LOOKUP START] stage=index key=" << colKey_str << " field=" << field);
        FIND_STEP(4, "before index lookup stage=index key=" << colKey_str << " field=" << field);
        fs::path idxDir = LSM::collectionArtifactPath(userId, dbName, collection, ".idx");
        {
            bool fileExists = false, valueHit = false;
            std::vector<std::string> ids;
            const bool catalogReady = readyIndexForField(idxDir, field).has_value() && !hasPendingIndexUpdates(colKey_str);
            if (catalogReady) {
                const bool binary = binaryColumnIndexLookup(
                    idxDir, field, value, candidateLimit, ids, fileExists, valueHit);
                if (!binary) {
                    columnIndexLookupCached(indexFieldArtifactPath(idxDir, field), value,
                                            candidateLimit, ids, fileExists, valueHit);
                }
            }
            if (fileExists) {
                indexAvailable = true;
                if (valueHit) {
                    indexValueHit = true;
                    for (auto& id : ids) {
                        pendingIds.insert(id);
                        if (candidateLimit > 0 && pendingIds.size() >= candidateLimit) break;
                    }
                    FIND_STEP(5, "after index lookup stage=index key=" << colKey_str << " hit=true ids=" << ids.size());
                } else {
                    FIND_STEP(5, "after index lookup stage=index key=" << colKey_str << " hit=true ids=0");
                }
            }
        }

        if (!indexAvailable) {
            FIND_STEP(5, "after index lookup stage=index key=" << colKey_str << " hit=false");
        }
        FIND_LOG("[INDEX LOOKUP END] stage=index key=" << colKey_str
                 << " hit=" << (indexAvailable ? "true" : "false")
                 << " value_hit=" << (indexValueHit ? "true" : "false")
                 << " ids=" << pendingIds.size());
    }

    // If an index file exists for this field, it is authoritative. A missing
    // value means "no matches", not "scan the whole collection".
    if (indexAvailable) {
        if (!indexValueHit) {
            g_columnIndexAuthoritativeEmpty.fetch_add(1, std::memory_order_relaxed);
            LSM::setLastReadVisibilitySource("INDEX_EMPTY");
            FIND_STEP(6, "before disk read stage=index_sst_scan key=" << colKey_str << " pending=0");
            FIND_STEP(7, "after disk read stage=index_sst_scan key=" << colKey_str << " pending=0");
            FIND_STEP(8, "before return stage=index key=" << colKey_str << " results=0");
            return {};
        }

        g_columnIndexHits.fetch_add(1, std::memory_order_relaxed);

        std::unordered_map<std::string, json> latestById;

        // Check memtable first: it has the newest version. If a visible
        // memtable entry exists for an indexed ID, it supersedes all SST copies.
        {
            std::unique_ptr<std::shared_lock<std::shared_mutex>> lk;
            if (!isFindLockBypassEnabled()) {
                auto lockWaitStart = std::chrono::steady_clock::now();
                lk = std::make_unique<std::shared_lock<std::shared_mutex>>(getCollectionMutex(colKey_str));
                pacificdb::timing::recordStage(pacificdb::timing::Stage::LockWait,
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lockWaitStart).count()));
            }
            auto mtIt = memtables.find(colKey_str);
            if (mtIt != memtables.end()) {
                auto& mt = mtIt->second;
                for (auto it = pendingIds.begin(); it != pendingIds.end(); ) {
                    auto docIt = mt.find(*it);
                    if (docIt == mt.end() || !docVisibleByCommitIndex(docIt->second, committedIndex)) {
                        ++it;
                        continue;
                    }
                    latestById[*it] = docIt->second;
                    it = pendingIds.erase(it);
                }
            }
            auto imm = immutableSnapshotForKey(colKey_str);
            for (const auto& table : imm) {
                if (!table || pendingIds.empty()) continue;
                for (auto it = pendingIds.begin(); it != pendingIds.end(); ) {
                    auto docIt = table->find(*it);
                    if (docIt == table->end() || !docVisibleByCommitIndex(docIt->second, committedIndex)) {
                        ++it;
                        continue;
                    }
                    latestById[*it] = docIt->second;
                    it = pendingIds.erase(it);
                    g_readImmutableHits.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // For bounded lookups (the common API path is indexed equality + small limit),
        // targeted per-id SST seeks avoid scanning every SST line for low-cardinality
        // values such as status=active.
        if (!pendingIds.empty()) {
            fs::path sstDir = LSM::collectionArtifactPath(userId, dbName, collection, ".lsm");
            if (fs::exists(sstDir)) {
                auto sstFileSetLock = stableCollectionReadLock(colKey_str);
                FIND_LOG("[DISK READ] start stage=index_sst_scan key=" << colKey_str << " pending=" << pendingIds.size());
                FIND_STEP(6, "before disk read stage=index_sst_scan key=" << colKey_str << " pending=" << pendingIds.size());
                const auto sstFiles = publishedSstsForCollection(sstDir);

                // A targeted seek is cheap only when every SST has a sparse-ID
                // sidecar. Snapshot-restored SSTs from older releases can lack
                // it; seeking N candidates then degenerates into N full scans.
                // In that state, scan each SST once for the whole candidate set.
                const bool everySstHasSparseIndex = std::all_of(
                    sstFiles.begin(), sstFiles.end(), [](const fs::path& path) {
                        return sstHasPointIndex(path);
                    });
                if (pendingIds.size() <= columnIndexTargetedSeekMaxIds() &&
                    everySstHasSparseIndex) {
                    for (const auto& id : pendingIds) {
                        for (const auto& sstPath : sstFiles) {
                            auto found = seekDocInSSTById(sstPath.string(), id);
                            if (!found.has_value()) continue;
                            if (!docVisibleByCommitIndex(*found, committedIndex)) continue;
                            auto existing = latestById.find(id);
                            if (existing == latestById.end() || isCandidateNewer(*found, existing->second)) {
                                latestById[id] = *found;
                            }
                        }
                    }
                } else {
                    if (!everySstHasSparseIndex) {
                        g_columnIndexSinglePassMissingSidecar.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    // Large candidate sets are cheaper as one pass over SST files. Do not
                    // erase an ID on first sight: older SSTs can contain obsolete versions.
                    for (const auto& sstPath : sstFiles) {
                        std::string error;
                        if (!scanSstRows(sstPath, [&](json&& doc) {
                                if (!docVisibleByCommitIndex(doc, committedIndex)) return true;
                                if (!doc.contains("id") || !doc["id"].is_string()) return true;
                                std::string id = doc["id"].get<std::string>();
                                if (pendingIds.find(id) == pendingIds.end()) return true;
                                auto existing = latestById.find(id);
                                if (existing == latestById.end() || isCandidateNewer(doc, existing->second)) {
                                    latestById[id] = std::move(doc);
                                }
                                return true;
                            }, &error)) {
                            throw std::runtime_error(
                                "cannot scan indexed SST " + sstPath.string() + ": " + error);
                        }
                    }
                }
                FIND_LOG("[DISK READ] end stage=index_sst_scan key=" << colKey_str << " pending=" << pendingIds.size());
                FIND_STEP(7, "after disk read stage=index_sst_scan key=" << colKey_str << " pending=" << pendingIds.size());
            } else {
                FIND_LOG("[DISK READ] start stage=index_sst_scan key=" << colKey_str << " (no sst dir)");
                FIND_LOG("[DISK READ] end stage=index_sst_scan key=" << colKey_str << " pending=" << pendingIds.size());
                FIND_STEP(6, "before disk read stage=index_sst_scan key=" << colKey_str << " (no sst dir)");
                FIND_STEP(7, "after disk read stage=index_sst_scan key=" << colKey_str << " pending=" << pendingIds.size());
            }
        } else {
            FIND_STEP(6, "before disk read stage=index_sst_scan key=" << colKey_str << " pending=0");
            FIND_STEP(7, "after disk read stage=index_sst_scan key=" << colKey_str << " pending=0");
        }

        size_t staleOrMissing = 0;
        for (const auto& id : pendingIds) {
            auto it = latestById.find(id);
            if (it == latestById.end()) {
                staleOrMissing++;
                continue;
            }
            const json& doc = it->second;
            if (isDeletedDoc(doc) || !docFieldMatchesColumnValue(doc, field, value)) {
                staleOrMissing++;
                continue;
            }
            results.push_back(doc);
        }
        for (const auto& [id, doc] : latestById) {
            if (pendingIds.find(id) != pendingIds.end()) continue;
            if (isDeletedDoc(doc) || !docFieldMatchesColumnValue(doc, field, value)) {
                staleOrMissing++;
                continue;
            }
            results.push_back(doc);
        }
        if (staleOrMissing > 0) {
            g_columnIndexStaleReads.fetch_add(staleOrMissing, std::memory_order_relaxed);
        }

        LSM_LOG("[LSM][FINDBYFIELD] ⚡ Index hit: " << field << "=" << value << " found " << results.size() << " docs (FAST)");
        LSM::setLastReadVisibilitySource(results.empty() ? "INDEX_EMPTY" : "INDEX");
        FIND_STEP(8, "before return stage=index key=" << colKey_str << " results=" << results.size());
        return results;
    }

    // Step 2: Fallback to full scan (slow, but still works) without holding the mutex to avoid deadlocks
    g_columnIndexFallbackScans.fetch_add(1, std::memory_order_relaxed);
    LSM_LOG("[LSM][FINDBYFIELD] ⚠️ Index miss: " << field << "=" << value << " - falling back to scan");
    size_t scanned = 0;
    visitLatest(userId, dbName, collection, [&](const LatestRowView& row) {
        ++scanned;
        const auto fieldValue = row.field(field);
        if (fieldValue && columnIndexValue(*fieldValue) == value) {
            results.push_back(row.materialize());
        }
        return true;
    });
    FIND_LOG("[FIND] fallback scan key=" << colKey_str << " docs=" << scanned);

    FIND_STEP(8, "before return stage=fallback key=" << colKey_str << " results=" << results.size());

    return results;
}
