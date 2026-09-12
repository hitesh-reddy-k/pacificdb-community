#include <iostream>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include "lsm.hpp"
#include "lsm_manifest.hpp"
#include "data_durability.hpp"
#include "owned_test_root.hpp"
#include "storage_format_v2.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

int main() {
    try {
        const OwnedTestRoot ownedRoot("snapshot-e2e");
        const std::string leaderRoot =
            (ownedRoot.dataRoot() / "leader").string();
        const std::string followerRoot =
            (ownedRoot.dataRoot() / "follower").string();

        fs::create_directories(leaderRoot);
        fs::create_directories(followerRoot);

        const std::string checksumFixture = "streamed-snapshot-check";
        const fs::path checksumPath = ownedRoot.dataRoot() / "snapshot.bin";
        std::ofstream(checksumPath, std::ios::binary) << checksumFixture;
        const std::string expectedChecksum =
            pacificdb::durability::ChecksumCalculator::sha256(
                checksumFixture.data(), checksumFixture.size());
        if (pacificdb::durability::ChecksumCalculator::sha256File(
                checksumPath.string()) != expectedChecksum) {
            std::cerr << "Streaming snapshot checksum verification failed" << std::endl;
            return 1;
        }

        // Initialize leader LSM and write some data
        LSM::init(leaderRoot);
        // create some collections and insert data
        json d1 = { {"id","a"}, {"name","alice"} };
        json d2 = { {"id","b"}, {"name","bob"} };
        json d3 = { {"id","c"}, {"name","alice"} };
        json d4 = { {"id","d"}, {"unindexed",true} };
        // The standalone fixture does not start the asynchronous index batcher,
        // so make these writes synchronously index-complete. Otherwise a stale
        // pending-queue marker correctly disables authoritative index reads and
        // the missing-sidecar planner branch is never exercised.
        LSM::put("user1","db1","coll1", d1, true);
        LSM::put("user1","db1","coll1", d2, true);
        LSM::put("user1","db1","coll1", d3, true);
        LSM::put("user1","db1","coll1", d4, true);

        auto index = LSM::createSecondaryIndex(
            "user1", "db1", "coll1",
            {{"name", "idx_coll1_name"}, {"field", "name"},
             {"type", "btree"}, {"unique", false}});
        if (index.value("status", std::string()) == "error") {
            std::cerr << "Failed to create source index: " << index.dump() << std::endl;
            return 1;
        }

        // Force flush to SST
        LSM::forceFlush();

        // Export snapshot
        json snap = LSM::exportSnapshotJson(10);
        if (snap.is_null()) { std::cerr << "Failed to export snapshot" << std::endl; return 1; }
        bool rawBinarySst = false;
        for (const auto& file : snap.value("files", json::array())) {
            if (fs::path(file.value("path", std::string())).extension() == ".sst") {
                rawBinarySst = file.value("contentEncoding", std::string()) ==
                    "pdb2-sst-base64";
                break;
            }
        }
        if (!rawBinarySst) {
            std::cerr << "Snapshot expanded binary SST rows instead of preserving bytes"
                      << std::endl;
            return 1;
        }

        // Initialize follower LSM (empty) and apply snapshot
        LSM::init(followerRoot);
        fs::create_directories(fs::path(followerRoot) / "security");
        fs::create_directories(fs::path(followerRoot) / "restores");
        std::ofstream(fs::path(followerRoot) / "security" / "api_keys.json")
            << "acknowledged-api-key-state";
        std::ofstream(fs::path(followerRoot) / "restores" / "journal.json")
            << "restore-state";
        std::ofstream(fs::path(followerRoot) / "shard_map.json") << "shard-state";
        bool ok = LSM::applySnapshot(snap);
        if (!ok) { std::cerr << "applySnapshot failed" << std::endl; return 1; }
        if (!fs::exists(fs::path(followerRoot) / "security" / "api_keys.json") ||
            !fs::exists(fs::path(followerRoot) / "restores" / "journal.json") ||
            !fs::exists(fs::path(followerRoot) / "shard_map.json")) {
            std::cerr << "snapshot apply removed non-LSM engine state" << std::endl;
            return 1;
        }
        if (!LSM::runPendingIndexRebuild()) {
            std::cerr << "snapshot index rebuild failed" << std::endl;
            return 1;
        }

        // Snapshot payloads contain typed rows; applySnapshot must publish a
        // v2 SST and rebuild its derived bloom before exposing the tree.
        const fs::path restoredLsm =
            fs::path(followerRoot) / "user1" / "db1" / "coll1.lsm";
        auto restoredManifest = LsmManifestStore::load(restoredLsm);
        if (restoredManifest.status != LsmManifestLoadStatus::OK ||
            restoredManifest.manifest.coveredWalLsn != 0 ||
            restoredManifest.manifest.sstFiles.empty()) {
            throw std::runtime_error("snapshot must publish a manifest without claiming collection WAL coverage");
        }
        fs::path restoredSst;
        for (const auto& entry : fs::directory_iterator(restoredLsm)) {
            if (entry.path().extension() == ".sst") {
                restoredSst = entry.path();
                break;
            }
        }
        if (restoredSst.empty() ||
            !fs::exists(restoredSst.string() + ".bloom") ||
            !pacificdb::storage_v2::isBinarySst(restoredSst) ||
            fs::exists(restoredSst.string() + ".sidx")) {
            std::cerr << "Verification failed: restored v2 SST artifacts are invalid" << std::endl;
            return 1;
        }

        // Verify follower now has SST files and can return documents
        auto docs = LSM::getAll("user1","db1","coll1");
        if (docs.size() < 4) {
            std::cerr << "Verification failed: expected >=4 docs, got " << docs.size() << std::endl;
            return 1;
        }

        auto indexed = LSM::findByField(
            "user1", "db1", "coll1", "name", "alice", 10);
        std::set<std::string> indexedIds;
        for (const auto& document : indexed) {
            indexedIds.insert(document.value("id", std::string()));
        }
        if (indexedIds != std::set<std::string>{"a", "c"}) {
            std::cerr << "Verification failed: restored secondary index lookup returned "
                      << indexed.size() << " rows" << std::endl;
            return 1;
        }

        // Older snapshot restores omitted the manifest. Startup must migrate
        // those validated SSTs once, not validate every row on every point read.
        fs::remove(restoredLsm / "_manifest.json");
        LSM::init(followerRoot);
        restoredManifest = LsmManifestStore::load(restoredLsm);
        if (restoredManifest.status != LsmManifestLoadStatus::OK ||
            restoredManifest.manifest.coveredWalLsn != 0 ||
            LSM::getAll("user1", "db1", "coll1").size() != 4) {
            throw std::runtime_error("legacy startup manifest migration lost data or claimed WAL coverage");
        }

        std::cout << "E2E snapshot apply verification passed: docs=" << docs.size()
                  << ", binary SST, bloom, and rebuilt column index verified" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
