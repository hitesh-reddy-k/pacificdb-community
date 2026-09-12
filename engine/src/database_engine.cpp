#include "database_engine.hpp"
#include "test_failpoint.hpp"
#include <atomic>
#include "query_cancellation.hpp"
#include "storage.hpp"
#include "wal.hpp"
#include "logger.hpp"
#include "query.hpp"
#include "update_ops.hpp"
#include "lsm.hpp"
#include "query_limiter.hpp"
#include "raft_core.hpp"
#include "memory_manager.hpp"
#include "id_generator.hpp"
#include "shard_manager.hpp"
#include "data_durability.hpp"
#include "request_timing.hpp"
#include "metrics_exporter.hpp"
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iostream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <unordered_set>
#include <cstdlib>

// Engine debug logging — off by default for throughput, enable with ENGINE_DEBUG_LOG=1
static bool g_engineDebug = (std::getenv("ENGINE_DEBUG_LOG") && std::string(std::getenv("ENGINE_DEBUG_LOG")) == "1");
#define ELOG(x) do { if (g_engineDebug) { std::cout << x; } } while(0)

namespace fs = std::filesystem;
using json = nlohmann::json;

using pacificdb::durability::ChecksumCalculator;

static std::mutex g_findLogMutex;
static std::atomic<uint64_t> g_mvccVersionCounter{1};

namespace {
struct StoragePipelineScope {
    StoragePipelineScope() {
        MetricsExporter::incrementCounter("pacificdb_pipeline_storage_started_total", 1.0);
    }
    ~StoragePipelineScope() {
        MetricsExporter::incrementCounter("pacificdb_pipeline_storage_completed_total", 1.0);
    }
};
}

struct ReadContextState {
    std::string consistency = "eventual";
    long long maxStalenessMs = 0;
    long long readFloorVersion = -1;
    uint64_t snapshotVersion = 0;
    std::chrono::steady_clock::time_point snapshotTime = std::chrono::steady_clock::now();
};

thread_local ReadContextState g_readContext;

static std::string DATA_ROOT;

static fs::path basePath(const std::string& userId, const std::string& dbName);
static long long nowMs();
static void replayMediaManifestWals();
static void cleanupOrphanMediaChunks();

struct ReadFenceEntry {
    long long version = -1;
    std::chrono::steady_clock::time_point updatedAt;
};

static std::unordered_map<std::string, ReadFenceEntry> g_readFence;
static std::deque<std::string> g_readFenceOrder;
static std::mutex g_readFenceMutex;

static size_t readFenceMaxKeys() {
    static const size_t value = [] {
        const char* v = std::getenv("STRONG_READ_FENCE_MAX_KEYS");
        if (!v) return size_t{20000};
        try { return std::max<size_t>(1024, std::stoul(v)); } catch (...) { return size_t{20000}; }
    }();
    return value;
}

static std::string makeReadFenceKey(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection,
                                    const std::string& id) {
    return userId + "|" + dbName + "|" + collection + "|" + id;
}

static void updateReadFenceFromDoc(const std::string& userId,
                                   const std::string& dbName,
                                   const std::string& collection,
                                   const json& doc) {
    if (!doc.is_object()) return;
    if (!doc.contains("id") || !doc["id"].is_string()) return;

    const char* versionField = doc.contains("_mvcc_version") ? "_mvcc_version" : "version";
    if (!doc.contains(versionField)) return;
    if (!doc[versionField].is_number_integer() && !doc[versionField].is_number_unsigned()) return;

    const std::string id = doc["id"].get<std::string>();
    if (id.empty()) return;
    const long long version = doc[versionField].get<long long>();

    const std::string key = makeReadFenceKey(userId, dbName, collection, id);
    const size_t maxKeys = readFenceMaxKeys();

    std::lock_guard<std::mutex> lk(g_readFenceMutex);
    auto it = g_readFence.find(key);
    if (it == g_readFence.end()) {
        g_readFence[key] = { version, std::chrono::steady_clock::now() };
        g_readFenceOrder.push_back(key);
    } else if (version > it->second.version) {
        it->second.version = version;
        it->second.updatedAt = std::chrono::steady_clock::now();
    }

    while (g_readFence.size() > maxKeys && !g_readFenceOrder.empty()) {
        const std::string victim = g_readFenceOrder.front();
        g_readFenceOrder.pop_front();
        auto vit = g_readFence.find(victim);
        if (vit != g_readFence.end()) {
            g_readFence.erase(vit);
        }
    }
}

static bool getReadFenceVersionInternal(const std::string& userId,
                                        const std::string& dbName,
                                        const std::string& collection,
                                        const std::string& id,
                                        long long& outVersion) {
    const std::string key = makeReadFenceKey(userId, dbName, collection, id);
    std::lock_guard<std::mutex> lk(g_readFenceMutex);
    auto it = g_readFence.find(key);
    if (it == g_readFence.end()) return false;
    outVersion = it->second.version;
    return true;
}

static bool documentVisibleForCurrentRead(const json& doc, bool bypassRaft = false);

static bool envFalse(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s == "0" || s == "false" || s == "no" || s == "off";
}

static bool standaloneRaftBypassEnabled() {
    static const bool explicitlyDisabled =
        envFalse("RAFT_STANDALONE_BYPASS") || envFalse("RAFT_BYPASS_SINGLE_NODE");
    if (explicitlyDisabled) return false;
    try {
        return RaftCore::instance().isEnabled() &&
               RaftCore::instance().isLeader() &&
               RaftCore::instance().peerCount() == 0;
    } catch (...) {
        return false;
    }
}

static double dotProduct(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
    return s;
}

static double magnitude(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x * x;
    return std::sqrt(s);
}

static double cosineSimilarity(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return std::numeric_limits<double>::quiet_NaN();
    double denom = magnitude(a) * magnitude(b);
    if (denom < 1e-12) return 0.0;
    return dotProduct(a, b) / denom;
}

static double l2(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return std::numeric_limits<double>::quiet_NaN();
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d = a[i] - b[i]; s += d * d;
    }
    return std::sqrt(s);
}

static std::vector<double> jsonToVector(const json& jvec) {
    std::vector<double> v;
    if (!jvec.is_array()) return v;
    v.reserve(jvec.size());
    for (auto& x : jvec) {
        if (x.is_number()) v.push_back(x.get<double>());
        else if (x.is_string()) {
            try { v.push_back(std::stod(x.get<std::string>())); } catch (...) {}
        }
    }
    return v;
}

struct HnswLiteNode {
    std::string id;
    std::vector<double> vec;
    json doc;
    std::vector<int> neighbors;
};

struct HnswLiteIndex {
    std::vector<HnswLiteNode> nodes;
    size_t signature = 0;
};

static std::unordered_map<std::string, HnswLiteIndex> g_hnswLiteIndexes;
static std::mutex g_hnswLiteMutex;

static bool hnswLiteEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("VECTOR_HNSW_ENABLED");
        if (!v) return true;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE" || s == "off");
    }();
    return enabled;
}

static int hnswLiteM() {
    static const int value = [] {
        const char* v = std::getenv("VECTOR_HNSW_M");
        if (!v) return 8;
        try { return std::max(4, std::min(48, std::stoi(v))); } catch (...) { return 8; }
    }();
    return value;
}

static int hnswLiteEfSearch() {
    static const int value = [] {
        const char* v = std::getenv("VECTOR_HNSW_EF_SEARCH");
        if (!v) return 64;
        try { return std::max(16, std::min(2048, std::stoi(v))); } catch (...) { return 64; }
    }();
    return value;
}

static bool hnswLiteAllowFallback() {
    static const bool enabled = [] {
        const char* v = std::getenv("VECTOR_HNSW_ALLOW_FALLBACK");
        if (!v) return true;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE" || s == "off");
    }();
    return enabled;
}

static std::string vectorIndexKey(const std::string& userId,
                                  const std::string& dbName,
                                  const std::string& collection) {
    return userId + "|" + dbName + "|" + collection;
}

static double vectorScore(const std::vector<double>& q, const std::vector<double>& v, const std::string& metric) {
    if (q.size() != v.size() || q.empty()) return std::numeric_limits<double>::quiet_NaN();
    if (metric == "l2" || metric == "euclidean") return -l2(q, v);
    if (metric == "dot" || metric == "dot_product") return dotProduct(q, v);
    return cosineSimilarity(q, v);
}

static void rebuildHnswLiteIndex(HnswLiteIndex& idx,
                                 const std::vector<json>& docs,
                                 const std::string& metric) {
    idx.nodes.clear();
    idx.nodes.reserve(docs.size());

    for (const auto& d : docs) {
        if (d.contains("_deleted") && d["_deleted"].is_boolean() && d["_deleted"].get<bool>()) continue;
        if (!d.contains("vector") || !d["vector"].is_array()) continue;
        auto v = jsonToVector(d["vector"]);
        if (v.empty()) continue;
        idx.nodes.push_back(HnswLiteNode{d.value("id", std::string("")), std::move(v), d, {}});
    }

    const int M = hnswLiteM();
    for (int i = 0; i < static_cast<int>(idx.nodes.size()); ++i) {
        if (i == 0) continue;
        std::vector<std::pair<double, int>> scored;
        scored.reserve(i);
        for (int j = 0; j < i; ++j) {
            double s = vectorScore(idx.nodes[i].vec, idx.nodes[j].vec, metric);
            scored.push_back({s, j});
        }
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int degree = std::min(M, static_cast<int>(scored.size()));
        for (int k = 0; k < degree; ++k) {
            int nb = scored[k].second;
            idx.nodes[i].neighbors.push_back(nb);
            idx.nodes[nb].neighbors.push_back(i);
        }
    }
}

static std::vector<int> searchHnswLiteCandidates(const HnswLiteIndex& idx,
                                                 const std::vector<double>& q,
                                                 const std::string& metric,
                                                 int efSearch) {
    std::vector<int> result;
    if (idx.nodes.empty() || q.empty()) return result;

    struct ScoredNode { double score; int node; };
    auto cmp = [](const ScoredNode& a, const ScoredNode& b) { return a.score < b.score; };
    std::priority_queue<ScoredNode, std::vector<ScoredNode>, decltype(cmp)> frontier(cmp);

    int entry = 0;
    double entryScore = vectorScore(q, idx.nodes[entry].vec, metric);
    frontier.push({entryScore, entry});

    std::unordered_set<int> visited;
    visited.insert(entry);

    while (!frontier.empty() && static_cast<int>(result.size()) < efSearch) {
        auto cur = frontier.top();
        frontier.pop();
        result.push_back(cur.node);

        for (int nb : idx.nodes[cur.node].neighbors) {
            if (visited.find(nb) != visited.end()) continue;
            visited.insert(nb);
            double ns = vectorScore(q, idx.nodes[nb].vec, metric);
            frontier.push({ns, nb});
        }
    }

    return result;
}

static void invalidateVectorIndex(const std::string& userId,
                                  const std::string& dbName,
                                  const std::string& collection) {
    std::lock_guard<std::mutex> lk(g_hnswLiteMutex);
    g_hnswLiteIndexes.erase(vectorIndexKey(userId, dbName, collection));
}

/* ---------------- INIT ---------------- */
void DatabaseEngine::configureStorageRoot(const std::string& rootPath) {
    if (rootPath.empty() || !fs::path(rootPath).is_absolute()) {
        throw std::runtime_error(
            "DatabaseEngine storage root requires an explicit absolute DATA_ROOT");
    }
    std::error_code rootEc;
    fs::create_directories(rootPath, rootEc);
    if (rootEc) {
        throw std::runtime_error(
            "DatabaseEngine cannot create DATA_ROOT: " +
            rootEc.message());
    }
    const fs::path canonicalRoot = fs::canonical(rootPath, rootEc);
    if (rootEc || canonicalRoot.empty()) {
        throw std::runtime_error(
            "DatabaseEngine cannot canonicalize DATA_ROOT");
    }
    DATA_ROOT = canonicalRoot.string();
    std::cout << "[ENGINE] Data root initialized: " << DATA_ROOT << std::endl;
}

void DatabaseEngine::init(const std::string& rootPath, bool restoreWal) {
    configureStorageRoot(rootPath);

    // Ensure system workspace and database exist for auth/metadata collections
    ensureUserRoot("system");
    createDatabase("system", "system", "binary");
    createCollection("system", "system", "users");

    // Initialize the LSM layer. A clean shutdown has already flushed the
    // authoritative state before its marker is written, so replaying every
    // historical collection/media WAL here only duplicates persisted rows and
    // rebuilds all indexes before the listener can bind.
    LSM::init(DATA_ROOT);
    if (restoreWal) {
        LSM::restoreFromWal();
        replayMediaManifestWals();
    } else {
        std::cout << "[ENGINE] Clean shutdown: collection and media WAL restore skipped"
                  << std::endl;
    }
    cleanupOrphanMediaChunks();
    // Ensure system users exist (seed an admin for first-time setups)
    try {
        auto sysUsers = LSM::getAll("system", "system", "users");
        if (sysUsers.empty()) {
            // Fail closed. A bootstrap identity is created only from an
            // operator-generated external password hash; no password is ever
            // computed or printed by the engine.
            const char* bootUser =
                std::getenv("PACIFICDB_BOOTSTRAP_ADMIN_USERNAME");
            const char* bootHash =
                std::getenv("PACIFICDB_BOOTSTRAP_ADMIN_PASSWORD_HASH");
            if (bootUser && bootHash && *bootUser && *bootHash) {
                json admin = {
                    {"id", std::string(bootUser)},
                    {"username", std::string(bootUser)},
                    {"passwordHash", std::string(bootHash)},
                    {"role", "admin"},
                    {"created", std::time(nullptr)}
                };
                LSM::put("system", "system", "users", admin);
                std::cout << "[ENGINE] Seeded bootstrap admin from operator-supplied credentials" << std::endl;
            } else {
                std::cout << "[ENGINE] No bootstrap admin seeded; set PACIFICDB_BOOTSTRAP_ADMIN_USERNAME and PACIFICDB_BOOTSTRAP_ADMIN_PASSWORD_HASH to create one" << std::endl;
            }
        }
    } catch (...) {}

}

std::string DatabaseEngine::getDataRoot() {
    return DATA_ROOT;
}

void DatabaseEngine::setReadContext(const std::string& consistency,
                                    long long maxStalenessMs,
                                    long long readFloorVersion) {
    g_readContext.consistency = consistency;
    g_readContext.maxStalenessMs = std::max<long long>(0, maxStalenessMs);
    g_readContext.readFloorVersion = readFloorVersion;
    // A Raft commit index is a snapshot boundary, not a per-document MVCC
    // floor. Applying it as a document version floor hides every valid row
    // written before the latest unrelated Raft entry. Explicit session/key
    // floors are still honored through readFloorVersion.
    if (RaftCore::instance().isEnabled()) {
        g_readContext.snapshotVersion = RaftCore::instance().getCommitIndex();
    } else {
        g_readContext.snapshotVersion = g_mvccVersionCounter.load(std::memory_order_relaxed);
    }
    g_readContext.snapshotTime = std::chrono::steady_clock::now();
}

void DatabaseEngine::clearReadContext() {
    g_readContext = ReadContextState{};
    LSM::clearLastReadVisibilitySource();
    LSM::clearSkipIdCache();
}

std::string DatabaseEngine::getLastReadVisibilitySource() {
    return LSM::getLastReadVisibilitySource();
}

long long DatabaseEngine::getReadFenceVersion(const std::string& userId,
                                              const std::string& dbName,
                                              const std::string& collection,
                                              const std::string& id,
                                              bool* found) {
    long long version = -1;
    bool ok = getReadFenceVersionInternal(userId, dbName, collection, id, version);
    if (found) *found = ok;
    return ok ? version : -1;
}

void DatabaseEngine::invalidateReadCache(const std::string& userId,
                                         const std::string& dbName,
                                         const std::string& collection,
                                         const std::string& id) {
    if (id.empty()) return;
    try {
        LSM::invalidateIdCache(userId, dbName, collection, id);
    } catch (...) {
        // Cache invalidation is best-effort; the read path still falls back to storage.
    }
}

// Recovery replay re-applies committed records with no client on the other end, so the
// write-backpressure sleep below must not apply to it. Backpressure exists to slow down
// INCOMING client writes when memory is tight; charging it to replay turns a large WAL into
// an unbounded startup delay (measured: 100 ms per record, ~41 minutes for 24.5k records)
// and can prevent a node from ever rejoining.
static std::atomic<bool> g_recoveryReplayMode{false};

void DatabaseEngine::setRecoveryReplayMode(bool enabled) {
    g_recoveryReplayMode.store(enabled, std::memory_order_release);
}

bool DatabaseEngine::recoveryReplayMode() {
    return g_recoveryReplayMode.load(std::memory_order_acquire);
}

bool DatabaseEngine::applyReplicatedEntry(const nlohmann::json& entry) {
    try {
        const std::uint64_t applyIndex =
            entry.value("_raft_commit_index", static_cast<std::uint64_t>(0));
        ELOG("[ENGINE][APPLY] START - Received entry with keys: ");
        if (g_engineDebug) { for (auto& k : entry.items()) std::cout << k.key() << " "; std::cout << std::endl; }

        ELOG("[ENGINE][APPLY] Full entry: " << entry.dump() << std::endl);

        if (!entry.contains("userId")) {
            std::cerr << "[ENGINE][APPLY] Missing userId" << std::endl;
            return false;
        }
        if (!entry.contains("db")) {
            std::cerr << "[ENGINE][APPLY] Missing db" << std::endl;
            return false;
        }
        // Support multiple schemas: some send "op"+"data", others use "action"+"doc"
        std::string opRaw = entry.value("op", entry.value("action", std::string("INSERT")));
        std::string op = opRaw;
        std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c){ return std::toupper(c); });

        // R2B: normalize logical API operation names into storage operation names.
        // Server may submit action=createDatabase/op=createDatabase/legacyOp=CREATE_DB.
        std::string action = entry.value("action", "");
        std::string legacyOp = entry.value("legacyOp", "");

        if (op == "CREATEDATABASE" || op == "CREATE_DATABASE" || op == "CREATE_DB" ||
            action == "createDatabase" || legacyOp == "CREATE_DB") {
            op = "CREATE_DB";
        }
        if (op == "CREATECOLLECTION" || op == "CREATE_COLLECTION" ||
            action == "createCollection" || legacyOp == "CREATE_COLLECTION") {
            op = "CREATE_COLLECTION";
        }
        if (op == "DROPDATABASE" || op == "DROP_DATABASE" || op == "DROP_DB" ||
            action == "dropDatabase" || legacyOp == "DROP_DB") {
            op = "DROP_DB";
        }
        if (op == "DROPCOLLECTION" || op == "DROP_COLLECTION" ||
            action == "dropCollection" || legacyOp == "DROP_COLLECTION") {
            op = "DROP_COLLECTION";
        }
        if (op == "CREATEINDEX" || op == "CREATE_INDEX" || action == "createIndex" || legacyOp == "CREATE_INDEX") op = "CREATE_INDEX";
        if (op == "DROPINDEX" || op == "DROP_INDEX" || action == "dropIndex" || legacyOp == "DROP_INDEX") op = "DROP_INDEX";
        if (op == "REBUILDINDEX" || op == "REBUILD_INDEX" || action == "rebuildIndex" || action == "indexRebuild" || legacyOp == "REBUILD_INDEX") op = "REBUILD_INDEX";

        // For CREATE_DB, collection/data are not required
        if (op != "CREATE_DB" && op != "DROP_DB") {
            if (!entry.contains("collection")) {
                std::cerr << "[ENGINE][APPLY] Missing collection" << std::endl;
                return false;
            }
        }
        if (op != "CREATE_DB" && op != "CREATE_COLLECTION" && op != "DROP_DB" && op != "DROP_COLLECTION" &&
            op != "CREATE_INDEX" && op != "DROP_INDEX" && op != "REBUILD_INDEX") {
            if (!entry.contains("data") && !entry.contains("doc")) {
                std::cerr << "[ENGINE][APPLY] Missing data/doc" << std::endl;
                return false;
            }
        }

        std::string userId = entry.value("userId", "system");
        std::string dbName = entry.value("db", "");
        std::string collection = entry.value("collection", "");

        // Reserved Community documents use the normal replicated write path.
        // Ensure followers have the local schema before applying the first entry.
        if (dbName == "pacificdb_meta" && !collection.empty() &&
            op != "CREATE_DB" && op != "DROP_DB" &&
            op != "CREATE_COLLECTION" && op != "DROP_COLLECTION") {
            if (!DatabaseEngine::createDatabase(userId, dbName, "binary") ||
                DatabaseEngine::createCollection(userId, dbName, collection).empty()) {
                std::cerr << "[ENGINE][APPLY] Could not initialize reserved Community schema\n";
                return false;
            }
        }
        // Accept either "data" or the older/alternate "doc" key
        json data = json::object();
        if (entry.contains("data")) data = entry["data"];
        else if (entry.contains("doc")) data = entry["doc"];

        ELOG("[ENGINE][APPLY] userId=" << userId << " db=" << dbName
                  << " collection=" << collection << std::endl);
        ELOG("[ENGINE][APPLY] data=" << data.dump() << std::endl);

        ELOG("[ENGINE][APPLY] Handling op=" << op << std::endl);

        if (op == "CREATE_DB") {
            std::string dbType = entry.value("dbType", std::string("binary"));
            std::cerr << "[DLOG][CREATE_DB][APPLY_REPLICATED_ENTRY] requestId="
                      << entry.value("requestId", std::string(""))
                      << " user=" << userId << " db=" << dbName
                      << " type=" << dbType << std::endl;
            ELOG("[ENGINE][APPLY] Creating DB on follower: user=" << userId << " db=" << dbName << " type=" << dbType << std::endl);
            if (!DatabaseEngine::createDatabase(userId, dbName, dbType)) {
                std::cerr << "[ENGINE][APPLY] CREATE_DB failed user=" << userId
                          << " db=" << dbName << std::endl;
                return false;
            }
        } else if (op == "DROP_DB") {
            if (!DatabaseEngine::dropDatabase(userId, dbName)) return false;
        } else if (op == "CREATE_COLLECTION") {
            std::cerr << "[DLOG][CREATE_COLLECTION][APPLY_REPLICATED_ENTRY] requestId="
                      << entry.value("requestId", std::string(""))
                      << " user=" << userId << " db=" << dbName
                      << " collection=" << collection << std::endl;
            ELOG("[ENGINE][APPLY] Creating collection on follower: user=" << userId << " db=" << dbName << " coll=" << collection << std::endl);
            std::string collId = DatabaseEngine::createCollection(userId, dbName, collection);
            if (collId.empty() && !DatabaseEngine::collectionExists(userId, dbName, collection)) {
                std::cerr << "[ENGINE][APPLY] CREATE_COLLECTION failed user=" << userId
                          << " db=" << dbName << " collection=" << collection << std::endl;
                return false;
            }
        } else if (op == "DROP_COLLECTION") {
            if (!DatabaseEngine::dropCollection(userId, dbName, collection)) return false;
        } else if (op == "CREATE_INDEX") {
            json definition = entry.value("index", json::object());
            auto result = LSM::createSecondaryIndex(userId, dbName, collection, definition);
            if (result.value("status", std::string()) == "error") {
                std::cerr << "[ENGINE][APPLY] CREATE_INDEX failed: " << result.dump() << std::endl;
                return false;
            }
        } else if (op == "DROP_INDEX") {
            auto result = LSM::dropSecondaryIndex(userId, dbName, collection, entry.value("indexName", std::string()));
            if (result.value("status", std::string()) == "error") return false;
        } else if (op == "REBUILD_INDEX") {
            auto result = LSM::rebuildSecondaryIndex(userId, dbName, collection, entry.value("indexName", std::string()));
            if (result.value("status", std::string()) == "error") return false;
        } else if (op == "INSERT_MANY" || op == "INSERTMANY") {
            if (!data.is_array()) {
                std::cerr << "[ENGINE][APPLY] INSERT_MANY data must be an array" << std::endl;
                return false;
            }
            std::vector<json> docs;
            docs.reserve(data.size());
            for (auto doc : data) {
                if (!doc.is_object()) continue;
                if (entry.contains("_raft_commit_index")) doc["_raft_commit_index"] = entry["_raft_commit_index"];
                if (entry.contains("_raft_term")) doc["_raft_term"] = entry["_raft_term"];
                if (entry.contains("_visibility_floor")) doc["_visibility_floor"] = entry["_visibility_floor"];
                if (entry.contains("committed")) doc["committed"] = entry["committed"];
                if (entry.contains("_visibility_state")) doc["_visibility_state"] = entry["_visibility_state"];
                updateReadFenceFromDoc(userId, dbName, collection, doc);
                docs.push_back(std::move(doc));
            }
            // V11.4-DIV-001 Phase 0A per-ordinal reapplication trace. Emits what the apply
            // path ACTUALLY received and produced for a committed batch, so a missing
            // document can be attributed to decode, apply, or post-apply loss rather than
            // inferred from source reading. Enabled only when the trace path is set.
            static const std::string div001Trace = [] {
                const char* value = std::getenv("PACIFICDB_DIV001_TRACE");
                return value ? std::string(value) : std::string();
            }();
            if (!div001Trace.empty()) {
                std::ofstream tr(div001Trace, std::ios::app);
                if (tr.is_open()) {
                    for (size_t di = 0; di < docs.size(); ++di) {
                        const auto& d = docs[di];
                        tr << "{\"phase\":\"pre_putmany\",\"applyIndex\":" << applyIndex
                           << ",\"ordinal\":" << di
                           << ",\"id\":" << (d.is_object() && d.contains("id") ? d["id"].dump() : std::string("null"))
                           << ",\"isObject\":" << (d.is_object() ? "true" : "false")
                           << "}\n";
                    }
                    tr << "{\"phase\":\"batch_received\",\"applyIndex\":" << applyIndex
                       << ",\"entryItems\":" << data.size()
                       << ",\"docsToPutMany\":" << docs.size() << "}\n";
                }
            }
            const json result =
                LSM::putMany(userId, dbName, collection, docs, true, applyIndex, false);
            if (!div001Trace.empty()) {
                std::ofstream tr(div001Trace, std::ios::app);
                if (tr.is_open()) {
                    tr << "{\"phase\":\"post_putmany\",\"applyIndex\":" << applyIndex
                       << ",\"status\":" << result.value("status", std::string("?")).c_str()
                       << ",\"reportedInserted\":" << result.value("inserted", 0)
                       << ",\"memtableSize\":" << result.value("memtable_size", 0) << "}\n";
                }
            }
            if (result.value("status", std::string("error")) != "ok") {
                std::cerr << "[ENGINE][APPLY] INSERT_MANY index completion failed: "
                          << result.dump() << std::endl;
                return false;
            }
            // V11.4-DIV-001 durability campaign. At this exact point putMany has reported
            // success and the caller has NOT yet persisted apply progress. Proven state
            // here: the collection WAL append has been issued (write + flush, NOT fsync —
            // WAL_FSYNC defaults false), the memtable is mutated, and any SST flush is
            // only scheduled. Crashing here asks whether recovery reconstructs the batch
            // from the committed Raft log or whether the entry is skipped and lost.
            pacificdb::test::hitFailpoint(
                "DIV001_CRASH_AFTER_PUTMANY_SUCCESS", applyIndex);
            ELOG("[ENGINE][APPLY] INSERT_MANY applied docs=" << docs.size() << std::endl);
        } else {
            // Monotonic register guard: never let an older version overwrite a newer one.
            // This protects linearizable register workloads from version rollback during retries/reordering.
            if ((op == "INSERT" || op == "UPDATE") && data.is_object() && data.contains("id") && data["id"].is_string()) {
                auto parseVersion = [](const json& doc, long long& out) -> bool {
                    if (!doc.is_object() || !doc.contains("version")) return false;
                    try {
                        if (doc["version"].is_number_integer() || doc["version"].is_number_unsigned()) {
                            out = doc["version"].get<long long>();
                            return true;
                        }
                    } catch (...) {}
                    return false;
                };

                long long incomingVersion = 0;
                bool hasIncomingVersion = parseVersion(data, incomingVersion);
                if (hasIncomingVersion) {
                    try {
                        const std::string id = data["id"].get<std::string>();
                        auto existing = LSM::findByField(userId, dbName, collection, "id", id);
                        for (const auto& doc : existing) {
                            if (!doc.is_object()) continue;
                            if (doc.contains("_deleted") && doc["_deleted"].is_boolean() && doc["_deleted"].get<bool>()) continue;
                            if (!doc.contains("id") || !doc["id"].is_string()) continue;
                            if (doc["id"].get<std::string>() != id) continue;

                            long long existingVersion = 0;
                            if (parseVersion(doc, existingVersion) && existingVersion > incomingVersion) {
                                ELOG("[ENGINE][APPLY] Skipping stale version write id=" << id
                                     << " incoming=" << incomingVersion << " existing=" << existingVersion << std::endl);
                                return true;
                            }
                            break;
                        }
                    } catch (...) {
                        // Best effort guard; proceed with write if lookup fails.
                    }
                }
            }

            // Propagate commit watermark metadata into the stored document when available.
            if (data.is_object()) {
                if (entry.contains("_raft_commit_index")) data["_raft_commit_index"] = entry["_raft_commit_index"];
                if (entry.contains("_raft_term")) data["_raft_term"] = entry["_raft_term"];
                if (entry.contains("_visibility_floor")) data["_visibility_floor"] = entry["_visibility_floor"];
                if (entry.contains("committed")) data["committed"] = entry["committed"];
                if (entry.contains("_visibility_state")) data["_visibility_state"] = entry["_visibility_state"];
            }

            // raft/log.bin is the WAL for replicated mutations. Writing the same payload
            // to both db.wal and the collection WAL tripled durable write traffic.
            LSM::put(userId, dbName, collection, data, true, applyIndex, false);
            updateReadFenceFromDoc(userId, dbName, collection, data);
            ELOG("[ENGINE][APPLY] Entry applied to LSM" << std::endl);
        }
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "[ENGINE][APPLY] EXCEPTION: " << ex.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[ENGINE][APPLY] Unknown exception" << std::endl;
        return false;
    }
}

bool DatabaseEngine::applySnapshot(const nlohmann::json& payload,
                                   const std::string& bundlePath) {
    try {
        std::cout << "[ENGINE] Applying snapshot payload to LSM..." << std::endl;
        bool ok = LSM::applySnapshot(payload, bundlePath);
        if (!ok) {
            std::cerr << "[ENGINE] Snapshot application failed" << std::endl;
            return false;
        }
        // V11.4-IDX-001: a snapshot restores base data only. Rebuild the declared column
        // indexes from raw unfiltered base records BEFORE reporting success, so the node
        // cannot become ready with missing indexes. Readiness and strong reads stay
        // withheld until LSM::indexRecoverySettled() is true.
        if (!LSM::runPendingIndexRebuild()) {
            std::cerr << "[ENGINE] Snapshot index rebuild FAILED: "
                      << LSM::indexRecoveryStatus().dump() << std::endl;
            return false;
        }
        std::cout << "[ENGINE] Snapshot applied successfully; index recovery="
                  << LSM::indexRecoveryStatus().dump() << std::endl;
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "[ENGINE] applySnapshot exception: " << ex.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[ENGINE] applySnapshot unknown exception" << std::endl;
        return false;
    }
}

static bool isSafeStorageIdentifier(const std::string& value) {
    try {
        validateStorageIdentifier(value, "storageIdentifier");
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

static void requireStorageNamespace(const std::string& userId,
                                    const std::string& dbName) {
    validateStorageIdentifier(userId, "userId");
    validateStorageIdentifier(dbName, "databaseName");

    const fs::path root(DATA_ROOT);
    const fs::path database = root / userId / dbName;
    validateContainedStoragePath(root, root / userId);
    validateContainedStoragePath(root, database);
    for (const char* child : {"db.meta", "data", "wal", "logs", "media"}) {
        validateContainedStoragePath(root, database / child);
    }
}

static void requireStorageNamespace(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection) {
    requireStorageNamespace(userId, dbName);
    validateStorageIdentifier(collection, "collectionName");

    const fs::path root(DATA_ROOT);
    const fs::path database = root / userId / dbName;
    for (const auto& artifact : {
             database / "data" / (collection + ".bin"),
             database / "wal" / (collection + ".wal"),
             database / (collection + ".lsm"),
             database / (collection + ".idx"),
         }) {
        validateContainedStoragePath(root, artifact);
    }
}

static fs::path basePath(const std::string& userId, const std::string& dbName) {
    requireStorageNamespace(userId, dbName);
    return validateContainedStoragePath(fs::path(DATA_ROOT),
        fs::path(DATA_ROOT) / userId / dbName);
}

// V11.4-PATH-002. basePath() validated its identifiers, but several call sites built
// fs::path(DATA_ROOT) / userId directly and bypassed the check. A userId of "../escape"
// created a directory one level above the data root, and "../../escape2" created
// /tmp/escape2 containing x/wal/db.wal — an engine WAL written outside the root entirely.
// Every user-root path must go through this helper so the identifier is validated exactly
// once, in one place.
static fs::path userRootPath(const std::string& userId) {
    validateStorageIdentifier(userId, "userId");
    return validateContainedStoragePath(fs::path(DATA_ROOT),
        fs::path(DATA_ROOT) / userId);
}

static void clearReadFencesWithPrefix(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(g_readFenceMutex);
    for (auto it = g_readFence.begin(); it != g_readFence.end();) {
        if (it->first.rfind(prefix, 0) == 0) it = g_readFence.erase(it);
        else ++it;
    }
    g_readFenceOrder.erase(std::remove_if(g_readFenceOrder.begin(), g_readFenceOrder.end(),
        [&](const std::string& key) { return key.rfind(prefix, 0) == 0; }), g_readFenceOrder.end());
}

static void clearVectorIndexesWithPrefix(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(g_hnswLiteMutex);
    for (auto it = g_hnswLiteIndexes.begin(); it != g_hnswLiteIndexes.end();) {
        if (it->first.rfind(prefix, 0) == 0) it = g_hnswLiteIndexes.erase(it);
        else ++it;
    }
}

static bool isTenantDocumentVisible(const std::string& userId, const json& doc) {
    if (!doc.contains("tenant_id")) return true;
        if (!doc["tenant_id"].is_string() || doc["tenant_id"].get<std::string>() != userId) return false;
    return doc["tenant_id"].get<std::string>() == userId;
}

static void enforceTenantOnWrite(const std::string& userId, json& doc) {
    if (doc.contains("tenant_id") && doc["tenant_id"].is_string()) {
        const std::string requestedTenant = doc["tenant_id"].get<std::string>();
        if (requestedTenant != userId) {
            throw std::runtime_error("tenant_mismatch_write: request tenant does not match authenticated user");
        }
    }
    doc["tenant_id"] = userId;
}

static uint64_t nextMvccVersion() {
    return g_mvccVersionCounter.fetch_add(1, std::memory_order_relaxed);
}

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static void stampMvccWrite(json& doc, bool preserveCreate = false) {
    const uint64_t mvccVersion = nextMvccVersion();
    const long long tsMs = nowMs();
    if (!preserveCreate || !doc.contains("created_txn")) doc["created_txn"] = mvccVersion;
    if (!preserveCreate || !doc.contains("created_at_ms")) doc["created_at_ms"] = tsMs;
    if (!doc.contains("version")) {
        doc["version"] = mvccVersion;
    }
    doc["_mvcc_version"] = mvccVersion;
    doc["_mvcc_commit_ms"] = tsMs;
    doc["deleted_txn"] = doc.value("deleted_txn", static_cast<uint64_t>(0));
    doc["deleted_at_ms"] = doc.value("deleted_at_ms", static_cast<long long>(0));
}

static void stampMvccDelete(json& doc) {
    const uint64_t version = nextMvccVersion();
    const long long tsMs = nowMs();
    if (!doc.contains("created_txn")) doc["created_txn"] = version;
    if (!doc.contains("created_at_ms")) doc["created_at_ms"] = tsMs;
    doc["deleted_txn"] = version;
    doc["deleted_at_ms"] = tsMs;
    doc["version"] = version;
    doc["_mvcc_version"] = version;
    doc["_mvcc_commit_ms"] = tsMs;
}

static long long resolveReadFloorVersionFromContext() {
    if (g_readContext.readFloorVersion >= 0) return g_readContext.readFloorVersion;
    return -1;
}

static bool documentVisibleForCurrentRead(const json& doc, bool bypassRaft) {
    if (!doc.is_object()) return false;
    if (doc.contains("_deleted") && doc["_deleted"].is_boolean() && doc["_deleted"].get<bool>()) return false;
    if (RaftCore::instance().isEnabled() && !bypassRaft) {
        if (doc.contains("_visibility_state") && doc["_visibility_state"].is_string()) {
            const std::string state = doc["_visibility_state"].get<std::string>();
            if (state == "PENDING") return false;
        }
        if (doc.contains("committed") && doc["committed"].is_boolean() && !doc["committed"].get<bool>()) return false;
        // A zero-peer standalone engine has no RF3 commit index to compare against.
        // This mode is used for isolated restore verification and local development.
        // Explicitly pending/uncommitted records remain hidden above; only the
        // cluster-relative commit/floor comparison is bypassed.
        if (standaloneRaftBypassEnabled()) {
            return true;
        }
        try {
            uint64_t docCommit = 0;
            bool commitFound = false;
            if (doc.contains("_raft_commit_index") && (doc["_raft_commit_index"].is_number_integer() || doc["_raft_commit_index"].is_number_unsigned())) {
                docCommit = doc["_raft_commit_index"].get<uint64_t>();
                commitFound = true;
            }
            if (!commitFound) return false;
            uint64_t liveCommit = RaftCore::instance().getCommitIndex();
            if (docCommit > liveCommit) return false;

            uint64_t visibilityFloor = docCommit;
            if (doc.contains("_visibility_floor") && (doc["_visibility_floor"].is_number_integer() || doc["_visibility_floor"].is_number_unsigned())) {
                visibilityFloor = doc["_visibility_floor"].get<uint64_t>();
            }
            if (visibilityFloor > liveCommit) {
                return false;
            }
        } catch (...) {
            return false;
        }
    }

    const long long floorVersion = resolveReadFloorVersionFromContext();
    if (floorVersion >= 0 && doc.contains("version") && (doc["version"].is_number_integer() || doc["version"].is_number_unsigned())) {
        long long docVersion = doc["version"].get<long long>();
        if (docVersion < floorVersion) return false;
    }

    if (g_readContext.consistency == "bounded" && g_readContext.maxStalenessMs > 0 && doc.contains("_mvcc_commit_ms")) {
        long long commitMs = 0;
        try {
            if (doc["_mvcc_commit_ms"].is_number_integer() || doc["_mvcc_commit_ms"].is_number_unsigned()) {
                commitMs = doc["_mvcc_commit_ms"].get<long long>();
            }
        } catch (...) {}
        if (commitMs > 0) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - g_readContext.snapshotTime).count();
            if (elapsedMs < g_readContext.maxStalenessMs) {
                return true;
            }
        }
    }

    return true;
}

static void appendRaftWriteMeta(json& entry, const json& raftMeta) {
    if (!raftMeta.is_object()) return;

    if (raftMeta.contains("requestId") && raftMeta["requestId"].is_string()) {
        entry["requestId"] = raftMeta["requestId"].get<std::string>();
    }
    if (raftMeta.contains("trace_id") && raftMeta["trace_id"].is_string()) {
        entry["trace_id"] = raftMeta["trace_id"].get<std::string>();
    }
    if (raftMeta.contains("traceparent") && raftMeta["traceparent"].is_string()) {
        entry["traceparent"] = raftMeta["traceparent"].get<std::string>();
    }
    if (raftMeta.contains("leader_term") &&
        (raftMeta["leader_term"].is_number_unsigned() || raftMeta["leader_term"].is_number_integer())) {
        entry["leader_term"] = raftMeta["leader_term"];
    }
    if (raftMeta.contains("_raft_commit_index") &&
        (raftMeta["_raft_commit_index"].is_number_unsigned() || raftMeta["_raft_commit_index"].is_number_integer())) {
        entry["_raft_commit_index"] = raftMeta["_raft_commit_index"];
    }
    if (raftMeta.contains("_raft_term") &&
        (raftMeta["_raft_term"].is_number_unsigned() || raftMeta["_raft_term"].is_number_integer())) {
        entry["_raft_term"] = raftMeta["_raft_term"];
    }
    if (raftMeta.contains("_visibility_floor") &&
        (raftMeta["_visibility_floor"].is_number_unsigned() || raftMeta["_visibility_floor"].is_number_integer())) {
        entry["_visibility_floor"] = raftMeta["_visibility_floor"];
    }
    if (raftMeta.contains("committed") && raftMeta["committed"].is_boolean()) {
        entry["committed"] = raftMeta["committed"];
    }
    if (raftMeta.contains("_visibility_state") && raftMeta["_visibility_state"].is_string()) {
        entry["_visibility_state"] = raftMeta["_visibility_state"];
    }
}

/* ---------------- USER ROOT ---------------- */
void DatabaseEngine::ensureUserRoot(const std::string& userId) {
    validateStorageIdentifier(userId, "userId");
    fs::path userRoot = userRootPath(userId);
    createContainedStorageDirectories(fs::path(DATA_ROOT), userRoot);
    createContainedStorageDirectories(fs::path(DATA_ROOT), userRoot / "data");
    createContainedStorageDirectories(fs::path(DATA_ROOT), userRoot / "wal");
    createContainedStorageDirectories(fs::path(DATA_ROOT), userRoot / "logs");

        // ✅ For system user, ensure the system database exists
    if (userId == "system") {
        fs::path systemDb = userRoot / "system";
        createContainedStorageDirectories(fs::path(DATA_ROOT), systemDb / "data");
        createContainedStorageDirectories(fs::path(DATA_ROOT), systemDb / "wal");
        createContainedStorageDirectories(fs::path(DATA_ROOT), systemDb / "logs");
        createContainedStorageDirectories(fs::path(DATA_ROOT), systemDb / "media");

        // Create metadata file if it doesn't exist
        fs::path metaFile = systemDb / "db.meta";
        if (!fs::exists(metaFile)) {
            json dbMeta = {
                {"type", "binary"},
                {"created", std::time(nullptr)},
                {"collections", json::array()}
            };
            std::ofstream ofs(metaFile);
            ofs << dbMeta.dump(2);
        }
    }
}

/* ---------------- DATABASE WITH TYPE SUPPORT ----------------*/
bool DatabaseEngine::createDatabase(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& dbType) {

    requireStorageNamespace(userId, dbName);

    fs::path userRoot = userRootPath(userId);

    // Ensure user workspace exists (create if missing).
    // createDatabase should be able to initialize the user workspace
    // even if initUserSpace wasn't called separately by the caller.
    ensureUserRoot(userId);
    fs::path base = basePath(userId, dbName);
    fs::path metaFile = base / "db.meta";

    std::cerr << "[DLOG][CREATE_DB][PATH] user=" << userId
              << " db=" << dbName
              << " base=" << base.string()
              << std::endl;

    // CREATE_DB is replicated and may be replayed or submitted again by an
    // idempotent control-plane workflow. Never replace an existing catalog:
    // doing so makes every collection disappear from listCollections even
    // though its data/WAL files are still intact.
    if (fs::exists(metaFile)) {
        std::ifstream existingMeta(metaFile);
        json existing = json::parse(existingMeta, nullptr, false);
        if (existingMeta && !existing.is_discarded() && existing.is_object() &&
            existing.contains("collections") && existing["collections"].is_array()) {
            std::cout << "[ENGINE][CREATE DB] Database already exists; preserving metadata\n";
            return true;
        }
        std::cerr << "[ENGINE][CREATE DB] Existing metadata is unreadable; refusing to overwrite: "
                  << metaFile.string() << "\n";
        return false;
    }

    createContainedStorageDirectories(fs::path(DATA_ROOT), base / "data");
    createContainedStorageDirectories(fs::path(DATA_ROOT), base / "wal");
    createContainedStorageDirectories(fs::path(DATA_ROOT), base / "logs");
    createContainedStorageDirectories(fs::path(DATA_ROOT), base / "media");

    // Create database metadata file with type information
    std::string dbTypeNorm = dbType;
    // normalize to lowercase
    for (auto& c : dbTypeNorm) c = std::tolower(c);

    json dbMeta = {
        {"id", IDGenerator::generateObjectId()},
        {"type", dbTypeNorm},
        {"created", std::time(nullptr)},
        {"collections", json::array()}
    };

    std::ofstream metaOut(metaFile);
    if (!metaOut) {
        std::cerr << "[ENGINE][CREATE DB] Failed to write metadata: " << metaFile.string() << "\n";
        return false;
    }
    metaOut << dbMeta.dump(2);
    metaOut.close();

    Logger::write((base / "logs/db.log").string(), "DATABASE CREATED with type: " + dbTypeNorm);
    std::cout << "[ENGINE][CREATE DB] Database created with type: " << dbTypeNorm << "\n";
    return true;
}

/* -------- GET DATABASE METADATA -------- */
json DatabaseEngine::getDatabaseMetadata(const std::string& userId, const std::string& dbName) {
    requireStorageNamespace(userId, dbName);
    fs::path metaFile = basePath(userId, dbName) / "db.meta";

    if (!fs::exists(metaFile)) {
        return {
            {"type", "binary"},
            {"created", 0},
            {"collections", json::array()}
        };
    }

    std::ifstream metaIn(metaFile);
    if (!metaIn) {
        return {
            {"type", "binary"},
            {"created", 0},
            {"collections", json::array()}
        };
    }

    json meta = json::parse(metaIn, nullptr, false);
    metaIn.close();

    if (meta.is_discarded()) {
        return {
            {"type", "binary"},
            {"created", 0},
            {"collections", json::array()}
        };
    }

    return meta;
}

/* ---------------- COLLECTION ---------------- */
std::string DatabaseEngine::createCollection(const std::string& userId,
                                             const std::string& dbName,
                                             const std::string& collection) {
    requireStorageNamespace(userId, dbName, collection);
    fs::path dbRoot = basePath(userId, dbName);

    // Guard: database must exist
    if (!fs::exists(dbRoot)) {
        std::cerr << "[ENGINE][CREATE COLLECTION] DB does not exist. Create DB before collections. user="
                  << userId << " db=" << dbName << "\n";
        return "";  // Return empty string instead of false
    }

    // Ensure data directory exists
    fs::path dataDir = dbRoot / "data";
    createContainedStorageDirectories(fs::path(DATA_ROOT), dataDir);

    fs::path file = dataDir / (collection + ".bin");
    if (fs::exists(file)) {
        std::cout << "[ENGINE][CREATE COLLECTION] Already exists: " << file.string() << "\n";
        // Attempt to read db.meta to find existing collection id
        try {
            fs::path metaFile = basePath(userId, dbName) / "db.meta";
            json meta = json::object();
            if (fs::exists(metaFile)) {
                std::ifstream in(metaFile);
                if (in) {
                    meta = json::parse(in, nullptr, false);
                    in.close();
                    if (!meta.is_discarded() && meta.contains("collections") && meta["collections"].is_array()) {
                        for (auto &c : meta["collections"]) {
                            if (c.contains("name") && c["name"] == collection && c.contains("id")) {
                                return c["id"].get<std::string>();
                            }
                        }
                    }
                }
            }
            if (meta.is_discarded() || !meta.is_object()) meta = json::object();
            if (!meta.contains("type")) meta["type"] = "binary";
            if (!meta.contains("created")) meta["created"] = std::time(nullptr);
            if (!meta.contains("id")) meta["id"] = IDGenerator::generateObjectId();
            if (!meta.contains("collections") || !meta["collections"].is_array()) {
                meta["collections"] = json::array();
            }

            std::string collId = IDGenerator::generateObjectId();
            json collEntry = { {"name", collection}, {"id", collId}, {"created", std::time(nullptr)} };
            meta["collections"].push_back(collEntry);

            std::ofstream mout(metaFile);
            if (mout) {
                mout << meta.dump(2);
                mout.close();
                std::cout << "[ENGINE][CREATE COLLECTION] Repaired db.meta collection metadata: "
                          << collEntry.dump() << "\n";
                return collId;
            }
        } catch (...) {}
        return std::string();
    }

    std::ofstream out(file, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[ENGINE][CREATE COLLECTION] Failed to create file: " << file.string() << "\n";
        return std::string();
    }

    std::cout << "[ENGINE][CREATE COLLECTION] Created " << file.string() << "\n";

    // Update database metadata to include this collection with generated id
    try {
        fs::path metaFile = basePath(userId, dbName) / "db.meta";
        json meta = json::object();
        if (fs::exists(metaFile)) {
            std::ifstream in(metaFile);
            if (in) meta = json::parse(in, nullptr, false);
            in.close();
            if (meta.is_discarded()) meta = json::object();
        }

        if (!meta.contains("collections") || !meta["collections"].is_array()) {
            meta["collections"] = json::array();
        }

        // Generate a collection id and append
        std::string collId = IDGenerator::generateObjectId();
        json collEntry = { {"name", collection}, {"id", collId}, {"created", std::time(nullptr)} };
        meta["collections"].push_back(collEntry);

        std::ofstream mout(metaFile);
        if (mout) {
            mout << meta.dump(2);
            mout.close();
            std::cout << "[ENGINE][CREATE COLLECTION] Updated db.meta with collection metadata: " << collEntry.dump() << "\n";
        }
        return collId;
    } catch (const std::exception& ex) {
        std::cerr << "[ENGINE][CREATE COLLECTION] Failed to update db.meta: " << ex.what() << "\n";
    }
    return std::string();
}

bool DatabaseEngine::dropCollection(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection) {
    requireStorageNamespace(userId, dbName, collection);

    const fs::path dbRoot = basePath(userId, dbName);
    if (!fs::exists(dbRoot)) return true; // idempotent Raft replay
    if (!LSM::dropCollection(userId, dbName, collection)) return false;

    std::error_code ec;
    fs::remove(dbRoot / "data" / (collection + ".bin"), ec);
    if (ec) return false;
    fs::remove_all(dbRoot / "media" / collection, ec);
    if (ec) return false;

    const fs::path metaFile = dbRoot / "db.meta";
    if (fs::exists(metaFile)) {
        std::ifstream input(metaFile);
        json meta = json::parse(input, nullptr, false);
        input.close();
        if (meta.is_discarded() || !meta.is_object()) return false;
        if (!meta.contains("collections") || !meta["collections"].is_array()) {
            meta["collections"] = json::array();
        }
        json kept = json::array();
        for (const auto& item : meta["collections"]) {
            const std::string name = item.is_string() ? item.get<std::string>()
                : (item.is_object() ? item.value("name", std::string()) : std::string());
            if (name != collection) kept.push_back(item);
        }
        meta["collections"] = std::move(kept);
        const fs::path temp = metaFile.string() + ".drop.tmp";
        std::ofstream output(temp, std::ios::trunc);
        if (!output) return false;
        output << meta.dump(2);
        output.flush();
        if (!output.good()) return false;
        output.close();
        fs::rename(temp, metaFile, ec);
        if (ec) {
            fs::remove(temp);
            return false;
        }
    }

    clearReadFencesWithPrefix(userId + "|" + dbName + "|" + collection + "|");
    clearVectorIndexesWithPrefix(userId + "|" + dbName + "|" + collection);
    return true;
}

bool DatabaseEngine::dropDatabase(const std::string& userId, const std::string& dbName) {
    requireStorageNamespace(userId, dbName);

    const fs::path dbRoot = basePath(userId, dbName);
    if (!fs::exists(dbRoot)) return true; // idempotent Raft replay

    std::set<std::string> collections;
    const json meta = getDatabaseMetadata(userId, dbName);
    if (meta.contains("collections") && meta["collections"].is_array()) {
        for (const auto& item : meta["collections"]) {
            const std::string name = item.is_string() ? item.get<std::string>()
                : (item.is_object() ? item.value("name", std::string()) : std::string());
            if (isSafeStorageIdentifier(name)) collections.insert(name);
        }
    }
    auto collectNames = [&](const fs::path& dir, const std::string& suffix, bool directoriesOnly) {
        std::error_code walkError;
        if (!fs::exists(dir, walkError)) return;
        for (const auto& item : fs::directory_iterator(dir, walkError)) {
            if (walkError) break;
            if (directoriesOnly && !item.is_directory()) continue;
            if (!directoriesOnly && !item.is_regular_file()) continue;
            std::string name = item.path().filename().string();
            if (!suffix.empty() && name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                name.resize(name.size() - suffix.size());
            } else if (!suffix.empty()) {
                continue;
            }
            if (isSafeStorageIdentifier(name)) collections.insert(name);
        }
    };
    collectNames(dbRoot / "data", ".bin", false);
    collectNames(dbRoot, ".lsm", true);
    collectNames(dbRoot, ".idx", true);
    collectNames(dbRoot / "wal", ".wal", false);
    collectNames(dbRoot / "media", "", true);
    collections.erase("media_manifest");

    for (const auto& collection : collections) {
        if (!dropCollection(userId, dbName, collection)) return false;
    }

    std::error_code ec;
    fs::remove_all(dbRoot, ec);
    if (ec) return false;
    clearReadFencesWithPrefix(userId + "|" + dbName + "|");
    clearVectorIndexesWithPrefix(userId + "|" + dbName + "|");
    return true;
}

/* ---------------- LIST DATABASES ---------------- */
std::vector<std::string> DatabaseEngine::listDatabases(const std::string& userId) {
    validateStorageIdentifier(userId, "userId");
    std::vector<std::string> names;

    fs::path userRoot = userRootPath(userId);
    if (!fs::exists(userRoot)) return names;

    for (const auto& entry : fs::directory_iterator(userRoot)) {
        if (entry.is_directory() && fs::exists(entry.path() / "db.meta")) {
            names.push_back(entry.path().filename().string());
        }
    }

    return names;
}

/* ---------------- VALIDATION HELPERS ---------------- */
bool DatabaseEngine::userExists(const std::string& userId) {
    validateStorageIdentifier(userId, "userId");
    fs::path userRoot = userRootPath(userId);
    return fs::exists(userRoot) && fs::is_directory(userRoot);
}

bool DatabaseEngine::databaseExists(const std::string& userId, const std::string& dbName) {
    requireStorageNamespace(userId, dbName);
    fs::path dbPath = basePath(userId, dbName);
    fs::path metaFile = dbPath / "db.meta";
    return fs::exists(dbPath) && fs::is_directory(dbPath) && fs::exists(metaFile);
}

bool DatabaseEngine::collectionExists(const std::string& userId, const std::string& dbName, const std::string& collection) {
    requireStorageNamespace(userId, dbName, collection);
    if (!databaseExists(userId, dbName)) return false;

    // ✅ PRIMARY CHECK: Collection data file exists (most reliable indicator)
    fs::path collectionFile = basePath(userId, dbName) / "data" / (collection + ".bin");
    if (fs::exists(collectionFile)) {
        return true;
    }

    // ✅ SECONDARY CHECK: Collection registered in db.meta (for newly created collections without data yet)
    fs::path metaFile = basePath(userId, dbName) / "db.meta";
    if (!fs::exists(metaFile)) return false;

    try {
        std::ifstream ifs(metaFile);
        json meta;
        ifs >> meta;
        if (meta.contains("collections") && meta["collections"].is_array()) {
            for (auto& c : meta["collections"]) {
                if (c.is_object() && c.contains("name") && c["name"] == collection) return true;
                if (c.is_string() && c == collection) return true;
            }
        }
    } catch (...) {}
    return false;
}

/* ---------------- INSERT ---------------- */
void DatabaseEngine::insert(const std::string& userId,
                            const std::string& dbName,
                            const std::string& collection,
                            json doc,
                            const json& raftMeta) {
    requireStorageNamespace(userId, dbName, collection);
    StoragePipelineScope storageScope;

    ELOG("[ENGINE][INSERT] START insert userId=" << userId << " db=" << dbName
              << " coll=" << collection << std::endl);

    // AUTO-GENERATE ID if not provided (MongoDB-style behavior)
    if (!doc.contains("id") || doc["id"].is_null() ||
        (doc["id"].is_string() && doc["id"].get<std::string>().empty())) {
        std::string generatedId = IDGenerator::generateObjectId();
        doc["id"] = generatedId;
        ELOG("[ENGINE][INSERT] Auto-generated ID: " << generatedId << std::endl);
    }

    // Engine-layer tenant guard: do not rely only on router checks.
    enforceTenantOnWrite(userId, doc);
    stampMvccWrite(doc);

    // Check memory pressure and apply backpressure if needed
    if (!DatabaseEngine::recoveryReplayMode() && MemoryManager::shouldSlowDownWrites()) {
        double usage = MemoryManager::getMemoryUsage() * 100.0;
        ELOG("[ENGINE][INSERT] BACKPRESSURE - Memory at " << usage
                  << "% - slowing down write" << std::endl);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    fs::path dbp = basePath(userId, dbName);
    ELOG("[ENGINE][INSERT] Checking DB path: " << dbp.string() << std::endl);
    if (!fs::exists(dbp)) {
        std::string errMsg = "[ENGINE] DB does not exist. Insert blocked. user=" + userId + " db=" + dbName + " path=" + dbp.string();
        std::cerr << errMsg << "\n";
        throw std::runtime_error(errMsg);
    }


    bool isMainUsers = (dbName == "system" && collection == "users");

    if (isMainUsers) {
        // 🔍 CRITICAL: System users stored in GLOBAL location, not per-user
        fs::path dataFile = basePath("system", "system") / "data" / (collection + ".bin");
        fs::path walFile  = basePath("system", "system") / "wal/db.wal";

        ELOG("[ENGINE][INSERT] System user to: " << dataFile.string() << "\n");

        // append to storage and write WAL
        auto memtableStart = std::chrono::steady_clock::now();
        Storage::appendDocument(dataFile.string(), doc);
        auto memtableEnd = std::chrono::steady_clock::now();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::MemtableInsert,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(memtableEnd - memtableStart).count()));

        json walEntry = {
            {"op", "INSERT"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"data", doc}
        };
        auto walStart = std::chrono::steady_clock::now();
        WAL::log(walFile.string(), walEntry);
        auto walEnd = std::chrono::steady_clock::now();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::WalAppend,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(walEnd - walStart).count()));

        ELOG("[ENGINE] Inserted into .bin storage for system users\n");
        return;
    }

    // If Raft is enabled and this node is the leader, replicate synchronously
    bool raftEnabled = RaftCore::instance().isEnabled();
    ELOG("[ENGINE][INSERT] Raft enabled=" << (raftEnabled ? "YES" : "NO") << std::endl);

    if (raftEnabled) {
        bool isLeader = RaftCore::instance().isLeader();
        ELOG("[ENGINE][INSERT] Is leader=" << (isLeader ? "YES" : "NO") << std::endl);

        if (!isLeader) {
            std::cerr << "[ENGINE] Node is follower; insert must be sent to leader" << std::endl;
            throw std::runtime_error("not_leader");
        }

        json entry = {
            {"op", "INSERT"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"data", doc}
        };
        appendRaftWriteMeta(entry, raftMeta);

        ELOG("[ENGINE][INSERT] Calling replicateAndApply..." << std::endl);
        auto replicationStart = std::chrono::steady_clock::now();
        bool ok = RaftCore::instance().replicateAndApply(entry);
        auto replicationEnd = std::chrono::steady_clock::now();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::Replication,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(replicationEnd - replicationStart).count()));
        ELOG("[ENGINE][INSERT] replicateAndApply returned: " << (ok ? "true" : "false") << std::endl);
        if (!ok) {
            std::cerr << "[ENGINE] Raft replicate failed" << std::endl;
            throw std::runtime_error("write_not_committed");
        }

        // Update shard metrics for auto-split (best-effort)
        try {
            std::string key = doc.value("id", "");
            if (!key.empty()) {
                ShardManager& sm = ShardManager::instance();
                std::string shardId = sm.getShardForKey(key);
                ShardInfo info = sm.getShardInfo(shardId);
                if (!info.shardId.empty()) {
                    info.documentCount += 1;
                    info.sizeBytes += doc.dump().size();

                    double sizeRatio = 0.0;
                    double docRatio = 0.0;
                    size_t sizeThreshold = sm.getShardSizeThreshold();
                    size_t docThreshold = sm.getShardDocCountThreshold();
                    if (sizeThreshold > 0) sizeRatio = static_cast<double>(info.sizeBytes) / static_cast<double>(sizeThreshold);
                    if (docThreshold > 0) docRatio = static_cast<double>(info.documentCount) / static_cast<double>(docThreshold);
                    info.loadFactor = std::max(sizeRatio, docRatio);
                    if (info.loadFactor > 1.0) info.loadFactor = 1.0;

                    sm.updateShard(info);
                }
            }
        } catch (...) {}
        return;
    }

    // delegate to LSM layer (which will write WAL, memtable and flush to SST)
    ELOG("[ENGINE] Using LSM::put for insert\n");
    auto memtableStart = std::chrono::steady_clock::now();
    LSM::put(userId, dbName, collection, doc);
    auto memtableEnd = std::chrono::steady_clock::now();
    uint64_t memtableUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(memtableEnd - memtableStart).count());
    ELOG("[ENGINE] LSM::put completed: " << memtableUs << " microseconds\n");
    pacificdb::timing::recordStage(pacificdb::timing::Stage::MemtableInsert, memtableUs);
    updateReadFenceFromDoc(userId, dbName, collection, doc);

    // Update shard metrics for auto-split (best-effort)
    try {
        std::string key = doc.value("id", "");
        if (!key.empty()) {
            ShardManager& sm = ShardManager::instance();
            std::string shardId = sm.getShardForKey(key);
            ShardInfo info = sm.getShardInfo(shardId);
            if (!info.shardId.empty()) {
                info.documentCount += 1;
                info.sizeBytes += doc.dump().size();

                double sizeRatio = 0.0;
                double docRatio = 0.0;
                size_t sizeThreshold = sm.getShardSizeThreshold();
                size_t docThreshold = sm.getShardDocCountThreshold();
                if (sizeThreshold > 0) sizeRatio = static_cast<double>(info.sizeBytes) / static_cast<double>(sizeThreshold);
                if (docThreshold > 0) docRatio = static_cast<double>(info.documentCount) / static_cast<double>(docThreshold);
                info.loadFactor = std::max(sizeRatio, docRatio);
                if (info.loadFactor > 1.0) info.loadFactor = 1.0;

                sm.updateShard(info);
            }
        }
    } catch (...) {}
}

json DatabaseEngine::insertMany(const std::string& userId,
                                const std::string& dbName,
                                const std::string& collection,
                                std::vector<json> docs,
                                const json& raftMeta) {
    requireStorageNamespace(userId, dbName, collection);
    StoragePipelineScope storageScope;
    auto totalStart = std::chrono::steady_clock::now();
    json result = {
        {"status", "ok"},
        {"mode", "batch"},
        {"inserted", 0},
        {"failed", 0},
        {"timings_us", {
            {"validation", 0},
            {"raft_bypass", 0},
            {"replication", 0},
            {"lsm_total", 0},
            {"shard_metrics", 0},
            {"total", 0}
        }}
    };

    if (docs.empty()) {
        result["total"] = 0;
        return result;
    }

    auto validationStart = std::chrono::steady_clock::now();
    std::vector<json> prepared;
    prepared.reserve(docs.size());
    for (auto& doc : docs) {
        if (!doc.is_object()) {
            result["failed"] = result.value("failed", 0) + 1;
            continue;
        }
        if (!doc.contains("id") || doc["id"].is_null() ||
            (doc["id"].is_string() && doc["id"].get<std::string>().empty())) {
            doc["id"] = IDGenerator::generateObjectId();
        }
        enforceTenantOnWrite(userId, doc);
        stampMvccWrite(doc);
        prepared.push_back(std::move(doc));
    }
    auto validationEnd = std::chrono::steady_clock::now();
    result["timings_us"]["validation"] =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(validationEnd - validationStart).count());
    const uint64_t batchEstimatedBytes = static_cast<uint64_t>(json(prepared).dump().size());
    result["estimated_batch_bytes"] = batchEstimatedBytes;

    if (prepared.empty()) {
        result["status"] = "error";
        result["error"] = "no valid documents";
        result["total"] = docs.size();
        return result;
    }

    if (!DatabaseEngine::recoveryReplayMode() && MemoryManager::shouldSlowDownWrites()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    fs::path dbp = basePath(userId, dbName);
    if (!fs::exists(dbp)) {
        throw std::runtime_error("[ENGINE] DB does not exist. InsertMany blocked. user=" + userId + " db=" + dbName + " path=" + dbp.string());
    }

    bool isMainUsers = (dbName == "system" && collection == "users");
    if (isMainUsers) {
        result["mode"] = "system-users-fallback";
        for (auto& doc : prepared) {
            DatabaseEngine::insert(userId, dbName, collection, doc, raftMeta);
        }
        result["inserted"] = prepared.size();
        result["total"] = docs.size();
        auto totalEnd = std::chrono::steady_clock::now();
        result["timings_us"]["total"] =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(totalEnd - totalStart).count());
        return result;
    }

    bool raftEnabled = RaftCore::instance().isEnabled();
    bool raftBypassed = standaloneRaftBypassEnabled();
    result["standalone_raft_bypass"] = raftBypassed;
    if (raftEnabled && !raftBypassed) {
        if (!RaftCore::instance().isLeader()) {
            throw std::runtime_error("not_leader");
        }

        json entry = {
            {"op", "INSERT_MANY"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"data", prepared}
        };
        appendRaftWriteMeta(entry, raftMeta);

        auto replicationStart = std::chrono::steady_clock::now();
        bool ok = RaftCore::instance().replicateAndApply(entry);
        auto replicationEnd = std::chrono::steady_clock::now();
        const uint64_t replicationUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(replicationEnd - replicationStart).count());
        pacificdb::timing::recordStage(pacificdb::timing::Stage::Replication, replicationUs);
        result["timings_us"]["replication"] = replicationUs;
        result["mode"] = "raft-batch";
        if (!ok) {
            throw std::runtime_error("write_not_committed");
        }
    } else {
        auto bypassStart = std::chrono::steady_clock::now();
        if (raftEnabled && raftBypassed) {
            result["mode"] = "standalone-raft-bypass";
        }
        auto bypassEnd = std::chrono::steady_clock::now();
        result["timings_us"]["raft_bypass"] =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(bypassEnd - bypassStart).count());

        auto lsmStart = std::chrono::steady_clock::now();
        json lsm = LSM::putMany(userId, dbName, collection, prepared);
        auto lsmEnd = std::chrono::steady_clock::now();
        result["lsm"] = lsm;
        result["timings_us"]["lsm_total"] =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(lsmEnd - lsmStart).count());
    }

    for (const auto& doc : prepared) {
        updateReadFenceFromDoc(userId, dbName, collection, doc);
    }

    // Update shard metrics for auto-split (best-effort, aggregated by shard).
    auto shardMetricsStart = std::chrono::steady_clock::now();
    try {
        ShardManager& sm = ShardManager::instance();
        std::string shardKey;
        for (const auto& doc : prepared) {
            shardKey = doc.value("id", "");
            if (!shardKey.empty()) break;
        }
        if (!shardKey.empty()) {
            std::string shardId = sm.getShardForKey(shardKey);
            ShardInfo info = sm.getShardInfo(shardId);
            if (!info.shardId.empty()) {
                info.documentCount += prepared.size();
                info.sizeBytes += batchEstimatedBytes;
                double sizeRatio = 0.0;
                double docRatio = 0.0;
                size_t sizeThreshold = sm.getShardSizeThreshold();
                size_t docThreshold = sm.getShardDocCountThreshold();
                if (sizeThreshold > 0) sizeRatio = static_cast<double>(info.sizeBytes) / static_cast<double>(sizeThreshold);
                if (docThreshold > 0) docRatio = static_cast<double>(info.documentCount) / static_cast<double>(docThreshold);
                info.loadFactor = std::max(sizeRatio, docRatio);
                if (info.loadFactor > 1.0) info.loadFactor = 1.0;
                sm.updateShard(info);
            }
        }
    } catch (...) {}
    auto shardMetricsEnd = std::chrono::steady_clock::now();
    result["timings_us"]["shard_metrics"] =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(shardMetricsEnd - shardMetricsStart).count());

    result["inserted"] = prepared.size();
    result["failed"] = result.value("failed", 0);
    result["total"] = docs.size();
    auto totalEnd = std::chrono::steady_clock::now();
    result["timings_us"]["total"] =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(totalEnd - totalStart).count());
    result["timings_ms"] = {
        {"raftBypassMs", result["timings_us"].value("raft_bypass", 0ULL) / 1000.0},
        {"walAppendMs", result.contains("lsm") ? result["lsm"].value("timings_us", json::object()).value("wal_append", 0ULL) / 1000.0 : 0.0},
        {"walFsyncMs", 0.0},
        {"memtableWriteMs", result.contains("lsm") ? result["lsm"].value("timings_us", json::object()).value("memtable_write", 0ULL) / 1000.0 : 0.0},
        {"indexUpdateMs", result.contains("lsm") ? result["lsm"].value("timings_us", json::object()).value("index_update", 0ULL) / 1000.0 : 0.0},
        {"flushWaitMs", result.contains("lsm") ? result["lsm"].value("timings_us", json::object()).value("flush_wait", 0ULL) / 1000.0 : 0.0},
        {"shardMetricsMs", result["timings_us"].value("shard_metrics", 0ULL) / 1000.0},
        {"totalMs", result["timings_us"].value("total", 0ULL) / 1000.0}
    };
    return result;
}

/* ---------------- INSERT VECTOR ---------------- */
void DatabaseEngine::insertVector(const std::string& userId,
                                  const std::string& dbName,
                                  const std::string& collection,
                                  const json& doc,
                                  const json& raftMeta) {
    requireStorageNamespace(userId, dbName, collection);
    if (!doc.contains("vector") || !doc["vector"].is_array() || doc["vector"].empty()) {
        throw std::invalid_argument("vector must be a non-empty numeric array");
    }
    for (const auto& value : doc["vector"]) {
        if (!value.is_number() || !std::isfinite(value.get<double>())) {
            throw std::invalid_argument("vector must be a non-empty numeric array");
        }
    }
    json toStore = doc;
    toStore["kind"] = "vector";
    insert(userId, dbName, collection, std::move(toStore), raftMeta);
    invalidateVectorIndex(userId, dbName, collection);
}

static size_t parseEnvBytes(const char* env, size_t fallback) {
    if (!env) return fallback;
    try {
        long long value = std::stoll(env);
        if (value > 0) return static_cast<size_t>(value);
    } catch (...) {}
    return fallback;
}

static bool isTruthyEnv(const char* v) {
    if (!v) return false;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

static bool mediaIntegrityEnforced() {
    static const bool enabled = [] {
        const char* v = std::getenv("MEDIA_INTEGRITY_ENFORCE");
        return v ? isTruthyEnv(v) : true;
    }();
    return enabled;
}

static bool mediaRepairEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MEDIA_REPAIR_ON_READ");
        return v ? isTruthyEnv(v) : true;
    }();
    return enabled;
}

static bool mediaManifestReplayEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MEDIA_MANIFEST_REPLAY");
        return v ? isTruthyEnv(v) : true;
    }();
    return enabled;
}

static bool mediaManifestRebuildEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MEDIA_MANIFEST_REBUILD");
        return v ? isTruthyEnv(v) : true;
    }();
    return enabled;
}

static bool mediaOrphanCleanupEnabled() {
    static const bool enabled = isTruthyEnv(std::getenv("MEDIA_ORPHAN_CLEANUP"));
    return enabled;
}

static bool mediaManifestWalEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MEDIA_MANIFEST_WAL");
        return v ? isTruthyEnv(v) : true;
    }();
    return enabled;
}

static std::string resolveReplicaHash() {
    const char* nodeEnv = std::getenv("RAFT_NODE_ID");
    const char* hostEnv = std::getenv("HOSTNAME");
    std::string seed = nodeEnv ? std::string(nodeEnv) : std::string("node-0");
    if (hostEnv && *hostEnv) {
        seed.append("|");
        seed.append(hostEnv);
    }
    return ChecksumCalculator::sha256(seed.data(), seed.size());
}

static fs::path mediaManifestPath(const fs::path& dir) {
    return dir / "manifest.json";
}

static fs::path mediaManifestWalPath(const std::string& userId, const std::string& dbName) {
    return basePath(userId, dbName) / "wal" / "media_manifest.wal";
}

static std::vector<fs::path> resolveReplicaRoots() {
    std::vector<fs::path> roots;
    const char* v = std::getenv("MEDIA_REPLICA_ROOTS");
    if (!v) return roots;
    std::string raw(v);
    size_t start = 0;
    while (start < raw.size()) {
        size_t comma = raw.find(',', start);
        std::string token = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!token.empty()) roots.push_back(fs::path(token));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return roots;
}

static std::vector<fs::path> resolveReplicaChunkDirs(const fs::path& localDir) {
    std::vector<fs::path> dirs;
    std::vector<fs::path> roots = resolveReplicaRoots();
    if (roots.empty()) return dirs;
    std::error_code ec;
    fs::path dataRoot = fs::path(DATA_ROOT);
    fs::path rel = fs::relative(localDir, dataRoot, ec);
    if (ec) return dirs;
    for (const auto& root : roots) {
        dirs.push_back(root / rel);
    }
    return dirs;
}

struct MediaChunkMeta {
    size_t index = 0;
    size_t size = 0;
    std::string checksum;
};

struct MediaReadOutcome {
    std::vector<unsigned char> data;
    bool ok = true;
    bool repaired = false;
    size_t failedChunk = static_cast<size_t>(-1);
    std::string error;
};

static bool writeManifestAtomic(const fs::path& dir, const json& manifest) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    fs::path tmpPath = dir / "manifest.json.tmp";
    fs::path finalPath = mediaManifestPath(dir);
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    const std::string payload = manifest.dump();
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();
    out.close();
    if (ec) return false;
    fs::rename(tmpPath, finalPath, ec);
    return !ec;
}

static bool loadManifest(const fs::path& dir, json& manifest) {
    fs::path path = mediaManifestPath(dir);
    if (!fs::exists(path)) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::string payload((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (payload.empty()) return false;
    try {
        manifest = json::parse(payload);
        return manifest.is_object();
    } catch (...) {
        return false;
    }
}

static bool extractChecksumsFromManifest(const json& manifest,
                                         std::vector<std::string>& checksums,
                                         std::vector<size_t>& chunkSizes) {
    if (!manifest.is_object() || !manifest.contains("chunks") || !manifest["chunks"].is_array()) {
        return false;
    }
    checksums.clear();
    chunkSizes.clear();
    for (const auto& c : manifest["chunks"]) {
        if (!c.is_object()) continue;
        std::string checksum = c.value("checksum", "");
        size_t size = c.value("size", static_cast<size_t>(0));
        checksums.push_back(checksum);
        chunkSizes.push_back(size);
    }
    return !checksums.empty();
}

static json buildMediaManifest(const std::string& userId,
                               const std::string& dbName,
                               const std::string& collection,
                               const std::string& mediaId,
                               size_t chunkBytes,
                               size_t totalSize,
                               const std::vector<MediaChunkMeta>& chunks) {
    json manifest = json::object();
    manifest["manifestVersion"] = 1;
    manifest["mediaId"] = mediaId;
    manifest["userId"] = userId;
    manifest["dbName"] = dbName;
    manifest["collection"] = collection;
    manifest["chunkSize"] = chunkBytes;
    manifest["chunkCount"] = chunks.size();
    manifest["sizeBytes"] = totalSize;
    manifest["replicaHash"] = resolveReplicaHash();
    manifest["compression"] = json{{"algorithm", "none"}, {"level", 0}};

    json chunkArray = json::array();
    for (const auto& chunk : chunks) {
        json entry = json::object();
        entry["index"] = chunk.index;
        entry["size"] = chunk.size;
        entry["checksum"] = chunk.checksum;
        entry["chunkId"] = mediaId + ":" + std::to_string(chunk.index);
        entry["version"] = 1;
        entry["replicaHash"] = manifest["replicaHash"];
        entry["compression"] = manifest["compression"];
        chunkArray.push_back(entry);
    }
    manifest["chunks"] = chunkArray;

    std::string manifestPayload = manifest.dump();
    manifest["manifestChecksum"] = ChecksumCalculator::sha256(manifestPayload.data(), manifestPayload.size());
    return manifest;
}

static bool computeChunkChecksum(const fs::path& path, std::string& outChecksum) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty() && fs::file_size(path) > 0) return false;
    outChecksum = ChecksumCalculator::sha256(data.data(), data.size());
    return true;
}

static bool attemptReplicaRepair(const fs::path& localDir,
                                 size_t chunkIndex,
                                 const std::string& expectedChecksum,
                                 std::string& repairSource) {
    std::vector<fs::path> replicas = resolveReplicaChunkDirs(localDir);
    if (replicas.empty()) return false;
    for (const auto& replicaDir : replicas) {
        fs::path candidate = replicaDir / ("chunk-" + std::to_string(chunkIndex) + ".bin");
        if (!fs::exists(candidate)) continue;
        std::string checksum;
        if (!computeChunkChecksum(candidate, checksum)) continue;
        if (checksum != expectedChecksum) continue;
        fs::path localChunk = localDir / ("chunk-" + std::to_string(chunkIndex) + ".bin");
        std::error_code ec;
        fs::copy_file(candidate, localChunk, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            repairSource = candidate.string();
            return true;
        }
    }
    return false;
}

static void logMediaManifestWal(const std::string& userId,
                                const std::string& dbName,
                                const std::string& collection,
                                const std::string& mediaId,
                                const fs::path& dir,
                                const json& manifest) {
    if (!mediaManifestWalEnabled()) return;
    json entry = json::object();
    entry["op"] = "MEDIA_MANIFEST";
    entry["userId"] = userId;
    entry["db"] = dbName;
    entry["collection"] = collection;
    entry["mediaId"] = mediaId;
    entry["diskPath"] = dir.string();
    entry["manifest"] = manifest;
    WAL::log(mediaManifestWalPath(userId, dbName).string(), entry);
}

static size_t resolveMediaChunkBytes() {
    static const size_t value = [] {
        const char* chunkMbEnv = std::getenv("MAX_STREAM_CHUNK_MB");
        if (chunkMbEnv) {
            try {
                const double mb = std::stod(chunkMbEnv);
                if (mb > 0.0) return static_cast<size_t>(mb * 1024.0 * 1024.0);
            } catch (...) {}
        }
        return parseEnvBytes(std::getenv("MEDIA_CHUNK_BYTES"), 4 * 1024 * 1024);
    }();
    return value;
}

static size_t resolveMediaInlineMaxBytes() {
    static const size_t value = parseEnvBytes(
        std::getenv("MEDIA_INLINE_MAX_BYTES"),
        std::max<size_t>(256 * 1024, resolveMediaChunkBytes()));
    return value;
}

static size_t resolveMediaMaxBytes() {
    static const size_t value = [] {
        if (const char* v = std::getenv("MEDIA_MAX_BYTES")) {
            return parseEnvBytes(v, 256 * 1024 * 1024);
        }
        if (const char* v = std::getenv("MAX_REQUEST_SIZE_MB")) {
            try {
                const double mb = std::stod(v);
                if (mb > 0.0) return static_cast<size_t>(mb * 1024.0 * 1024.0);
            } catch (...) {}
        }
        return size_t{256 * 1024 * 1024};
    }();
    return value;
}

static fs::path mediaChunkDir(const std::string& userId,
                              const std::string& dbName,
                              const std::string& collection,
                              const std::string& mediaId) {
    requireStorageNamespace(userId, dbName, collection);
    validateStorageIdentifier(mediaId, "mediaId");
    return validateContainedStoragePath(
        fs::path(DATA_ROOT),
        basePath(userId, dbName) / "media" / collection / mediaId);
}

static bool writeMediaChunks(const fs::path& dir,
                             const std::vector<unsigned char>& binaryData,
                             size_t chunkBytes,
                             size_t& outChunkCount,
                             std::vector<MediaChunkMeta>& outChunks,
                             fs::path& outStagingDir) {
    if (chunkBytes == 0) return false;
    std::error_code ec;
    outChunks.clear();
    outStagingDir = dir;
    outStagingDir += ".tmp";
    fs::remove_all(outStagingDir, ec);
    ec.clear();
    fs::create_directories(outStagingDir, ec);
    if (ec) return false;

    outChunkCount = (binaryData.size() + chunkBytes - 1) / chunkBytes;
    outChunks.reserve(outChunkCount);
    for (size_t i = 0; i < outChunkCount; ++i) {
        size_t offset = i * chunkBytes;
        size_t remaining = binaryData.size() - offset;
        size_t len = std::min(chunkBytes, remaining);
        fs::path chunkPath = outStagingDir / ("chunk-" + std::to_string(i) + ".bin");
        std::ofstream out(chunkPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(reinterpret_cast<const char*>(binaryData.data() + offset), static_cast<std::streamsize>(len));
        if (!out) return false;

        MediaChunkMeta meta;
        meta.index = i;
        meta.size = len;
        meta.checksum = ChecksumCalculator::sha256(binaryData.data() + offset, len);
        outChunks.push_back(meta);
    }
    return true;
}

static std::vector<unsigned char> sliceBinary(const std::vector<unsigned char>& data,
                                              size_t offset,
                                              size_t length) {
    if (offset >= data.size()) return {};
    size_t end = (length > 0) ? std::min(data.size(), offset + length) : data.size();
    return std::vector<unsigned char>(data.begin() + offset, data.begin() + end);
}

static MediaReadOutcome readChunkRange(const fs::path& dir,
                                       size_t chunkCount,
                                       size_t chunkBytes,
                                       size_t totalSize,
                                       size_t offset,
                                       size_t length,
                                       const std::vector<std::string>* checksums,
                                       const std::vector<size_t>* chunkSizes,
                                       bool enforceIntegrity,
                                       bool attemptRepair,
                                       json* manifest) {
    MediaReadOutcome outcome;
    if (chunkCount == 0 || chunkBytes == 0 || totalSize == 0) return outcome;
    if (offset >= totalSize) return outcome;

    if (enforceIntegrity && checksums && checksums->size() < chunkCount) {
        outcome.ok = false;
        outcome.error = "media_manifest_incomplete";
        return outcome;
    }

    size_t endPos = length > 0 ? std::min(totalSize, offset + length) : totalSize;
    if (endPos <= offset) return outcome;

    size_t startChunk = offset / chunkBytes;
    size_t endChunk = (endPos - 1) / chunkBytes;

    outcome.data.reserve(endPos - offset);

    for (size_t chunkIndex = startChunk; chunkIndex <= endChunk; ++chunkIndex) {
        fs::path chunkPath = dir / ("chunk-" + std::to_string(chunkIndex) + ".bin");
        if (!fs::exists(chunkPath)) {
            outcome.ok = false;
            outcome.failedChunk = chunkIndex;
            outcome.error = "media_chunk_missing";
            return outcome;
        }

        bool validate = enforceIntegrity && checksums && chunkIndex < checksums->size();
        std::vector<unsigned char> chunkData;

        if (validate) {
            std::ifstream in(chunkPath, std::ios::binary);
            if (!in.is_open()) {
                outcome.ok = false;
                outcome.failedChunk = chunkIndex;
                outcome.error = "media_chunk_open_failed";
                return outcome;
            }
            chunkData.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!chunkData.empty() || fs::file_size(chunkPath) == 0) {
                std::string expected = (*checksums)[chunkIndex];
                std::string actual = ChecksumCalculator::sha256(chunkData.data(), chunkData.size());
                bool checksumOk = expected.empty() || actual == expected;

                if (chunkSizes && chunkIndex < chunkSizes->size()) {
                    size_t expectedSize = (*chunkSizes)[chunkIndex];
                    if (expectedSize > 0 && expectedSize != chunkData.size()) {
                        checksumOk = false;
                    }
                }

                if (!checksumOk) {
                    std::string repairSource;
                    bool repaired = false;
                    if (attemptRepair && !expected.empty()) {
                        repaired = attemptReplicaRepair(dir, chunkIndex, expected, repairSource);
                        if (repaired) {
                            outcome.repaired = true;
                            std::ifstream rin(chunkPath, std::ios::binary);
                            chunkData.assign(std::istreambuf_iterator<char>(rin), std::istreambuf_iterator<char>());
                            std::string repairedChecksum = ChecksumCalculator::sha256(chunkData.data(), chunkData.size());
                            if (repairedChecksum != expected) {
                                repaired = false;
                            }
                        }
                    }

                    if (!repaired) {
                        outcome.ok = false;
                        outcome.failedChunk = chunkIndex;
                        outcome.error = "media_checksum_mismatch";
                        if (manifest && manifest->is_object()) {
                            json entry = json::object();
                            entry["chunk"] = chunkIndex;
                            entry["expected"] = expected;
                            entry["actual"] = actual;
                            entry["timestamp_ms"] = nowMs();
                            entry["repaired"] = false;
                            if (!manifest->contains("corruptions")) (*manifest)["corruptions"] = json::array();
                            (*manifest)["corruptions"].push_back(entry);
                            writeManifestAtomic(dir, *manifest);
                        }
                        return outcome;
                    }

                    if (manifest && manifest->is_object()) {
                        json entry = json::object();
                        entry["chunk"] = chunkIndex;
                        entry["expected"] = expected;
                        entry["timestamp_ms"] = nowMs();
                        entry["repaired"] = true;
                        if (!manifest->contains("repairs")) (*manifest)["repairs"] = json::array();
                        (*manifest)["repairs"].push_back(entry);
                        writeManifestAtomic(dir, *manifest);
                    }
                }
            }
        }

        size_t chunkOffset = (chunkIndex == startChunk) ? (offset % chunkBytes) : 0;
        size_t chunkEnd = (chunkIndex == endChunk) ? (endPos % chunkBytes) : chunkBytes;
        if (chunkIndex == endChunk && chunkEnd == 0) chunkEnd = chunkBytes;
        if (chunkEnd < chunkOffset) chunkEnd = chunkOffset;
        size_t toRead = chunkEnd - chunkOffset;

        if (toRead > 0) {
            if (validate) {
                size_t safeEnd = std::min(chunkData.size(), chunkEnd);
                size_t safeOffset = std::min(chunkData.size(), chunkOffset);
                if (safeEnd < safeOffset) safeEnd = safeOffset;
                size_t sliceLen = safeEnd - safeOffset;
                outcome.data.insert(outcome.data.end(),
                                    chunkData.begin() + static_cast<std::ptrdiff_t>(safeOffset),
                                    chunkData.begin() + static_cast<std::ptrdiff_t>(safeOffset + sliceLen));
            } else {
                std::ifstream in(chunkPath, std::ios::binary);
                if (!in.is_open()) {
                    outcome.ok = false;
                    outcome.failedChunk = chunkIndex;
                    outcome.error = "media_chunk_open_failed";
                    return outcome;
                }
                in.seekg(static_cast<std::streamoff>(chunkOffset), std::ios::beg);
                std::vector<unsigned char> buffer(toRead);
                in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(toRead));
                size_t actuallyRead = static_cast<size_t>(in.gcount());
                buffer.resize(actuallyRead);
                outcome.data.insert(outcome.data.end(), buffer.begin(), buffer.end());
            }
        }
    }

    return outcome;
}

static bool extractChecksumsFromDoc(const json& doc, std::vector<std::string>& checksums) {
    if (!doc.contains("chunkChecksums") || !doc["chunkChecksums"].is_array()) return false;
    checksums.clear();
    for (const auto& c : doc["chunkChecksums"]) {
        if (c.is_string()) checksums.push_back(c.get<std::string>());
    }
    return !checksums.empty();
}

static bool rebuildManifestFromDisk(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection,
                                    const std::string& mediaId,
                                    const fs::path& dir,
                                    size_t chunkCount,
                                    size_t chunkBytes,
                                    size_t totalSize,
                                    std::vector<std::string>& checksums,
                                    std::vector<size_t>& chunkSizes,
                                    json& manifest) {
    if (!fs::exists(dir)) return false;
    std::vector<MediaChunkMeta> chunks;
    chunks.reserve(chunkCount);
    checksums.clear();
    chunkSizes.clear();

    for (size_t i = 0; i < chunkCount; ++i) {
        fs::path chunkPath = dir / ("chunk-" + std::to_string(i) + ".bin");
        if (!fs::exists(chunkPath)) return false;
        size_t size = static_cast<size_t>(fs::file_size(chunkPath));
        std::string checksum;
        if (!computeChunkChecksum(chunkPath, checksum)) return false;
        chunks.push_back(MediaChunkMeta{i, size, checksum});
        checksums.push_back(checksum);
        chunkSizes.push_back(size);
    }

    manifest = buildMediaManifest(userId, dbName, collection, mediaId, chunkBytes, totalSize, chunks);
    if (!writeManifestAtomic(dir, manifest)) return false;
    logMediaManifestWal(userId, dbName, collection, mediaId, dir, manifest);
    return true;
}

static bool loadMediaChecksums(const json& doc,
                               const std::string& userId,
                               const std::string& dbName,
                               const std::string& collection,
                               const std::string& mediaId,
                               const fs::path& dir,
                               size_t chunkCount,
                               size_t chunkBytes,
                               size_t totalSize,
                               std::vector<std::string>& checksums,
                               std::vector<size_t>& chunkSizes,
                               json& manifest,
                               std::string& error) {
    if (extractChecksumsFromDoc(doc, checksums)) {
        chunkSizes.clear();
        chunkSizes.reserve(chunkCount);
        for (size_t i = 0; i < chunkCount; ++i) {
            size_t expected = (i + 1 == chunkCount) ? (totalSize - (i * chunkBytes)) : chunkBytes;
            chunkSizes.push_back(expected);
        }
        if (mediaManifestRebuildEnabled() && !fs::exists(mediaManifestPath(dir)) && checksums.size() == chunkCount) {
            std::vector<MediaChunkMeta> chunks;
            chunks.reserve(chunkCount);
            for (size_t i = 0; i < chunkCount; ++i) {
                MediaChunkMeta meta;
                meta.index = i;
                meta.size = chunkSizes[i];
                meta.checksum = checksums[i];
                chunks.push_back(meta);
            }
            manifest = buildMediaManifest(userId, dbName, collection, mediaId, chunkBytes, totalSize, chunks);
            writeManifestAtomic(dir, manifest);
            logMediaManifestWal(userId, dbName, collection, mediaId, dir, manifest);
        }
        return true;
    }

    if (loadManifest(dir, manifest)) {
        if (extractChecksumsFromManifest(manifest, checksums, chunkSizes)) {
            return true;
        }
    }

    if (mediaManifestRebuildEnabled()) {
        if (rebuildManifestFromDisk(userId, dbName, collection, mediaId, dir,
                                    chunkCount, chunkBytes, totalSize,
                                    checksums, chunkSizes, manifest)) {
            return true;
        }
    }

    error = "media_manifest_missing";
    return false;
}

static void replayMediaManifestWals() {
    if (!mediaManifestReplayEnabled()) return;
    std::error_code ec;
    for (const auto& userDir : fs::directory_iterator(DATA_ROOT, ec)) {
        if (ec || !userDir.is_directory()) continue;
        const std::string userId = userDir.path().filename().string();
        for (const auto& dbDir : fs::directory_iterator(userDir.path(), ec)) {
            if (ec || !dbDir.is_directory()) continue;
            const std::string dbName = dbDir.path().filename().string();
            fs::path walPath = dbDir.path() / "wal" / "media_manifest.wal";
            if (!fs::exists(walPath)) continue;
            auto entries = WAL::readAll(walPath.string());
            for (const auto& raw : entries) {
                try {
                    json entry = json::parse(raw);
                    if (!entry.is_object()) continue;
                    if (entry.value("op", "") != "MEDIA_MANIFEST") continue;
                    if (!entry.contains("manifest") || !entry["manifest"].is_object()) continue;
                    std::string collection = entry.value("collection", "");
                    std::string mediaId = entry.value("mediaId", "");
                    fs::path dir;
                    if (entry.contains("diskPath") && entry["diskPath"].is_string()) {
                        dir = fs::path(entry["diskPath"].get<std::string>());
                    } else if (!collection.empty() && !mediaId.empty()) {
                        dir = mediaChunkDir(userId, dbName, collection, mediaId);
                    }
                    if (dir.empty()) continue;
                    if (fs::exists(mediaManifestPath(dir))) continue;
                    writeManifestAtomic(dir, entry["manifest"]);
                } catch (...) {
                    continue;
                }
            }
        }
    }
}

static void cleanupOrphanMediaChunks() {
    if (!mediaOrphanCleanupEnabled()) return;
    std::error_code ec;
    for (const auto& userDir : fs::directory_iterator(DATA_ROOT, ec)) {
        if (ec || !userDir.is_directory()) continue;
        const std::string userId = userDir.path().filename().string();
        for (const auto& dbDir : fs::directory_iterator(userDir.path(), ec)) {
            if (ec || !dbDir.is_directory()) continue;
            const std::string dbName = dbDir.path().filename().string();
            fs::path mediaRoot = dbDir.path() / "media";
            if (!fs::exists(mediaRoot)) continue;
            for (const auto& collDir : fs::directory_iterator(mediaRoot, ec)) {
                if (ec || !collDir.is_directory()) continue;
                const std::string collection = collDir.path().filename().string();

                std::unordered_set<std::string> validIds;
                try {
                    auto docs = LSM::getAll(userId, dbName, collection);
                    for (const auto& doc : docs) {
                        if (doc.contains("_deleted") && doc["_deleted"].is_boolean() && doc["_deleted"].get<bool>()) continue;
                        if (doc.value("kind", "") != "media") continue;
                        std::string id = doc.value("id", "");
                        if (!id.empty()) validIds.insert(id);
                    }
                } catch (...) {}

                for (const auto& mediaDir : fs::directory_iterator(collDir.path(), ec)) {
                    if (ec || !mediaDir.is_directory()) continue;
                    std::string mediaId = mediaDir.path().filename().string();
                    if (mediaId.size() >= 4 && mediaId.rfind(".tmp") == mediaId.size() - 4) {
                        std::error_code rmEc;
                        fs::remove_all(mediaDir.path(), rmEc);
                        continue;
                    }
                    if (validIds.find(mediaId) == validIds.end()) {
                        std::error_code rmEc;
                        fs::remove_all(mediaDir.path(), rmEc);
                    }
                }
            }
        }
    }
}

/* ---------------- INSERT MEDIA ---------------- */
static void collectQueryRootFields(const QueryNode& query,
                                   std::unordered_set<std::string>& fields) {
    if (!query.field.empty()) {
        const auto dot = query.field.find('.');
        fields.insert(query.field.substr(0, dot));
    }
    for (const auto& child : query.children) collectQueryRootFields(child, fields);
}

static json projectLatestRow(const LSM::LatestRowView& row,
                             const std::unordered_set<std::string>& queryFields) {
    json projected = json::object();
    for (const auto& field : queryFields) {
        if (auto value = row.field(field)) projected[field] = std::move(*value);
    }
    for (const char* field : {"id", "tenant_id", "_deleted", "_visibility_state",
                              "committed", "_raft_commit_index", "_visibility_floor",
                              "version", "_mvcc_commit_ms"}) {
        if (!projected.contains(field)) {
            if (auto value = row.field(field)) projected[field] = std::move(*value);
        }
    }
    return projected;
}

/* ---------------- FIND (OPTIMIZED) ----------------*/
std::vector<json> DatabaseEngine::find(const std::string& userId,
                                       const std::string& dbName,
                                       const std::string& collection,
                                       const json& filter,
                                       long long limit,
                                       long long offset) {
    requireStorageNamespace(userId, dbName, collection);
    static const bool isFindDeepLog = isTruthyEnv(std::getenv("FIND_DEEP_LOG"));
    auto findLog = [&](const std::string& msg) {
        if (isFindDeepLog) {
            std::lock_guard<std::mutex> lk(g_findLogMutex);
            std::cerr << msg << std::endl;
        }
    };

    auto findStep = [&](int step, const std::string& msg) {
        findLog("[FIND] " + std::to_string(step) + ": " + msg);
    };

    findStep(1, "start");

    static const bool dummyFind = isTruthyEnv(std::getenv("FIND_DUMMY_RESULT"));
    if (dummyFind) {
        findStep(8, "before return (dummy mode)");
        return { json{{"ok", true}} };
    }

    static const bool bypassRaftFind = isTruthyEnv(std::getenv("BYPASS_RAFT_FIND"));
    if (bypassRaftFind) {
        findLog("[FIND] BYPASS_RAFT_FIND=1 (reads will not wait for any consensus path)");
    }

    static const bool readLocal = isTruthyEnv(std::getenv("READ_LOCAL_FIND"));
    if (readLocal) {
        findLog("[FIND] READ_LOCAL_FIND=1 (reads are served locally by storage path)");
    }

    LSM::clearLastReadVisibilitySource();

    auto queryStart = std::chrono::steady_clock::now();
    // Generate unique query ID for tracking
    std::string queryId = userId + "_" + dbName + "_" + collection + "_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    // Start query tracking
    QueryLimiter::startQuery(queryId);

    try {
    bool isMainUsers = (dbName == "system" && collection == "users");
    std::vector<json> docs;
    std::vector<json> matches;
    size_t scanned = 0;
    size_t maxScan = QueryLimiter::getMaxScanRows();
    size_t maxResults = QueryLimiter::getMaxResultDocs();
    if (limit > 0) maxResults = std::min<size_t>(maxResults, static_cast<size_t>(limit));
    const size_t skipMatches = offset > 0 ? static_cast<size_t>(offset) : 0;
    size_t matchedBeforeOffset = 0;
    bool streamedScan = false;
    const QueryNode parsedQuery = parseQuery(filter);
    std::unordered_set<std::string> queryFields;
    collectQueryRootFields(parsedQuery, queryFields);

    ELOG("[ENGINE][FIND] userId=" << userId << " db=" << dbName
              << " col=" << collection << " filter=" << filter.dump() << "\n");

    auto resolveShardKey = [&]() -> std::string {
        if (filter.is_object()) {
            if (filter.contains("id") && filter["id"].is_string()) {
                return filter["id"].get<std::string>();
            }
            if (filter.contains("keyPrefix") && filter["keyPrefix"].is_string()) {
                return filter["keyPrefix"].get<std::string>();
            }
            if (filter.size() == 1) {
                auto it = filter.begin();
                if (it.value().is_string()) {
                    return it.value().get<std::string>();
                }
            }
        }
        return collection;
    };

    auto recordQueryLoad = [&]() {
        try {
            auto end = std::chrono::steady_clock::now();
            double latencyMs = std::chrono::duration_cast<std::chrono::microseconds>(end - queryStart).count() / 1000.0;
            std::string key = resolveShardKey();
            if (!key.empty()) {
                ShardManager& sm = ShardManager::instance();
                std::string shardId = sm.getShardForKey(key);
                sm.updateShardQueryLoad(shardId, latencyMs);
            }
        } catch (...) {}
    };

    if (isMainUsers) {
        findStep(6, "before disk read (system users)");
        LSM::setLastReadVisibilitySource("SST_L0");
        // 🔍 System users always stored in default location (not user-specific)
        fs::path file = basePath("system", "system") / "data" / (collection + ".bin");
        ELOG("[ENGINE][FIND] Reading system users from: " << file.string() << "\n");
        docs = Storage::readAll(file.string());
        ELOG("[ENGINE][FIND] Found " << docs.size() << " total documents\n");
        findStep(7, "after disk read (system users)");
    } else {
        // ⚡ OPTIMIZATION: Try fast index-based lookup for simple equality filters
        const long long readFloorVersion = resolveReadFloorVersionFromContext();
        const bool strictVisibilityRead = g_readContext.consistency == "strong" || readFloorVersion >= 0;
        // Pick the best field for index lookup: for compound filters, use the first
        // simple equality field; remaining conditions are applied in the match loop below.
        std::string fastField;
        json fastValue;
        if (filter.is_object() && !filter.empty()) {
            for (auto it = filter.begin(); it != filter.end(); ++it) {
                if (!it.value().is_object() && !it.value().is_array()) {
                    fastField = it.key();
                    fastValue = it.value();
                    break;
                }
            }
        }

        // Column indexes currently address top-level fields. Treating a dotted
        // path as an indexed lookup makes an empty index result authoritative
        // and incorrectly hides documents that the normal query evaluator can
        // match. Keep dotted predicates on the merged scan until nested column
        // indexes have an explicit on-disk representation.
        if (fastField.find('.') != std::string::npos) fastField.clear();

        bool docsFetched = false;
        if (!fastField.empty()) {
            // Strong reads may use the same visibility-aware index path as eventual
            // reads once this collection's asynchronous index batch is complete.
            // On timeout, retain the existing merged-scan fallback.
            if (strictVisibilityRead && fastField != "id" &&
                !LSM::waitForColumnIndexUpdates(userId, dbName, collection, 30000)) {
                findLog("[FIND] timed out draining pending index updates; falling back to merged scan");
                fastField.clear();
            }
        }
        if (!fastField.empty()) {
            if (strictVisibilityRead) LSM::setSkipIdCache(true);
            std::string strValue = fastValue.is_string() ? fastValue.get<std::string>() : fastValue.dump();
            ELOG("[ENGINE][FIND] index lookup: " << fastField << "=" << strValue << "\n");
            findStep(2, "before lock");
            findStep(4, "before index lookup");
            size_t indexCandidateLimit = 0;
            if (limit > 0) {
                const long long wanted = std::max<long long>(1, limit + std::max<long long>(0, offset));
                indexCandidateLimit = static_cast<size_t>(wanted);
            }
            docs = LSM::findByField(userId, dbName, collection, fastField, strValue, indexCandidateLimit);
            findStep(3, "after lock");
            findStep(5, "after index lookup");
            QueryLimiter::checkTimeout(queryId);
            ELOG("[ENGINE][FIND] index returned " << docs.size() << " candidates\n");
            docsFetched = true;
            // Single-field filter: fast early-return path (no secondary match needed)
            if (filter.size() == 1 && !docs.empty()) {
                std::vector<json> tenantVisible;
                tenantVisible.reserve(docs.size());
                for (const auto& d : docs) {
                    if (!(d.contains("_deleted") && d["_deleted"].is_boolean() && d["_deleted"].get<bool>())
                        && isTenantDocumentVisible(userId, d)
                        && documentVisibleForCurrentRead(d)) {
                        tenantVisible.push_back(d);
                        if (limit > 0 && tenantVisible.size() >= static_cast<size_t>(limit + offset)) {
                            break;
                        }
                    }
                }
                if (!tenantVisible.empty() || g_readContext.readFloorVersion < 0) {
                    if (offset > 0 && static_cast<size_t>(offset) < tenantVisible.size()) {
                        tenantVisible.erase(tenantVisible.begin(), tenantVisible.begin() + offset);
                    } else if (offset > 0) {
                        tenantVisible.clear();
                    }
                    if (limit > 0 && tenantVisible.size() > static_cast<size_t>(limit)) {
                        tenantVisible.resize(static_cast<size_t>(limit));
                    }
                    findStep(8, "before return (fast index path)");
                    QueryLimiter::endQuery(queryId);
                    recordQueryLoad();
                    return tenantVisible;
                }
                findLog("[FIND] fast index hit did not satisfy read floor; falling back to merged scan");
                docsFetched = false; // force re-fetch via getAll below
            }
            // Compound filter: fall through to match loop with narrowed candidate set
        }

        if (!docsFetched) {
            if (strictVisibilityRead) {
                LSM::setLastReadVisibilitySource("MERGED");
                LSM::setSkipIdCache(true);
            }
            ELOG("[ENGINE][FIND] full scan\n");
            findStep(6, "before disk read (full scan)");
            LSM::visitLatest(userId, dbName, collection,
                [&](const LSM::LatestRowView& row) {
                    if (QueryCancel::isCancelled()) return false;
                    if (++scanned > maxScan) {
                        QueryLimiter::reportScanLimitReached(scanned, maxScan);
                        return false;
                    }
                    if (scanned % 100 == 0) QueryLimiter::checkTimeout(queryId);
                    const json projected = projectLatestRow(row, queryFields);
                    if (!isTenantDocumentVisible(userId, projected) ||
                        !documentVisibleForCurrentRead(projected) ||
                        !evalQuery(parsedQuery, projected)) return true;
                    if (matchedBeforeOffset < skipMatches) {
                        ++matchedBeforeOffset;
                        return true;
                    }
                    matches.push_back(row.materialize());
                    if (matches.size() >= maxResults) {
                        QueryLimiter::reportResultLimitReached(matches.size(), maxResults);
                        return false;
                    }
                    return true;
                });
            streamedScan = true;
            findStep(7, "after disk read (full scan)");
        }
    }

    if (!streamedScan) for (auto& d : docs) {
        if (QueryCancel::isCancelled()) {
            std::cerr << "[ENGINE][FIND] Query cancelled by timeout, returning partial results\n";
            break;
        }

        // Track scanned rows and enforce scan limit
        scanned++;

        if (scanned > maxScan) {
            QueryLimiter::reportScanLimitReached(scanned, maxScan);
            break;
        }

        // Check timeout periodically (every 100 docs)
        if (scanned % 100 == 0) {
            QueryLimiter::checkTimeout(queryId);
        }

        // skip tombstones produced by LSM deletes
        if (d.contains("_deleted") && d["_deleted"].is_boolean() && d["_deleted"].get<bool>()) continue;
        if (!isTenantDocumentVisible(userId, d) || !documentVisibleForCurrentRead(d, isMainUsers)) continue;

	        if (evalQuery(parsedQuery, d)) {
	            if (matchedBeforeOffset < skipMatches) {
	                matchedBeforeOffset++;
	                continue;
	            }
	            matches.push_back(d);

	            if (matches.size() >= maxResults) {
	                QueryLimiter::reportResultLimitReached(matches.size(), maxResults);
                break;
            }
        }
    }

    // Check result size before returning
    QueryLimiter::checkResultSize(matches);

    ELOG("[ENGINE][FIND] Matched " << matches.size() << " / "
         << (streamedScan ? scanned : docs.size()));
    findStep(8, "before return");

    QueryLimiter::endQuery(queryId);
    recordQueryLoad();
    return matches;

    } catch (const std::exception& e) {
        // Log and rethrow
        std::cerr << "[ENGINE] Query " << queryId << " failed: " << e.what() << std::endl;
        QueryLimiter::endQuery(queryId);
        throw;
    }
}

size_t DatabaseEngine::count(const std::string& userId,
                             const std::string& dbName,
                             const std::string& collection,
                             const json& filter) {
    requireStorageNamespace(userId, dbName, collection);
    if (dbName == "system" && collection == "users") {
        fs::path file = basePath("system", "system") / "data" / (collection + ".bin");
        size_t total = 0;
        const QueryNode query = parseQuery(filter);
        for (const auto& doc : Storage::readAll(file.string())) {
            if (isTenantDocumentVisible(userId, doc) &&
                documentVisibleForCurrentRead(doc, true) && evalQuery(query, doc)) ++total;
        }
        return total;
    }

    const QueryNode query = parseQuery(filter);
    std::unordered_set<std::string> queryFields;
    collectQueryRootFields(query, queryFields);
    size_t total = 0;
    LSM::visitLatest(userId, dbName, collection, [&](const LSM::LatestRowView& row) {
        const json projected = projectLatestRow(row, queryFields);
        if (isTenantDocumentVisible(userId, projected) &&
            documentVisibleForCurrentRead(projected) && evalQuery(query, projected)) ++total;
        return true;
    });
    return total;
}

/* ---------------- VECTOR QUERY ---------------- */
std::vector<json> DatabaseEngine::queryVector(const std::string& userId,
                                              const std::string& dbName,
                                              const std::string& collection,
                                              const json& query) {
    requireStorageNamespace(userId, dbName, collection);
    auto queryStart = std::chrono::steady_clock::now();
    // query: { vector: [...], k: int, metric: "cosine"|"l2", filter: {...}, modality?: string }
    if (!query.contains("vector") || !query["vector"].is_array() ||
        query["vector"].empty()) {
        throw std::invalid_argument("vector must be a non-empty numeric array");
    }
    for (const auto& value : query["vector"]) {
        if (!value.is_number() || !std::isfinite(value.get<double>())) {
            throw std::invalid_argument("vector must be a non-empty numeric array");
        }
    }
    if (query.contains("k") && !query["k"].is_number_integer()) {
        throw std::invalid_argument("k must be a positive integer");
    }
    std::vector<double> q = jsonToVector(query.value("vector", json::array()));
    int k = query.value("k", 10);
    std::string metric = query.value("metric", std::string("cosine"));
    if (k <= 0) throw std::invalid_argument("k must be a positive integer");
    if (metric != "cosine" && metric != "l2" && metric != "euclidean" &&
        metric != "dot" && metric != "dot_product") {
        throw std::invalid_argument("unsupported vector metric");
    }
    json filter = query.value("filter", json::object());
    std::string modality = query.value("modality", std::string(""));

    struct Scored { double score; json doc; };
    std::vector<Scored> scored;
    scored.reserve(static_cast<size_t>(std::max(1, k)));

    size_t scanned = 0;
    size_t maxScan = QueryLimiter::getMaxScanRows();
    size_t maxResults = QueryLimiter::getMaxResultDocs();
    const QueryNode filterQuery = parseQuery(filter);
    std::unordered_set<std::string> filterFields;
    collectQueryRootFields(filterQuery, filterFields);

    bool usedHnswLite = false;
    if (hnswLiteEnabled() && !q.empty()) {
        auto key = vectorIndexKey(userId, dbName, collection);
        size_t signature = 0;
        size_t documentCount = 0;
        LSM::visitLatest(userId, dbName, collection, [&](const LSM::LatestRowView& row) {
            ++documentCount;
            const auto id = row.field("id");
            const auto version = row.field("_mvcc_version");
            if (id) signature ^= std::hash<std::string>{}(id->dump());
            if (version) signature ^= std::hash<std::string>{}(version->dump()) * 0x9e3779b97f4a7c15ULL;
            return true;
        });
        signature ^= documentCount * 0xc2b2ae3d27d4eb4fULL;
        HnswLiteIndex idxSnapshot;
        {
            std::lock_guard<std::mutex> lk(g_hnswLiteMutex);
            auto& idx = g_hnswLiteIndexes[key];
            if (idx.signature != signature || (idx.nodes.empty() && documentCount > 0)) {
                std::vector<json> docs;
                docs.reserve(documentCount);
                LSM::visitLatest(userId, dbName, collection,
                    [&](const LSM::LatestRowView& row) {
                        docs.push_back(row.materialize());
                        return true;
                    });
                rebuildHnswLiteIndex(idx, docs, metric);
                idx.signature = signature;
            }
            idxSnapshot = idx;
        }

        if (!idxSnapshot.nodes.empty()) {
            auto candidates = searchHnswLiteCandidates(idxSnapshot, q, metric, hnswLiteEfSearch());
            for (int ci : candidates) {
                if (ci < 0 || ci >= static_cast<int>(idxSnapshot.nodes.size())) continue;
                const auto& node = idxSnapshot.nodes[ci];
                const auto& d = node.doc;

                scanned++;
                if (scanned > maxScan) {
                    QueryLimiter::reportScanLimitReached(scanned, maxScan);
                    break;
                }

                if (!modality.empty() && d.value("modality", std::string("")) != modality) continue;
                if (!evalQuery(filterQuery, d)) continue;
                double score = vectorScore(q, node.vec, metric);
                if (std::isnan(score)) continue;
                scored.push_back({score, d});
                if (scored.size() >= maxResults) {
                    QueryLimiter::reportResultLimitReached(scored.size(), maxResults);
                    QueryLimiter::markTruncated("result_limit");
                    break;
                }
            }
            usedHnswLite = true;
        }
    }

    if (!usedHnswLite || (hnswLiteAllowFallback() && static_cast<int>(scored.size()) < k)) {
        LSM::visitLatest(userId, dbName, collection, [&](const LSM::LatestRowView& row) {
            if (QueryCancel::isCancelled()) {
                std::cerr << "[ENGINE][VECTOR] Query cancelled by timeout\n";
                return false;
            }

            scanned++;
            if (scanned > maxScan) {
                QueryLimiter::reportScanLimitReached(scanned, maxScan);
                return false;
            }

            json projected = projectLatestRow(row, filterFields);
            const auto vectorValue = row.field("vector");
            if (!vectorValue) return true;
            projected["vector"] = *vectorValue;
            if (!modality.empty()) {
                const auto rowModality = row.field("modality");
                if (!rowModality || !rowModality->is_string() ||
                    rowModality->get<std::string>() != modality) return true;
            }
            if (!evalQuery(filterQuery, projected)) return true;
            auto v = jsonToVector(*vectorValue);
            if (v.empty() || q.empty()) return true;
            double score = vectorScore(q, v, metric);
            if (std::isnan(score)) return true;
            scored.push_back({score, row.materialize()});
            if (scored.size() >= maxResults) {
                QueryLimiter::reportResultLimitReached(scored.size(), maxResults);
                QueryLimiter::markTruncated("result_limit");
                return false;
            }
            return true;
        });
    }

    {
        std::unordered_map<std::string, Scored> byId;
        byId.reserve(scored.size());
        for (const auto& s : scored) {
            std::string id = s.doc.value("id", std::string(""));
            if (id.empty()) continue;
            auto it = byId.find(id);
            if (it == byId.end() || s.score > it->second.score) {
                byId[id] = s;
            }
        }
        if (!byId.empty()) {
            scored.clear();
            scored.reserve(byId.size());
            for (auto& kv : byId) scored.push_back(std::move(kv.second));
        }
    }

    if (k < 1) k = 10;
    if (k > (int)scored.size()) k = (int)scored.size();
    if (k > 0) {
        std::nth_element(scored.begin(), scored.begin() + (k - 1), scored.end(),
                         [](const Scored& a, const Scored& b) { return a.score > b.score; });
        std::sort(scored.begin(), scored.begin() + k,
                  [](const Scored& a, const Scored& b) { return a.score > b.score; });
    }

    std::vector<json> out;
    out.reserve(k);
    for (int i = 0; i < k; ++i) {
        json r = scored[i].doc;
        r["score"] = scored[i].score;
        out.push_back(r);
    }
    try {
        auto end = std::chrono::steady_clock::now();
        double latencyMs = std::chrono::duration_cast<std::chrono::microseconds>(end - queryStart).count() / 1000.0;
        std::string key = collection;
        if (!key.empty()) {
            ShardManager& sm = ShardManager::instance();
            std::string shardId = sm.getShardForKey(key);
            sm.updateShardQueryLoad(shardId, latencyMs);
        }
    } catch (...) {}
    return out;
}

/* ---------------- UPDATE ---------------- */
bool DatabaseEngine::updateOne(
    const std::string& userId,
    const std::string& dbName,
    const std::string& collection,
    const json& filter,
    const json& update,
    const json& raftMeta
) {
    requireStorageNamespace(userId, dbName, collection);
    StoragePipelineScope storageScope;
    bool isMainUsers = (dbName == "system" && collection == "users");

    if (isMainUsers) {
        // 🔍 CRITICAL: System users stored in GLOBAL location
        fs::path dataFile = basePath("system", "system") / "data" / (collection + ".bin");
        fs::path walFile = basePath("system", "system") / "wal/db.wal";

        auto docs = Storage::readAll(dataFile.string());
        bool updated = false;

        for (auto& d : docs) {
            if (!isTenantDocumentVisible(userId, d) || !documentVisibleForCurrentRead(d, isMainUsers)) continue;
            if (match(d, filter)) {
                ELOG("[ENGINE][UPDATE][BEFORE] " << d.dump() << "\n");

                bool hasOperator = false;
                for (auto& [k, _] : update.items()) {
                    if (!k.empty() && k[0] == '$') { hasOperator = true; break; }
                }

                json effectiveUpdate = hasOperator ? update : json{{"$set", update}};
                if (!hasOperator) ELOG("[ENGINE][UPDATE] Plain update → auto $set\n");

                applyUpdateOps(d, effectiveUpdate);
                d["tenant_id"] = userId;
                stampMvccWrite(d, true);

                ELOG("[ENGINE][UPDATE][AFTER] " << d.dump() << "\n");
                updated = true;
                break;
            }
        }

        if (!updated) {
            ELOG("[ENGINE][UPDATE] No match for filter " << filter.dump() << "\n");
            return false;
        }

        json walEntry = {
            {"op", "UPDATE"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"filter", filter},
            {"update", update}
        };

        WAL::log(walFile.string(), walEntry);
        Storage::writeAll(dataFile.string(), docs);
        ELOG("[ENGINE][UPDATE] Update persisted to .bin storage\n");
        return true;
    }

    // fallback to LSM path for regular collections
    bool updated = false; json updatedDoc;

    std::vector<json> docs;
    if (filter.is_object() && filter.contains("id") &&
        !filter["id"].is_object() && !filter["id"].is_array()) {
        const std::string id = filter["id"].is_string()
            ? filter["id"].get<std::string>() : filter["id"].dump();
        docs = LSM::findByField(userId, dbName, collection, "id", id, 1);
    } else {
        docs = LSM::getAll(userId, dbName, collection);
    }
    for (auto& d : docs) {
        if (d.contains("_deleted") && d["_deleted"].is_boolean() && d["_deleted"].get<bool>()) continue;
        if (!isTenantDocumentVisible(userId, d) || !documentVisibleForCurrentRead(d)) continue;
        if (match(d, filter)) {
            ELOG("[ENGINE][UPDATE][BEFORE] " << d.dump() << "\n");
            bool hasOperator = false;
            for (auto& [k, _] : update.items()) {
                if (!k.empty() && k[0] == '$') { hasOperator = true; break; }
            }
            json effectiveUpdate = hasOperator ? update : json{{"$set", update}};
            applyUpdateOps(d, effectiveUpdate);
            d["tenant_id"] = userId;
            stampMvccWrite(d, true);
            ELOG("[ENGINE][UPDATE][AFTER] " << d.dump() << "\n");
            updated = true; updatedDoc = d; break;
        }
    }

    if (!updated) { ELOG("[ENGINE][UPDATE] No match for filter " << filter.dump() << "\n"); return false; }
    if (!updatedDoc.contains("id")) { ELOG("[ENGINE][UPDATE] Missing id field, skipping update write\n"); return false; }

    bool raftEnabled = RaftCore::instance().isEnabled();
    if (raftEnabled) {
        if (!RaftCore::instance().isLeader()) {
            std::cerr << "[ENGINE][UPDATE] Node is follower; update must be sent to leader" << std::endl;
            throw std::runtime_error("not_leader");
        }

        json entry = {
            {"op", "UPDATE"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"data", updatedDoc},
            {"filter", filter},
            {"update", update}
        };
        appendRaftWriteMeta(entry, raftMeta);

        bool ok = RaftCore::instance().replicateAndApply(entry);
        if (!ok) {
            std::cerr << "[ENGINE][UPDATE] Raft replicate failed" << std::endl;
            throw std::runtime_error("write_not_committed");
        }

        ELOG("[ENGINE][UPDATE] Replicated update committed via Raft\n");
        return true;
    }

    LSM::put(userId, dbName, collection, updatedDoc);
    updateReadFenceFromDoc(userId, dbName, collection, updatedDoc);
    ELOG("[ENGINE][UPDATE] Update persisted via LSM\n");
    return true;
}

/* ---------------- DELETE ---------------- */
bool DatabaseEngine::deleteOne(const std::string& userId,
                               const std::string& dbName,
                               const std::string& collection,
                               const json& filter,
                               const json& raftMeta) {
    requireStorageNamespace(userId, dbName, collection);
    StoragePipelineScope storageScope;

    fs::path base = basePath(userId, dbName);
    if (!fs::exists(base)) {
        ELOG("[ENGINE][DELETE] DB does not exist user=" << userId << " db=" << dbName << ", returning false\n");
        return false;
    }

    bool isMainUsers = (dbName == "system" && collection == "users");

    if (isMainUsers) {
        fs::path file = base / "data" / (collection + ".bin");
        fs::path walFile = base / "wal/db.wal";

        auto docs = Storage::readAll(file.string());
        std::vector<json> kept;
        bool deleted = false;

        for (auto& d : docs) {
            if (!deleted && match(d, filter)) {
                deleted = true;
                continue;
            }
            kept.push_back(d);
        }

        if (!deleted) return false;

        json walEntry = {
            {"op","DELETE"},
            {"userId",userId},
            {"db",dbName},
            {"collection",collection},
            {"filter",filter}
        };

        WAL::log(walFile.string(), walEntry);
        Storage::writeAll(file.string(), kept);

        ELOG("[ENGINE][DELETE] Success\n");
        return true;
    }

    // LSM-backed collection: locate matching document, then write a tombstone
    bool found = false;
    std::string targetId;

    // OPTIMIZATION: If filter contains "id", resolve via indexed lookup while enforcing tenant visibility.
    if (filter.contains("id") && filter["id"].is_string()) {
        const std::string id = filter["id"].get<std::string>();
        auto docsById = LSM::findByField(userId, dbName, collection, "id", id);
        for (auto& d : docsById) {
            if (d.contains("_deleted") && d["_deleted"].is_boolean() && d["_deleted"].get<bool>()) continue;
            if (!isTenantDocumentVisible(userId, d)) continue;
            if (match(d, filter) && d.contains("id") && d["id"].is_string()) {
                targetId = d["id"].get<std::string>();
                found = true;
                break;
            }
        }
        ELOG("[ENGINE][DELETE] Fast path lookup by id=" << id << " found=" << (found ? "true" : "false") << "\n");
    }
    // Fallback: Full scan for complex filters (slower)
    else {
        auto docsAll = LSM::getAll(userId, dbName, collection);
        for (auto& d : docsAll) {
            // skip LSM tombstones when searching
            if (d.contains("_deleted") && d["_deleted"].is_boolean() && d["_deleted"].get<bool>()) continue;
            if (!isTenantDocumentVisible(userId, d) || !documentVisibleForCurrentRead(d)) continue;
            if (match(d, filter)) {
                if (d.contains("id") && d["id"].is_string()) {
                    targetId = d["id"].get<std::string>();
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        ELOG("[ENGINE][DELETE] No match for filter " << filter.dump() << "\n");
        return false;
    }

    bool raftEnabled = RaftCore::instance().isEnabled();
    if (raftEnabled) {
        if (!RaftCore::instance().isLeader()) {
            std::cerr << "[ENGINE][DELETE] Node is follower; delete must be sent to leader" << std::endl;
            throw std::runtime_error("not_leader");
        }

        json tombstone = {
            {"id", targetId},
            {"_deleted", true},
            {"tenant_id", userId},
            {"_timestamp", static_cast<long long>(std::time(nullptr))}
        };
        stampMvccDelete(tombstone);

        json entry = {
            {"op", "DELETE"},
            {"userId", userId},
            {"db", dbName},
            {"collection", collection},
            {"filter", filter},
            {"data", tombstone}
        };
        appendRaftWriteMeta(entry, raftMeta);

        bool ok = RaftCore::instance().replicateAndApply(entry);
        if (!ok) {
            std::cerr << "[ENGINE][DELETE] Raft replicate failed" << std::endl;
            throw std::runtime_error("write_not_committed");
        }

        ELOG("[ENGINE][DELETE] Replicated tombstone committed via Raft\n");
        return true;
    }

    // write tombstone via LSM::del (which logs a DELETE and inserts tombstone into memtable)
    LSM::del(userId, dbName, collection, targetId);

    ELOG("[ENGINE][DELETE] Tombstone written for id=" << targetId << "\n");
    return true;
}

/* ---------------- MATCH ---------------- */
bool DatabaseEngine::match(const json& doc, const json& filter)
{
    if (filter.is_null() || filter.empty()) return true;
    QueryNode query = parseQuery(filter);
    return evalQuery(query, doc);
}
