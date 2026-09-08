#include "database_engine.hpp"
#include "lsm.hpp"
#include "metrics_exporter.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

static void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << std::endl;
        std::exit(1);
    }
    std::cout << "[PASS] " << message << std::endl;
}

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("pacificdb-strong-read-memtable-" + std::to_string(nonce));
    const std::string user = "strong-read-user";
    const std::string database = "strong-read-db";
    const std::string collection = "documents";
    const std::string id = "schema-manifest";

    fs::remove_all(root);
    DatabaseEngine::init(root.string());
    LSM::init(root.string());
    LSM::startBackgroundTasks();
    DatabaseEngine::ensureUserRoot(user);
    require(DatabaseEngine::createDatabase(user, database, "binary"), "database created");
    require(!DatabaseEngine::createCollection(user, database, collection).empty(), "collection created");

    DatabaseEngine::insert(user, database, collection, {
        {"id", id},
        {"version", 900000},
        {"kind", "schema-manifest"}
    });

    bool fenceFound = false;
    const long long floor = DatabaseEngine::getReadFenceVersion(
        user, database, collection, id, &fenceFound);
    require(fenceFound && floor > 0 && floor != 900000,
            "read fence uses the engine MVCC version");

    LSM::invalidateIdCache(user, database, collection, id);
    const json result = LSM::findStrongSnapshotBound(
        user, database, collection, id, static_cast<uint64_t>(floor));
    require(result.value("found", false), "unflushed document is visible by id");
    require(result.value("floor_satisfied", false), "snapshot-bound read satisfies MVCC floor");
    require(result.value("visibility_source", std::string()) == "MEMTABLE",
            "snapshot-bound read resolves the active memtable");
    require(result.value("returned_version", -1LL) == floor,
            "returned version matches the engine read fence");

    const json cachedResult = LSM::findStrongSnapshotBound(
        user, database, collection, id, static_cast<uint64_t>(floor));
    require(cachedResult.value("found", false),
            "authoritative id index preserves the strong-read result");
    require(cachedResult.value("visibility_source", std::string()) == "CACHE",
            "repeated strong point read avoids storage amplification");

    const auto index = LSM::createSecondaryIndex(
        user, database, collection,
        {{"name", "kind_1"}, {"field", "kind"}, {"type", "btree"}});
    require(index.value("status", std::string()) == "ok", "secondary index created");
    const auto indexHitsBefore = LSM::getRuntimeStats().value("index_hits", 0ULL);
    DatabaseEngine::insert(user, database, collection, {
        {"id", "schema-manifest-2"},
        {"kind", "schema-manifest"}
    });
    DatabaseEngine::setReadContext("strong", 0, -1);
    const auto indexed = DatabaseEngine::find(
        user, database, collection, {{"kind", "schema-manifest"}});
    DatabaseEngine::clearReadContext();
    require(indexed.size() == 2, "strong read drains pending index updates");
    require(LSM::getRuntimeStats().value("index_hits", 0ULL) > indexHitsBefore,
            "strong read uses the secondary index path");

    const fs::path indexRoot = root / user / database / (collection + ".idx");
    require(fs::is_directory(indexRoot / "kind.cidx"),
            "secondary index is stored as binary segments");
    require(!fs::exists(indexRoot / "kind.json"),
            "whole-file JSON index is retired");

    LSM::flush(user, database, collection);
    const auto shadowBefore = LSM::getRuntimeStats();
    DatabaseEngine::setReadContext("strong", 0, -1);
    const auto strongById = DatabaseEngine::find(
        user, database, collection, {{"id", id}});
    DatabaseEngine::clearReadContext();
    const auto shadowAfter = LSM::getRuntimeStats();
    require(strongById.size() == 1, "shadowed strong id read keeps authoritative result");
    require(shadowAfter.value("strong_id_cache_shadow_matches", 0ULL) >
                shadowBefore.value("strong_id_cache_shadow_matches", 0ULL),
            "strong id cache candidate matches the authoritative path");
    require(shadowAfter.value("strong_id_cache_shadow_mismatches", 0ULL) ==
                shadowBefore.value("strong_id_cache_shadow_mismatches", 0ULL),
            "strong id cache shadow reports no mismatch");

    const auto scansBeforeUpdate =
        LSM::getLsmMetrics().value("sst_scans_samples", 0ULL);
    require(DatabaseEngine::updateOne(
                user, database, collection, {{"id", "schema-manifest-2"}},
                {{"kind", "schema-version"}}),
            "indexed value updated");
    require(LSM::getLsmMetrics().value("sst_scans_samples", 0ULL) == scansBeforeUpdate,
            "id update avoids a full collection scan");
    DatabaseEngine::setReadContext("strong", 0, -1);
    const auto oldValue = DatabaseEngine::find(
        user, database, collection, {{"kind", "schema-manifest"}});
    const auto newValue = DatabaseEngine::find(
        user, database, collection, {{"kind", "schema-version"}});
    DatabaseEngine::clearReadContext();
    require(oldValue.size() == 1 && newValue.size() == 1,
            "index tombstone removes the previous value");

    require(DatabaseEngine::deleteOne(
                user, database, collection, {{"id", "schema-manifest-2"}}),
            "indexed document deleted");
    DatabaseEngine::setReadContext("strong", 0, -1);
    const auto deleted = DatabaseEngine::find(
        user, database, collection, {{"kind", "schema-version"}});
    DatabaseEngine::clearReadContext();
    require(deleted.empty(), "index tombstone removes a deleted document");

    const auto tombstoneShadowBefore = LSM::getRuntimeStats();
    DatabaseEngine::setReadContext("strong", 0, -1);
    const auto deletedById = DatabaseEngine::find(
        user, database, collection, {{"id", "schema-manifest-2"}});
    DatabaseEngine::clearReadContext();
    const auto tombstoneShadowAfter = LSM::getRuntimeStats();
    require(deletedById.empty(), "shadowed strong id read preserves deletion");
    require(tombstoneShadowAfter.value("strong_id_cache_shadow_matches", 0ULL) >
                tombstoneShadowBefore.value("strong_id_cache_shadow_matches", 0ULL),
            "strong id cache retains the authoritative tombstone");
    require(tombstoneShadowAfter.value("strong_id_cache_shadow_misses", 0ULL) ==
                tombstoneShadowBefore.value("strong_id_cache_shadow_misses", 0ULL),
            "deleted id is not misclassified as a cache miss");
    const auto metrics = MetricsExporter::getMetrics();
    require(metrics.find("pacificdb_strong_id_cache_shadow_matches_total ") != std::string::npos,
            "strong id cache shadow matches are exported");
    require(metrics.find("pacificdb_strong_id_cache_shadow_mismatches_total 0") != std::string::npos,
            "strong id cache shadow mismatches are exported");

    if (const char* configured = std::getenv("FIND_ID_INDEX_MAX");
        configured && std::string(configured) == "64") {
        for (int i = 0; i < 128; ++i) {
            const std::string freshId = "cache-bound-" + std::to_string(i);
            DatabaseEngine::insert(user, database, collection, {
                {"id", freshId},
                {"kind", "cache-bound"}
            });
            const auto fresh = LSM::findByField(user, database, collection, "id", freshId);
            require(fresh.size() == 1 && fresh[0].value("id", std::string()) == freshId,
                    "fresh write remains visible with a full cache");
            require(LSM::getLastReadVisibilitySource() == "CACHE",
                    "cache eviction retains the freshly applied document");
        }
        const auto bounded = LSM::getRuntimeStats();
        require(bounded.value("id_index_entries_total", 65ULL) <= 64,
                "striped id cache honors the collection-wide bound");
    }

    LSM::stopBackgroundTasks();
    fs::remove_all(root);
    std::cout << "STRONG_READ_MEMTABLE_REGRESSION_PASS" << std::endl;
    return 0;
}
