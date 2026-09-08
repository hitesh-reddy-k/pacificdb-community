#include "database_engine.hpp"
#include "lsm.hpp"
#include "lsm_manifest.hpp"
#include "owned_test_root.hpp"
#include "raft_core.hpp"
#include "storage_format_v2.hpp"

#include <cstdlib>
#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

static void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << std::endl;
        std::exit(1);
    }
    std::cout << "[PASS] " << message << std::endl;
}

static size_t countSstFiles(const fs::path& dir) {
    if (!fs::exists(dir)) return 0;
    size_t count = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".sst") count++;
    }
    return count;
}

static bool anySstContains(const fs::path& dir, const std::string& needle) {
    if (!fs::exists(dir)) return false;
    for (auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".sst") continue;
        if (pacificdb::storage_v2::isBinarySst(e.path())) {
            bool found = false;
            std::string error;
            expect(pacificdb::storage_v2::scanSst(e.path(), [&](json&& row) {
                found = row.contains(needle);
                return !found;
            }, &error), "binary SST is readable: " + error);
            if (found) return true;
            continue;
        }
        std::ifstream in(e.path());
        std::string line;
        while (std::getline(in, line)) if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

int main() {
    setenv("LSM_COMPACTION_FANOUT", "4", 1);

    const OwnedTestRoot ownedRoot("lsm-compaction");
    const std::string root = ownedRoot.dataRoot().string();
#ifndef _WIN32
    setenv("DATA_DIR", root.c_str(), 1);
    setenv("DATA_ROOT", root.c_str(), 1);
#else
    _putenv_s("DATA_DIR", root.c_str());
    _putenv_s("DATA_ROOT", root.c_str());
#endif

    const std::string user = "gc_tester";
    const std::string db = "gc_db";
    const std::string coll = "docs";
    const std::string tiered = "tiered";
    DatabaseEngine::init(root);
    LSM::init(root);
    DatabaseEngine::ensureUserRoot(user);
    DatabaseEngine::createDatabase(user, db, "binary");
    DatabaseEngine::createCollection(user, db, coll);
    DatabaseEngine::createCollection(user, db, tiered);

    DatabaseEngine::insert(user, db, coll, {{"id", "doc1"}, {"value", "old"}});
    LSM::flush(user, db, coll);
    bool deleted = DatabaseEngine::deleteOne(user, db, coll, {{"id", "doc1"}});
    expect(deleted, "delete writes tombstone");
    LSM::flush(user, db, coll);

    DatabaseEngine::insert(user, db, coll, {{"id", "live-0"}, {"value", 0}});
    LSM::flush(user, db, coll);

    fs::path lsmDir = fs::path(root) / user / db / (coll + ".lsm");
    expect(countSstFiles(lsmDir) == 3, "test produced three SSTs below fanout");
    expect(anySstContains(lsmDir, "_deleted"), "tombstone exists before compaction");

    fs::path probeSst;
    for (const auto& entry : fs::directory_iterator(lsmDir)) {
        if (entry.path().extension() == ".sst") { probeSst = entry.path(); break; }
    }
    std::string absentId;
    for (int i = 0; i < 2048 && absentId.empty(); ++i) {
        const std::string candidate = "missing-" + std::to_string(i);
        if (!LSM::mayExistInSST(probeSst.string(), candidate)) absentId = candidate;
    }
    expect(!absentId.empty(), "bloom filter skips a missing id");
    const auto bloomCacheHits = LSM::getLsmMetrics().value("bloom_cache_hit", 0ULL);
    expect(!LSM::mayExistInSST(probeSst.string(), absentId) &&
               LSM::getLsmMetrics().value("bloom_cache_hit", 0ULL) > bloomCacheHits,
           "parsed bloom filter is reused from cache");

    fs::path tierDir = fs::path(root) / user / db / (tiered + ".lsm");
    for (int i = 0; i < 64; ++i) {
        std::string padding(4096, '\0');
        for (size_t j = 0; j < padding.size(); ++j) {
            padding[j] = static_cast<char>(1 + ((i * 131 + j * 17) % 250));
        }
        LSM::put(user, db, tiered,
                 {{"id", "large-" + std::to_string(i)}, {"padding", padding}});
    }
    LSM::flush(user, db, tiered);
    fs::path largeTierSst;
    for (const auto& entry : fs::directory_iterator(tierDir)) {
        if (entry.path().extension() == ".sst") largeTierSst = entry.path();
    }
    for (int i = 0; i < 3; ++i) {
        LSM::put(user, db, tiered, {{"id", "small-" + std::to_string(i)}});
        LSM::flush(user, db, tiered);
    }
    uintmax_t largestSmall = 0;
    for (const auto& entry : fs::directory_iterator(tierDir)) {
        if (entry.path().extension() == ".sst" && entry.path() != largeTierSst) {
            largestSmall = std::max(largestSmall, fs::file_size(entry.path()));
        }
    }
    expect(fs::file_size(largeTierSst) > largestSmall * 2,
           "tier fixture has one SST larger than twice the small tier");

    // Compaction is deliberately fenced until Raft recovery establishes a
    // trustworthy commit watermark. Initialize only after arranging the local
    // fixture so setup writes remain standalone and the actual compaction runs
    // behind the same recovery gate used in production.
    RaftCore::instance().init({}, "lsm-compaction-test", 0, true);

    const auto runsBeforeFanout = LSM::getLsmMetrics().value("compaction_runs_total", 0ULL);
    LSM::compact(user, db, coll);
    expect(countSstFiles(lsmDir) == 3, "three SSTs do not compact below fanout four");
    expect(LSM::getLsmMetrics().value("compaction_runs_total", 0ULL) == runsBeforeFanout,
           "below-fanout compaction does not count a run");

    LSM::compact(user, db, tiered);
    expect(countSstFiles(tierDir) == 4,
           "a differently-sized SST is excluded from a three-file small tier");

    LSM::put(user, db, coll, {{"id", "live-1"}, {"value", 1}});
    LSM::flush(user, db, coll);

    std::atomic<bool> stopReader{false};
    std::atomic<bool> readerStarted{false};
    std::exception_ptr readerError;
    std::thread reader([&] {
        readerStarted = true;
        try {
            while (!stopReader) (void)LSM::getAll(user, db, coll);
        } catch (...) {
            readerError = std::current_exception();
        }
    });
    while (!readerStarted) std::this_thread::yield();
    LSM::compact(user, db, coll);
    stopReader = true;
    reader.join();

    auto docs = LSM::getAll(user, db, coll);
    expect(!readerError, "concurrent reads keep their SST file set through compaction");
    expect(docs.size() == 2, "deleted document stays deleted and live documents survive compaction");
    expect(countSstFiles(lsmDir) == 1, "fanout compaction collapsed four similarly-sized SSTs");
    std::vector<fs::path> publishedAfterCompaction;
    std::string manifestError;
    expect(LsmManifestStore::publishedSsts(
               lsmDir, publishedAfterCompaction, &manifestError) &&
               publishedAfterCompaction.size() == 1,
           "compaction publishes exactly one authoritative SST generation");
    expect(!anySstContains(lsmDir, "_deleted"), "full-coverage compaction reclaimed tombstone");

    json overlay;
    for (const auto& doc : docs) {
        if (doc.value("id", "") == "live-0") overlay = doc;
    }
    overlay["value"] = "memtable";
    LSM::put(user, db, coll, overlay);
    size_t visited = 0;
    bool sawOverlay = false;
    LSM::visitLatest(user, db, coll, [&](const LSM::LatestRowView& row) {
        ++visited;
        const auto id = row.field("id");
        const auto value = row.field("value");
        if (id && *id == "live-0") {
            sawOverlay = value && *value == "memtable" &&
                         row.materialize().value("value", "") == "memtable";
        }
        return true;
    });
    expect(visited == 2 && sawOverlay,
           "streaming latest-row visitor applies tombstones and memtable overlays");
    LSM::flush(user, db, coll);
    json stale = overlay;
    stale["value"] = "stale";
    stale["_mvcc_version"] = 0;
    stale["version"] = 0;
    LSM::put(user, db, coll, stale);
    LSM::flush(user, db, coll);
    sawOverlay = false;
    LSM::visitLatest(user, db, coll, [&](const LSM::LatestRowView& row) {
        const auto id = row.field("id");
        if (id && *id == "live-0") sawOverlay = row.field("value") == json("memtable");
        return true;
    });
    expect(sawOverlay, "streaming latest-row visitor keeps the highest MVCC version");

    const auto ingestedBeforeMutation =
        LSM::getLsmMetrics().value("bytes_ingested", 0ULL);
    LSM::put(user, db, tiered, {{"id", "small-3"}});
    expect(LSM::getLsmMetrics().value("bytes_ingested", 0ULL) > ingestedBeforeMutation,
           "logical ingested bytes advance before an SST flush");
    LSM::flush(user, db, tiered);
    LSM::compact(user, db, tiered);
    expect(countSstFiles(tierDir) == 2 && fs::exists(largeTierSst),
           "four small SSTs compact without rewriting the large tier");

    auto metrics = LSM::getLsmMetrics();
    expect(metrics.value("compaction_tombstones_reclaimed", 0ULL) >= 1,
           "reclaimed tombstone metric increments");
    expect(metrics.value("bytes_ingested", 0ULL) > 0,
           "applied mutation bytes are counted as logical ingested bytes");
    expect(metrics.value("compaction_bytes_read", 0ULL) > 0,
           "compaction input bytes are counted");
    expect(metrics.value("compaction_bytes_written", 0ULL) > 0,
           "compaction output bytes are counted");
    expect(metrics.value("compaction_write_amplification", 0.0) > 0.0,
           "compaction write amplification is exported");
    const auto compactionReadBytes = metrics.value("compaction_bytes_read", 0ULL);
    (void)LSM::getAll(user, db, coll);
    expect(LSM::getLsmMetrics().value("compaction_bytes_read", 0ULL) == compactionReadBytes,
           "ordinary reads do not increment compaction bytes");

    std::cout << "LSM_COMPACTION_TOMBSTONE_GC_PASS" << std::endl;
    return 0;
}
