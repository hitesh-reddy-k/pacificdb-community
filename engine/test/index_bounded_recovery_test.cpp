#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "storage_format_v2.hpp"
#include "wal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr int kRecords = 200000;
static const std::string kUser = "index_recovery_user";
static const std::string kDatabase = "index_recovery_db";
static const std::string kCollection = "documents";

static std::string fieldValue(char prefix, int index) {
    std::string value(72, static_cast<char>('a' + index % 26));
    const std::string key = std::string(1, prefix) + "-" + std::to_string(index) + "-";
    value.replace(0, key.size(), key);
    return value;
}

static json documentFor(int index) {
    return {{"id", "doc-" + std::to_string(index)},
            {"field_a", fieldValue('a', index)},
            {"field_b", fieldValue('b', index)},
            {"payload", std::string(256, static_cast<char>('A' + index % 26))}};
}

static int writeFixture(const fs::path& root) {
    setenv("WAL_GROUP_COMMIT", "false", 1);
    setenv("WAL_FSYNC_ENABLED", "false", 1);
    setenv("WAL_SEGMENT_MAX_BYTES", "8388608", 1);
    LSM::init(root.string());
    for (const auto& field : {"field_a", "field_b"}) {
        const auto result = LSM::createSecondaryIndex(
            kUser, kDatabase, kCollection,
            {{"name", std::string(field) + "_1"}, {"field", field}, {"type", "btree"}});
        if (result.value("status", std::string()) != "ok") return 2;
    }
    const fs::path wal = root / kUser / kDatabase / "wal" / (kCollection + ".wal");
    fs::create_directories(wal.parent_path());
    for (int start = 0; start < kRecords; start += 1000) {
        std::vector<json> batch;
        batch.reserve(1000);
        for (int index = start; index < std::min(start + 1000, kRecords); ++index) {
            batch.push_back({{"op", "PUT"}, {"userId", kUser}, {"db", kDatabase},
                             {"collection", kCollection}, {"data", documentFor(index)}});
        }
        WAL::logBatch(wal.string(), batch);
    }
    auto updated = documentFor(kRecords / 2);
    updated["field_a"] = fieldValue('u', kRecords / 2);
    WAL::logBatch(wal.string(), {
        {{"op", "UPDATE"}, {"userId", kUser}, {"db", kDatabase},
         {"collection", kCollection}, {"data", updated}},
        {{"op", "DELETE"}, {"userId", kUser}, {"db", kDatabase},
         {"collection", kCollection}, {"id", "doc-0"},
         {"data", {{"id", "doc-0"}, {"_deleted", true}}}}
    });
    WAL::shutdown();
    return 0;
}

static bool verifyField(const fs::path& directory, const std::string& field,
                        char prefix, bool updatedField, size_t& segmentCount) {
    const auto segments = pacificdb::storage_v2::listIndexSegments(directory);
    segmentCount = segments.size();
    std::unordered_set<int> ids;
    std::string error;
    const bool visited = pacificdb::storage_v2::visitIndexMutations(
        directory, [&](const pacificdb::storage_v2::IndexMutation& mutation) {
            if (mutation.op != pacificdb::storage_v2::IndexMutation::Op::Put ||
                mutation.id.rfind("doc-", 0) != 0) return false;
            int index = -1;
            try { index = std::stoi(mutation.id.substr(4)); } catch (...) { return false; }
            if (index <= 0 || index >= kRecords || !ids.insert(index).second) return false;
            const std::string expected = updatedField && index == kRecords / 2
                ? fieldValue('u', index) : fieldValue(prefix, index);
            return mutation.value == expected;
        }, &error);
    if (!visited || !error.empty() || ids.size() != static_cast<size_t>(kRecords - 1)) {
        std::cerr << "index verification failed for " << field << ": " << error
                  << " ids=" << ids.size() << '\n';
        return false;
    }
    return true;
}

static int recoverFixture(const fs::path& root) {
    LSM::init(root.string());
    LSM::restoreFromWal();
    if (!LSM::indexRecoverySettled()) return 3;
#ifndef _WIN32
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const uint64_t peakRssBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#else
    const uint64_t peakRssBytes = 0;
#endif
    const fs::path idx = root / kUser / kDatabase / (kCollection + ".idx");
    size_t segmentsA = 0, segmentsB = 0;
    if (!verifyField(idx / "field_a.cidx", "field_a", 'a', true, segmentsA) ||
        !verifyField(idx / "field_b.cidx", "field_b", 'b', false, segmentsB)) return 4;
    if (!LSM::findByField(kUser, kDatabase, kCollection, "field_a",
                          fieldValue('a', kRecords / 2)).empty() ||
        LSM::findByField(kUser, kDatabase, kCollection, "field_a",
                         fieldValue('u', kRecords / 2)).size() != 1 ||
        !LSM::findByField(kUser, kDatabase, kCollection, "field_b",
                          fieldValue('b', 0)).empty()) return 5;
    std::ofstream(root / "index-recovery-result.json") << json{
        {"peak_rss_bytes", peakRssBytes}, {"segments_a", segmentsA},
        {"segments_b", segmentsB}, {"entries_per_index", kRecords - 1},
        {"recovery", LSM::indexRecoveryStatus()}
    }.dump();
    return 0;
}

static std::string quote(const fs::path& path) { return "'" + path.string() + "'"; }

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write") return writeFixture(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--recover") return recoverFixture(argv[2]);
    try {
        const OwnedTestRoot ownedRoot("index-bounded-recovery");
        const fs::path executable = fs::canonical(argv[0]);
        const fs::path root = ownedRoot.dataRoot();
        if (std::system((quote(executable) + " --write " + quote(root)).c_str()) != 0) {
            throw std::runtime_error("fixture writer failed");
        }
        setenv("LSM_MEMTABLE_LIMIT", "5000", 1);
        setenv("INDEX_REBUILD_BATCH_BYTES", "8388608", 1);
        if (std::system((quote(executable) + " --recover " + quote(root)).c_str()) != 0) {
            throw std::runtime_error("recovery child failed");
        }
        json result;
        std::ifstream(root / "index-recovery-result.json") >> result;
        if (result.value("entries_per_index", 0) != kRecords - 1 ||
            result.value("segments_a", 0) < 2 || result.value("segments_b", 0) < 2) {
            throw std::runtime_error("index rebuild was incomplete or not chunked");
        }
#ifndef _WIN32
        if (result.value("peak_rss_bytes", uint64_t{0}) >= 768ULL * 1024 * 1024) {
            throw std::runtime_error("index recovery exceeded 768 MiB RSS");
        }
#endif
        std::cout << "index_bounded_recovery_test passed: " << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "index_bounded_recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
