#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "storage_format_v2.hpp"
#include "wal.hpp"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace fs = std::filesystem;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
#ifdef _WIN32
    _putenv_s("COLUMN_INDEX_BUILD_BATCH_BYTES", "4096");
#else
    setenv("COLUMN_INDEX_BUILD_BATCH_BYTES", "4096", 1);
#endif
    OwnedTestRoot root("secondary-index-streaming");
    DatabaseEngine::init(root.dataRoot().string());
    struct Stop { ~Stop() { DatabaseEngine::stopNativeVectorIndexes(); LSM::stopBackgroundTasks(); WAL::shutdown(); } } stop;
    try {
        DatabaseEngine::ensureUserRoot("alice");
        DatabaseEngine::createDatabase("alice", "app", "document");
        DatabaseEngine::createCollection("alice", "app", "records");
        std::vector<json> docs;
        for (int i = 0; i < 2000; ++i)
            docs.push_back({{"id", "r-" + std::to_string(i)}, {"group", i % 7},
                            {"payload", std::string(4096, 'x')}});
        require(DatabaseEngine::insertMany("alice", "app", "records", docs).at("inserted") == 2000, "fixture insert");
        docs.clear(); docs.shrink_to_fit();
        LSM::flush("alice", "app", "records");
        const auto result = LSM::createSecondaryIndex("alice", "app", "records",
            {{"name", "group_idx"}, {"field", "group"}});
        if (!result.value("complete", false)) std::cerr << "BUILD_RESULT " << result.dump() << std::endl;
        require(result.value("complete", false), "index was not complete");
        require(result.value("build_strategy", "") == "streaming", "index build still materializes the entire collection");
        require(result.value("build_peak_batch_bytes", size_t{0}) > 0 &&
                result.at("build_peak_batch_bytes").get<size_t>() <= 4096, "unbounded index mutation batch");
        const auto dir = LSM::collectionArtifactPath("alice", "app", "records", ".idx") / "group.cidx";
        const auto segments = pacificdb::storage_v2::listIndexSegments(dir);
        require(segments.size() > 1, "bounded build did not produce multiple segments");
        size_t entries = 0;
        for (size_t i = 0; i < segments.size(); ++i) {
            pacificdb::storage_v2::IndexSegment segment; std::string error;
            require(pacificdb::storage_v2::readIndexSegment(segments[i], segment, &error), "cannot independently decode built segment");
            require(segment.snapshot == (i == 0), "snapshot/delta segment ordering changed");
            size_t bytes = 0;
            for (const auto& m : segment.mutations) { bytes += 16 + m.id.size() + m.value.size(); ++entries; }
            require(bytes <= 4096, "actual segment exceeds the configured mutation batch");
        }
        require(entries == 2000, "built index omitted or duplicated records");
        const auto rows = LSM::findByField("alice", "app", "records", "group", "1", 10000);
        require(rows.size() == 286, "index lookup cardinality");
        for (const auto& row : rows) require(row.at("group") == 1, "index points to a different value");
        // Simulate loss of an interrupted replica-local build. Replaying the
        // already committed definition must reconstruct it, not keep BUILDING.
        const auto catalogPath = LSM::collectionArtifactPath("alice", "app", "records", ".idx") / "_catalog.json";
        json interrupted; { std::ifstream input(catalogPath); input >> interrupted; }
        for (auto& def : interrupted["indexes"]) {
            def["status"] = "building"; def["complete"] = false;
        }
        { std::ofstream output(catalogPath); output << interrupted.dump(); }
        fs::remove_all(dir);
        require(LSM::createSecondaryIndex("alice", "app", "records", {{"name", "group_idx"}, {"field", "group"}}).value("complete", false), "committed interrupted index definition was not rebuilt");
        require(LSM::findByField("alice", "app", "records", "group", "1", 10000).size() == 286, "replayed build did not restore complete coverage");
        // A failed catalog write must not remove the currently usable generation.
        const auto idx = LSM::collectionArtifactPath("alice", "app", "records", ".idx");
        fs::create_directory(idx / "_catalog.json.tmp");
        require(LSM::rebuildSecondaryIndex("alice", "app", "records", "group_idx").value("status", "") == "error", "catalog failure was hidden");
        fs::remove(idx / "_catalog.json.tmp");
        require(LSM::findByField("alice", "app", "records", "group", "1", 10000).size() == 286, "failed rebuild destroyed previous generation");

        DatabaseEngine::createCollection("alice", "app", "versions");
        LSM::put("alice", "app", "versions", {{"id", "pending"}, {"version", 1}, {"group", "old"}, {"_visibility_state", "COMMITTED_VISIBLE"}});
        LSM::put("alice", "app", "versions", {{"id", "gone"}, {"version", 1}, {"group", "old"}});
        LSM::flush("alice", "app", "versions");
        LSM::put("alice", "app", "versions", {{"id", "pending"}, {"version", 2}, {"group", "new"}, {"_visibility_state", "PENDING"}});
        LSM::put("alice", "app", "versions", {{"id", "gone"}, {"version", 2}, {"_deleted", true}});
        LSM::flush("alice", "app", "versions");
        require(LSM::createSecondaryIndex("alice", "app", "versions", {{"name", "group_idx"}, {"field", "group"}}).value("complete", false), "versioned build failed");
        const auto visible = LSM::findByField("alice", "app", "versions", "group", "old", 100);
        require(visible.size() == 1 && visible.front().at("id") == "pending", "build changed visible-version/tombstone semantics");
        std::cout << "SECONDARY_INDEX_STREAMING_PASS\n";
    } catch (const std::exception& e) {
        std::cerr << "SECONDARY_INDEX_STREAMING_FAIL: " << e.what() << '\n'; return 1;
    }
}
