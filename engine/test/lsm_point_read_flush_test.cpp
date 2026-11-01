#include "lsm.hpp"
#include "database_engine.hpp"
#include "raft_core.hpp"
#include "owned_test_root.hpp"
#include "test_failpoint.hpp"
#include "wal.hpp"
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
std::mutex barrierMutex;
std::function<void()> betweenLayers;
std::function<void()> afterVisibility;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void exercise(const std::string& collection, bool inserted, bool deleted, bool strong) {
    const std::string user = "alice", database = "app", id = "same";
    LSM::put(user, database, collection,
        {{"id", inserted ? "sentinel" : id}, {"_mvcc_version", 1}, {"vector", {1., 0.}}});
    LSM::flush(user, database, collection);
    if (deleted) {
        LSM::put(user, database, collection,
            {{"id", id}, {"_mvcc_version", 2}, {"_deleted", true}},
            false, 0, true, LSM::PutIntent::Tombstone);
    } else {
        LSM::put(user, database, collection,
            {{"id", id}, {"_mvcc_version", 2}, {"vector", {0., 2.}}});
    }
    LSM::put("bob", database, collection,
        {{"id", id}, {"_mvcc_version", 3}, {"vector", {9., 9.}}});
    LSM::invalidateIdCache(user, database, collection, id);
    LSM::setSkipIdCache(strong);
    {
        std::lock_guard<std::mutex> guard(barrierMutex);
        betweenLayers = [&] { LSM::flush(user, database, collection); };
    }
    auto check = [&](const std::vector<json>& rows) {
        if (deleted) {
            require(rows.empty(), "point read resurrected a committed deletion during flush");
        } else {
            require(rows.size() == 1, "point read lost a committed inserted row during flush");
            require(rows[0].at("_mvcc_version") == 2,
                "point read returned the old version after the newer row flushed between layers");
            require(rows[0].at("vector") == json::array({0., 2.}),
                "point read returned a vector from the wrong committed version");
        }
    };
    check(LSM::findByField(user, database, collection, "id", id, 1));
    LSM::clearSkipIdCache();
    check(LSM::findByField(user, database, collection, "id", id, 1));
    const auto other = LSM::findByField("bob", database, collection, "id", id, 1);
    require(other.size() == 1 && other[0].at("vector") == json::array({9., 9.}),
        "point read mixed namespaces with the same ID");
}
void exerciseCacheRefill(bool snapshotBound, bool evict, bool deletion) {
    const std::string user = "alice", database = "app", id = "same";
    const std::string collection = std::string("refill_") + (snapshotBound ? "snapshot_" : "ordinary_")
        + (evict ? "evict_" : "retained_") + (deletion ? "delete" : "update");
    LSM::put(user, database, collection, {{"id", id}, {"_mvcc_version", 1}, {"vector", {1., 0.}}});
    LSM::flush(user, database, collection);
    LSM::put(user, database, collection, {{"id", id}, {"_mvcc_version", 2}, {"vector", {0., 2.}}});
    LSM::invalidateIdCache(user, database, collection, id);
    {
        std::lock_guard<std::mutex> guard(barrierMutex);
        betweenLayers = [&] {
            if (deletion) {
                LSM::put(user, database, collection,
                    {{"id", id}, {"_mvcc_version", 3}, {"_deleted", true}},
                    false, 0, true, LSM::PutIntent::Tombstone);
            } else {
                LSM::put(user, database, collection,
                    {{"id", id}, {"_mvcc_version", 3}, {"vector", {3., 0.}}});
            }
            if (evict) LSM::invalidateIdCache(user, database, collection, id);
        };
    }
    const auto read = [&]() -> std::vector<json> {
        if (!snapshotBound) return LSM::findByField(user, database, collection, "id", id, 1);
        const auto result = LSM::findStrongSnapshotBound(user, database, collection, id, 0);
        return result.at("documents").get<std::vector<json>>();
    };
    const auto concurrent = read();
    require(concurrent.size() == 1 && concurrent[0].at("_mvcc_version") == 2,
        "fixture must retain the value visible when the concurrent read began");
    const auto afterCommit = read();
    if (deletion) {
        require(afterCommit.empty(), "late point-read cache refill resurrected a committed deletion");
    } else {
        require(afterCommit.size() == 1 && afterCommit[0].at("_mvcc_version") == 3,
            "late point-read cache refill replaced a newer committed value");
    }
}
void exerciseVisibilityAdvance(const std::filesystem::path& root) {
    DatabaseEngine::init(root.string());
    DatabaseEngine::ensureUserRoot("alice");
    require(DatabaseEngine::createDatabase("alice", "visibility", "vector"), "create visibility database");
    require(!DatabaseEngine::createCollection("alice", "visibility", "vectors").empty(), "create visibility collection");
    auto& raft = RaftCore::instance();
    std::filesystem::create_directories(root / "raft");
    raft.init({}, "point-visibility-test", 0, true);
    raft.start();
    struct Stop { RaftCore& raft; ~Stop() { raft.stop(); } } stop{raft};
    const auto write = [&](int version) {
        require(raft.replicateAndApply({{"op", "INSERT"}, {"userId", "alice"},
            {"db", "visibility"}, {"collection", "vectors"},
            {"data", {{"id", "same"}, {"_mvcc_version", version}, {"vector", {version, 1}}}}}),
            "real Raft write must commit");
    };
    write(1); LSM::flush("alice", "visibility", "vectors"); write(2);
    LSM::invalidateIdCache("alice", "visibility", "vectors", "same");
    {
        std::lock_guard<std::mutex> guard(barrierMutex);
        afterVisibility = [&] { write(3); };
    }
    const auto during = LSM::findStrongSnapshotBound("alice", "visibility", "vectors", "same", 2);
    require(!during.at("floor_satisfied").get<bool>() || during.at("returned_version").get<int>() >= 2,
        "a satisfied key floor must never return an older version");
    const auto after = LSM::findStrongSnapshotBound("alice", "visibility", "vectors", "same", 0);
    require(after.at("returned_version") == 3,
        "old Raft visibility bound poisoned the point cache after a concurrent commit");
    LSM::invalidateIdCache("alice", "visibility", "vectors", "same");
    LSM::setSkipIdCache(true);
    {
        std::lock_guard<std::mutex> guard(barrierMutex);
        afterVisibility = [&] { write(4); };
    }
    (void)LSM::findByField("alice", "visibility", "vectors", "id", "same", 1);
    LSM::clearSkipIdCache();
    const auto ordinary = LSM::findByField("alice", "visibility", "vectors", "id", "same", 1);
    require(ordinary.size() == 1 && ordinary[0].at("_mvcc_version") == 4,
        "old Raft visibility bound poisoned the ordinary point cache after a concurrent commit");
    LSM::flush("alice", "visibility", "vectors"); write(5);
    LSM::invalidateIdCache("alice", "visibility", "vectors", "same");
    {
        std::lock_guard<std::mutex> guard(barrierMutex);
        afterVisibility = [&] { write(6); };
    }
    const auto noFloor = LSM::findStrongSnapshotBound("alice", "visibility", "vectors", "same", 0);
    require(noFloor.at("returned_version").get<int>() >= 5,
        "point read rolled back behind a value committed before the read began");
    LSM::flush("alice", "visibility", "vectors"); write(7);
    {
        std::lock_guard<std::mutex> guard(barrierMutex);
        afterVisibility = [&] { write(8); };
    }
    const auto existsDuringWrite = LSM::findByField("alice", "visibility", "vectors", "id", "same", 1);
    require(existsDuringWrite.size() == 1 && existsDuringWrite[0].at("_mvcc_version").get<int>() >= 7,
        "point read falsely missed a continuously existing ID during a concurrent commit");
}
}

// This executable supplies a scheduling runtime for the existing failpoint seam.
// It keeps real LSM/WAL/SST behavior and flushes at a deterministic read boundary;
// no production executable links this runtime or gains a test-only public API.
namespace pacificdb::test {
bool failpointsCompiled() { return false; }
void requestShutdown() noexcept {}
bool injectApplyFailure(const char*, std::uint64_t) { return false; }
void hitFailpoint(const char* name, std::uint64_t) {
    std::function<void()> action;
    {
        std::lock_guard<std::mutex> guard(barrierMutex);
        if (std::strcmp(name, "FP_LSM_POINT_BETWEEN_LAYERS") == 0 ||
            std::strcmp(name, "FP_LSM_STRONG_POINT_BETWEEN_LAYERS") == 0) action.swap(betweenLayers);
        if (std::strcmp(name, "FP_LSM_POINT_AFTER_VISIBILITY_BOUND") == 0 ||
            std::strcmp(name, "FP_LSM_STRONG_POINT_AFTER_VISIBILITY_BOUND") == 0) action.swap(afterVisibility);
    }
    if (action) action();
}
}

int main() {
    OwnedTestRoot owned("lsm-point-read-flush");
    try {
        const auto environment = [](const char* name, const std::string& value) {
#ifdef _WIN32
            _putenv_s(name, value.c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        };
        environment("DATA_ROOT", owned.dataRoot().string());
        environment("DATA_DIR", owned.dataRoot().string());
        environment("RAFT_STANDALONE_BYPASS", "0");
        environment("MIN_QUORUM_SIZE", "1");
        environment("WAL_FSYNC_ENABLED", "true");
        LSM::init(owned.dataRoot().string());
        for (bool strong : {false, true}) {
            const auto suffix = strong ? "strong" : "ordinary";
            exercise(std::string("replace_") + suffix, false, false, strong);
            exercise(std::string("insert_") + suffix, true, false, strong);
            exercise(std::string("delete_") + suffix, false, true, strong);
        }
        for (bool snapshotBound : {false, true})
            for (bool evict : {false, true})
                for (bool deletion : {false, true}) exerciseCacheRefill(snapshotBound, evict, deletion);
        exerciseVisibilityAdvance(owned.dataRoot());
        WAL::shutdown();
        std::cout << "LSM_POINT_READ_FLUSH_PASS\n";
        return 0;
    } catch (const std::exception& error) {
        LSM::clearSkipIdCache();
        WAL::shutdown();
        std::cerr << "LSM_POINT_READ_FLUSH_FAIL: " << error.what() << '\n';
        return 1;
    }
}
