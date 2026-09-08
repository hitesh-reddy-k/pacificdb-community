#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

struct QueryNode;

class DatabaseEngine {
public:
    // Database types
    enum class DatabaseType {
        BINARY,
        VECTOR
    };

    // Establish the canonical root without creating logical catalog state.
    // Startup recovery needs this before replay can call applyReplicatedEntry.
    static void configureStorageRoot(const std::string& rootPath);
    static void init(const std::string& rootPath, bool restoreWal = true);
    static void ensureUserRoot(const std::string& userId);
    // Returns true on success (created or already exists), false on failure
    static bool createDatabase(const std::string& userId, const std::string& dbName, const std::string& dbType = "binary");
    // Returns true on success (created or already exists), false on failure
    // Returns the generated collection id on success, or empty string on failure
    static std::string createCollection(const std::string& userId, const std::string& dbName, const std::string& collection);
    static bool dropCollection(const std::string& userId, const std::string& dbName, const std::string& collection);
    static bool dropDatabase(const std::string& userId, const std::string& dbName);
    static std::vector<std::string> listDatabases(const std::string& userId);
    static json getDatabaseMetadata(const std::string& userId, const std::string& dbName);
    static void insert(const std::string& userId, const std::string& dbName, const std::string& collection, json doc, const json& raftMeta = json::object());
    static json insertMany(const std::string& userId, const std::string& dbName, const std::string& collection, std::vector<json> docs, const json& raftMeta = json::object());
    static void insertVector(const std::string& userId, const std::string& dbName, const std::string& collection, const json& doc, const json& raftMeta = json::object());
    static std::vector<json> find(const std::string& userId, const std::string& dbName, const std::string& collection, const json& filter, long long limit = -1, long long offset = 0);
    static size_t count(const std::string& userId, const std::string& dbName, const std::string& collection, const json& filter = json::object());
    static std::vector<json> queryVector(const std::string& userId, const std::string& dbName, const std::string& collection, const json& query);
    static bool updateOne(const std::string& userId, const std::string& dbName, const std::string& collection, const json& filter, const json& update, const json& raftMeta = json::object());
    static bool deleteOne(const std::string& userId, const std::string& dbName, const std::string& collection, const json& filter, const json& raftMeta = json::object());
    static bool match(const nlohmann::json& doc,
                      const nlohmann::json& filter);
    static void setReadContext(const std::string& consistency,
                               long long maxStalenessMs = 0,
                               long long readFloorVersion = -1);
    static void clearReadContext();
    static std::string getLastReadVisibilitySource();
    // Validation helpers
    static bool userExists(const std::string& userId);
    static bool databaseExists(const std::string& userId, const std::string& dbName);
    static bool collectionExists(const std::string& userId, const std::string& dbName, const std::string& collection);
    // Raft integration helpers
    static std::string getDataRoot();
    // Apply a replicated WAL entry locally without re-logging it (used by followers).
    // Returns false only when the state-machine apply did not reach visible storage.
    static bool applyReplicatedEntry(const nlohmann::json& entry);

    // Recovery replay has no client to push back on. Write backpressure must not be
    // applied to replayed records, or a large WAL makes startup take hours.
    static void setRecoveryReplayMode(bool enabled);
    static bool recoveryReplayMode();
    // Apply a snapshot payload (LSM) received via Raft install_snapshot
    static bool applySnapshot(
        const nlohmann::json& payload,
        const std::string& bundlePath = {});
    // Strong-read fence for latest applied versions (used by STRONG reads)
    static long long getReadFenceVersion(const std::string& userId,
                                         const std::string& dbName,
                                         const std::string& collection,
                                         const std::string& id,
                                         bool* found = nullptr);
    static void invalidateReadCache(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection,
                                    const std::string& id);
};
