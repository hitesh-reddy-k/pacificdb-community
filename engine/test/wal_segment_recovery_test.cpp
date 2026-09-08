#include "owned_test_root.hpp"
#include "wal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

static void expect(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(1);
    }
    std::cout << "[PASS] " << message << '\n';
}

static void setEnv(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

static void writeLegacyWal(const fs::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    expect(out.good(), "legacy WAL fixture opens");
    for (std::uint64_t lsn = 1; lsn <= 5; ++lsn) {
        json entry = {
            {"_wal_seq", lsn},
            {"userId", "wal_tester"},
            {"db", "testdb"},
            {"collection", "events"},
            {"data", {{"id", "legacy_" + std::to_string(lsn)}}}
        };
        const auto bytes = json::to_msgpack(entry);
        const auto op = WalOp::INSERT;
        const auto size = static_cast<std::uint32_t>(bytes.size());
        out.write(reinterpret_cast<const char*>(&op), sizeof(op));
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    expect(out.good(), "legacy WAL fixture is complete");
}

static std::vector<fs::path> segmentFiles(const fs::path& logicalWal) {
    std::vector<fs::path> files;
    const fs::path directory = logicalWal.string() + ".segments";
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wal") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

static void copyWal(const fs::path& source, const fs::path& destination) {
    fs::copy_file(source, destination);
    fs::copy(source.string() + ".segments", destination.string() + ".segments",
             fs::copy_options::recursive);
}

int main() {
    const OwnedTestRoot ownedRoot("wal-segment-recovery");
    const fs::path root = ownedRoot.dataRoot() / "wal_segment_recovery";
    fs::create_directories(root);
    const fs::path logicalWal = root / "events.wal";

    setEnv("WAL_GROUP_COMMIT", "false");
    setEnv("WAL_BATCH_INTERVAL_MS", "0");
    setEnv("WAL_FSYNC_ENABLED", "false");
    setEnv("WAL_COMPRESSION_ENABLED", "false");
    setEnv("WAL_SEGMENT_MAX_BYTES", "4096");
    writeLegacyWal(logicalWal);

    bool appendLsnsContiguous = true;
    for (int i = 0; i < 500; ++i) {
        const auto result = WAL::log(logicalWal.string(), {
            {"userId", "wal_tester"},
            {"db", "testdb"},
            {"collection", "events"},
            {"data", {{"id", "segmented_" + std::to_string(i)},
                      {"payload", std::string(80, static_cast<char>('a' + (i % 26)))}}}
        });
        appendLsnsContiguous = appendLsnsContiguous &&
            result.firstLsn == static_cast<std::uint64_t>(i + 6);
    }
    expect(appendLsnsContiguous, "append returns contiguous collection-local LSNs");
    WAL::shutdown();

    expect(WAL::getSegmentCount(logicalWal.string()) > 1,
           "small threshold rotates segments");
    const auto rotatedSegments = segmentFiles(logicalWal);
    bool sealedSegmentsBounded = true;
    for (size_t i = 0; i + 1 < rotatedSegments.size(); ++i) {
        sealedSegmentsBounded = sealedSegmentsBounded && fs::file_size(rotatedSegments[i]) <= 4096;
    }
    expect(sealedSegmentsBounded, "sealed segments respect the configured byte threshold");
    std::uint64_t expectedLsn = 1;
    bool scannedLsnsContiguous = true;
    auto clean = WAL::scan(logicalWal.string(), [&](const WalReplayRecord& record) {
        scannedLsnsContiguous = scannedLsnsContiguous && record.lsn == expectedLsn++;
        return true;
    });
    expect(scannedLsnsContiguous, "scan yields contiguous collection-local LSNs");
    expect(clean.status == WalScanStatus::OK, "legacy plus segments scan cleanly");
    expect(clean.records == 505, "scan visits every logical record");

    const fs::path tailWal = root / "tail.wal";
    const fs::path corruptWal = root / "corrupt.wal";
    const fs::path reclaimWal = root / "reclaim.wal";
    copyWal(logicalWal, tailWal);
    copyWal(logicalWal, corruptWal);
    copyWal(logicalWal, reclaimWal);

    auto tailSegments = segmentFiles(tailWal);
    expect(!tailSegments.empty(), "tail fixture has segments");
    {
        std::ofstream out(tailSegments.back(), std::ios::binary | std::ios::app);
        out.write("bad", 3);
    }
    std::uint64_t tailVisited = 0;
    auto tail = WAL::scan(tailWal.string(), [&](const WalReplayRecord&) {
        ++tailVisited;
        return true;
    });
    expect(tail.status == WalScanStatus::PARTIAL_TAIL,
           "partial final segment frame is classified as tail");
    expect(tail.records == 505 && tailVisited == 505,
           "partial tail preserves the valid prefix");
    const auto afterTail = WAL::log(tailWal.string(), {
        {"userId", "wal_tester"}, {"db", "testdb"}, {"collection", "events"},
        {"data", {{"id", "after_tail"}}}
    });
    expect(afterTail.firstLsn == 506,
           "append truncates only the partial tail and continues the LSN sequence");
    auto repairedTail = WAL::scan(tailWal.string(), [](const WalReplayRecord&) { return true; });
    expect(repairedTail.status == WalScanStatus::OK && repairedTail.records == 506,
           "tail repair preserves all acknowledged records");

    auto corruptSegments = segmentFiles(corruptWal);
    expect(corruptSegments.size() > 1, "corruption fixture has a sealed segment");
    {
        std::fstream file(corruptSegments.front(), std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(32 + sizeof(WalOp));
        std::uint32_t payloadSize = 0;
        file.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
        expect(payloadSize > 16, "first segmented payload is large enough to corrupt");
        const auto corruptOffset =
            32 + sizeof(WalOp) + sizeof(payloadSize) + payloadSize / 2;
        file.seekg(corruptOffset);
        char byte = 0;
        file.read(&byte, 1);
        file.seekp(corruptOffset);
        byte ^= 0x5a;
        file.write(&byte, 1);
    }
    auto corrupt = WAL::scan(corruptWal.string(), [](const WalReplayRecord&) { return true; });
    expect(corrupt.status == WalScanStatus::CORRUPT,
           "interior segment corruption fails closed");
    std::string reclaimError;
    expect(!WAL::reclaimThrough(corruptWal.string(), 505, &reclaimError),
           "corrupt WAL cannot be reclaimed");

    expect(WAL::reclaimThrough(reclaimWal.string(), 250, &reclaimError),
           "covered legacy and sealed segments are reclaimed");
    std::uint64_t firstRemaining = 0;
    auto reclaimed = WAL::scan(reclaimWal.string(), [&](const WalReplayRecord& record) {
        if (firstRemaining == 0) firstRemaining = record.lsn;
        return true;
    });
    expect(reclaimed.status == WalScanStatus::OK && reclaimed.records > 0 &&
               reclaimed.records < 505 && firstRemaining > 1 && reclaimed.lastLsn == 505,
           "reclaim preserves every later segment record");

    const fs::path legacyOnlyWal = root / "legacy_only.wal";
    writeLegacyWal(legacyOnlyWal);
    expect(WAL::reclaimThrough(legacyOnlyWal.string(), 5, &reclaimError),
           "fully covered legacy WAL is reclaimed");
    WAL::shutdown();
    const auto afterLegacyCheckpoint = WAL::log(legacyOnlyWal.string(), {
        {"userId", "wal_tester"}, {"db", "testdb"}, {"collection", "events"},
        {"data", {{"id", "after_legacy_checkpoint"}}}
    });
    expect(afterLegacyCheckpoint.firstLsn == 6,
           "append after full legacy reclaim continues beyond the checkpoint LSN");

    const fs::path clearWal = root / "clear_race.wal";
    setEnv("WAL_GROUP_COMMIT", "true");
    setEnv("WAL_GROUP_COMMIT_MAX_MS", "500");
    WAL::init();
    std::atomic<bool> clearRejectedWriter{false};
    std::thread queuedWriter([&] {
        try {
            WAL::log(clearWal.string(), {
                {"userId", "wal_tester"}, {"db", "testdb"}, {"collection", "events"},
                {"data", {{"id", "must_not_survive_clear"}}}
            });
        } catch (const std::runtime_error&) {
            clearRejectedWriter.store(true);
        }
    });
    while (WAL::getPendingCount() == 0) std::this_thread::yield();
    WAL::clear(clearWal.string());
    queuedWriter.join();
    WAL::shutdown();
    expect(clearRejectedWriter.load(), "clear rejects an append queued before removal");
    expect(WAL::scan(clearWal.string(), [](const WalReplayRecord&) { return true; }).records == 0,
           "clear cannot race a queued append into a recreated WAL");

    std::cout << "wal_segment_recovery_test passed\n";
    return 0;
}
