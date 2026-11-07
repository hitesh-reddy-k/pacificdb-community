#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "wal.hpp"
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
json read(const std::string& collection, const std::string& user = "alice") {
    return LSM::findStrongSnapshotBound(user, "app", collection, "same", 0);
}
void row(const std::string& collection, int version, const json& vector,
         const std::string& user = "alice") {
    LSM::put(user, "app", collection,
        {{"id", "same"}, {"_mvcc_version", version}, {"vector", vector}});
}
int main() {
    OwnedTestRoot root("verified-point-cache");
    try {
        LSM::init(root.dataRoot().string());
        row("vectors", 2, {0., 2.}); LSM::flush("alice", "app", "vectors");
        // Recovery can replay an older row while a newer one is already in SSTs.
        // A writer-populated cache entry is not proof of authoritative recency.
        row("vectors", 1, {1., 0.});
        const auto first = read("vectors");
        require(first.at("returned_version") == 2,
            "authoritative point read trusted an unverified older writer cache entry");
        require(first.at("documents")[0].at("vector") == json::array({0., 2.}), "wrong committed vector");
        const auto warm = read("vectors");
        require(warm.at("returned_version") == 2 && warm.at("visibility_source") == "CACHE",
            "unchanged authoritative row was not reusable from the verified cache");
        const auto snapshot = LSM::exportSnapshotJson(1);
        row("vectors", 3, {3., 0.});
        require(read("vectors").at("returned_version") == 3, "mutation retained the old cache proof");
        require(LSM::applySnapshot(snapshot), "snapshot restore failed");
        require(read("vectors").at("returned_version") == 2, "restored source reused a previous-root cache proof");
        require(LSM::dropCollection("alice", "app", "vectors"), "drop failed");
        row("vectors", 1, {9., 0.});
        require(read("vectors").at("documents")[0].at("vector") == json::array({9., 0.}),
            "recreated namespace reused the dropped collection cache");
        row("deleted", 1, {1., 0.});
        LSM::put("alice", "app", "deleted", {{"id", "same"}, {"_mvcc_version", 2}, {"_deleted", true}},
            false, 0, true, LSM::PutIntent::Tombstone);
        LSM::flush("alice", "app", "deleted"); row("deleted", 1, {1., 0.});
        const auto deleted = read("deleted");
        require(!deleted.at("found").get<bool>() && deleted.at("returned_version") == 2,
            "unverified replay cache resurrected a committed tombstone");
        row("vectors", 5, {5., 5.}, "bob");
        require(read("vectors", "bob").at("documents")[0].at("vector") == json::array({5., 5.}), "wrong tenant row");
        require(read("vectors").at("documents")[0].at("vector") == json::array({9., 0.}), "cache crossed tenant boundary");
        WAL::shutdown(); std::cout << "VERIFIED_POINT_CACHE_PASS\n"; return 0;
    } catch (const std::exception& error) {
        WAL::shutdown(); std::cerr << "VERIFIED_POINT_CACHE_FAIL: " << error.what() << '\n'; return 1;
    }
}
