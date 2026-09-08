#include "data_durability.hpp"
#include "lsm.hpp"
#include "lsm_manifest.hpp"
#include "owned_test_root.hpp"
#include "wal.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr int kRecords = 200000;
static const std::string kUser = "recovery_user";
static const std::string kDatabase = "recovery_db";
static const std::string kCollection = "documents";

static std::string payloadFor(int index) {
    std::string payload(1024, static_cast<char>('a' + index % 26));
    const std::string prefix = std::to_string(index) + ":";
    payload.replace(0, prefix.size(), prefix);
    return payload;
}

static uint32_t payloadHash(int index) {
    return pacificdb::durability::ChecksumCalculator::crc32(payloadFor(index));
}

static std::string updatedPayload() {
    return std::string(1024, 'z');
}

static int writeFixture(const fs::path& root) {
    setenv("WAL_GROUP_COMMIT", "false", 1);
    setenv("WAL_BATCH_INTERVAL_MS", "0", 1);
    setenv("WAL_FSYNC_ENABLED", "false", 1);
    setenv("WAL_SEGMENT_MAX_BYTES", "8388608", 1);
    const fs::path wal = root / kUser / kDatabase / "wal" / (kCollection + ".wal");
    fs::create_directories(wal.parent_path());
    for (int start = 0; start < kRecords; start += 1000) {
        std::vector<json> batch;
        batch.reserve(1000);
        for (int index = start; index < std::min(start + 1000, kRecords); ++index) {
            batch.push_back({
                {"op", "PUT"}, {"userId", kUser}, {"db", kDatabase},
                {"collection", kCollection},
                {"data", {{"id", "doc-" + std::to_string(index)},
                           {"payload", payloadFor(index)}}}
            });
        }
        WAL::logBatch(wal.string(), batch);
    }
    LSM::init(root.string());
    LSM::put(kUser, kDatabase, kCollection,
             {{"id", "doc-" + std::to_string(kRecords / 2)},
              {"payload", updatedPayload()}});
    LSM::del(kUser, kDatabase, kCollection, "doc-0");
    WAL::shutdown();
    return 0;
}

static int recoverFixture(const fs::path& root) {
    LSM::init(root.string());
    LSM::restoreFromWal();
    size_t streamedCount = 0;
    std::string previousId;
    bool ordered = true;
    if (!LSM::visitLatest(kUser, kDatabase, kCollection,
                          [&](const LSM::LatestRowView& row) {
                              const auto id = row.field("id");
                              if (!id || !id->is_string() ||
                                  (!previousId.empty() && id->get<std::string>() <= previousId)) {
                                  ordered = false;
                              } else {
                                  previousId = id->get<std::string>();
                              }
                              ++streamedCount;
                              return true;
                          }) || !ordered) return 4;
#ifndef _WIN32
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const uint64_t streamedPeakRssBytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#else
    const uint64_t streamedPeakRssBytes = 0;
#endif
    const auto documents = LSM::getAll(kUser, kDatabase, kCollection);
    json hashes = json::object();
    for (const int index : {1, kRecords / 2, kRecords - 1}) {
        const auto found = LSM::findByField(
            kUser, kDatabase, kCollection, "id", "doc-" + std::to_string(index));
        if (found.size() != 1) return 2;
        hashes[std::to_string(index)] =
            pacificdb::durability::ChecksumCalculator::crc32(
                found.front().value("payload", std::string()));
    }
    if (!LSM::findByField(kUser, kDatabase, kCollection, "id", "doc-0").empty()) return 3;
    const auto manifest = LsmManifestStore::load(
        root / kUser / kDatabase / (kCollection + ".lsm"));
    std::ofstream(root / "bounded-recovery-result.json") << json{
        {"count", documents.size()},
        {"streamed_count", streamedCount},
        {"hashes", hashes},
        {"manifest_generation", manifest.status == LsmManifestLoadStatus::OK
                                    ? manifest.manifest.generation : 0},
        {"streamed_peak_rss_bytes", streamedPeakRssBytes}
    }.dump();
    return 0;
}

static std::string quote(const fs::path& path) {
    return "'" + path.string() + "'";
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write") return writeFixture(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--recover") return recoverFixture(argv[2]);
    try {
        const OwnedTestRoot ownedRoot("lsm-bounded-recovery");
        const fs::path executable = fs::canonical(argv[0]);
        const fs::path root = ownedRoot.dataRoot();
        if (std::system((quote(executable) + " --write " + quote(root)).c_str()) != 0) {
            throw std::runtime_error("fixture writer failed");
        }
        setenv("LSM_MEMTABLE_LIMIT", "5000", 1);
        if (std::system((quote(executable) + " --recover " + quote(root)).c_str()) != 0) {
            throw std::runtime_error("recovery child failed");
        }
        json result;
        std::ifstream(root / "bounded-recovery-result.json") >> result;
        if (result.value("count", 0) != kRecords - 1 ||
            result.value("streamed_count", 0) != kRecords - 1) {
            throw std::runtime_error("count mismatch");
        }
        for (const int index : {1, kRecords / 2, kRecords - 1}) {
            const uint32_t expected = index == kRecords / 2
                ? pacificdb::durability::ChecksumCalculator::crc32(updatedPayload())
                : payloadHash(index);
            if (result["hashes"].value(std::to_string(index), uint32_t{0}) != expected) {
                throw std::runtime_error("payload hash mismatch");
            }
        }
        if (result.value("manifest_generation", uint64_t{0}) < 30) {
            throw std::runtime_error("recovery did not checkpoint in bounded batches");
        }
#ifndef _WIN32
        if (result.value("streamed_peak_rss_bytes", uint64_t{0}) >= 192ULL * 1024 * 1024) {
            throw std::runtime_error("streamed visit exceeded 192 MiB RSS: " +
                std::to_string(result.value("streamed_peak_rss_bytes", uint64_t{0})));
        }
#endif
        std::cout << "lsm_bounded_recovery_test passed: " << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lsm_bounded_recovery_test failed: " << error.what() << '\n';
        return 1;
    }
}
