#include "lsm.hpp"
#include "lsm_manifest.hpp"
#include "owned_test_root.hpp"
#include "storage_format_v2.hpp"
#include "wal.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    const OwnedTestRoot ownedRoot("lsm-checkpoint");
    const fs::path root = ownedRoot.dataRoot();
    const std::string user = "checkpoint_user";
    const std::string database = "checkpoint_db";
    const std::string collection = "documents";
    try {
#ifndef _WIN32
        setenv("WAL_GROUP_COMMIT", "false", 1);
        setenv("WAL_BATCH_INTERVAL_MS", "0", 1);
        setenv("WAL_FSYNC_ENABLED", "true", 1);
        setenv("WAL_SEGMENT_MAX_BYTES", "4096", 1);
#endif
        LSM::init(root.string());
        for (int i = 0; i < 100; ++i) {
            LSM::put(user, database, collection,
                     {{"id", "doc-" + std::to_string(i)}, {"value", i}});
        }
        const fs::path wal = root / user / database / "wal" / (collection + ".wal");
        const auto segmentsBeforeFlush = WAL::getSegmentCount(wal.string());
        require(segmentsBeforeFlush > 1, "fixture rotates collection WAL segments");

        LSM::flush(user, database, collection);
        const fs::path lsm = root / user / database / (collection + ".lsm");
        const auto loaded = LsmManifestStore::load(lsm);
        require(loaded.status == LsmManifestLoadStatus::OK &&
                    loaded.manifest.coveredWalLsn > 0,
                "flush publishes a positive WAL checkpoint");
        std::vector<fs::path> published;
        std::string error;
        require(LsmManifestStore::publishedSsts(lsm, published, &error) && !published.empty(), error);
        for (const auto& sst : published) {
            require(pacificdb::storage_v2::verifySst(sst, &error), error);
        }
        require(WAL::getSegmentCount(wal.string()) < segmentsBeforeFlush,
                "flush reclaims only checkpoint-covered sealed WAL segments");

        std::vector<json> orphanRowsStorage;
        for (int i = 0; i < 25; ++i) {
            orphanRowsStorage.push_back({{"id", "orphan-" + std::to_string(i)}, {"value", i}});
        }
        std::vector<std::reference_wrapper<const json>> orphanRows;
        for (const auto& row : orphanRowsStorage) orphanRows.emplace_back(row);
        require(pacificdb::storage_v2::writeSst(
                    lsm / "orphan.sst", orphanRows, 4096, nullptr, &error), error);
        require(LSM::getAll(user, database, collection).size() == 100,
                "manifest-authoritative reads ignore orphan SSTs");

        LSM::put(user, database, collection, {{"id", "doc-7"}, {"value", "new"}});
        const auto afterCheckpoint = WAL::scan(wal.string(), [](const WalReplayRecord&) {
            return true;
        });
        require(afterCheckpoint.status == WalScanStatus::OK &&
                    afterCheckpoint.lastLsn > loaded.manifest.coveredWalLsn,
                "post-checkpoint WAL records remain available");
        LSM::flush(user, database, collection);
        const auto documents = LSM::getAll(user, database, collection);
        bool sawNewValue = false;
        for (const auto& document : documents) {
            if (document.value("id", "") == "doc-7") {
                sawNewValue = document.value("value", json()) == json("new");
            }
        }
        require(documents.size() == 100 && sawNewValue,
                "newer value wins across checkpoint generations");

        WAL::shutdown();
        std::cout << "lsm_checkpoint_test passed\n";
        return 0;
    } catch (const std::exception& error) {
        WAL::shutdown();
        std::cerr << "lsm_checkpoint_test failed: " << error.what() << '\n';
        return 1;
    }
}
