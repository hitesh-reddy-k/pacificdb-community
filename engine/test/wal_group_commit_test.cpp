#include "wal.hpp"
#include "owned_test_root.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

static void expect(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "[FAIL] " << msg << std::endl;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << std::endl;
}

static void setEnvKV(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

static void unsetEnvKV(const char* key) {
#ifdef _WIN32
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}

int main() {
    const OwnedTestRoot ownedRoot("wal-group-commit");
    const fs::path root = ownedRoot.dataRoot() / "wal_group_commit";
    fs::create_directories(root);

    const std::string walFile = (root / "events.wal").string();

    setEnvKV("WAL_FSYNC_ENABLED", "false");
    setEnvKV("WAL_FSYNC", "true");
    setEnvKV("WAL_GROUP_COMMIT", "true");
    setEnvKV("WAL_GROUP_COMMIT_MAX_MS", "5");
    setEnvKV("WAL_GROUP_COMMIT_MAX_BATCH", "64");
    setEnvKV("WAL_GROUP_COMMIT_MAX_BYTES", "65536");

    WAL::clear(walFile);
    WAL::init();

    const int groupedCount = 200;
    for (int i = 0; i < groupedCount; ++i) {
        json e = {
            {"userId", "wal_tester"},
            {"db", "testdb"},
            {"collection", "events"},
            {"data", {{"id", "g_" + std::to_string(i)}, {"v", i}}}
        };
        WAL::log(walFile, e);
    }

    WAL::shutdown();

    auto grouped = WAL::readAll(walFile);
    expect(static_cast<int>(grouped.size()) == groupedCount,
           "group commit mode persists all WAL entries");
    expect(WAL::getStats().entriesFsynced.load() == 0,
           "WAL_FSYNC_ENABLED is the canonical fsync switch");

    // A durable grouped call must not return while its entry exists only in the
    // process-local buffer. Synchronize the callers so they form one physical
    // batch, then observe the durability counter at each return boundary.
    WAL::clear(walFile);
    setEnvKV("WAL_FSYNC_ENABLED", "true");
    setEnvKV("WAL_GROUP_COMMIT", "true");
    setEnvKV("WAL_GROUP_COMMIT_MAX_MS", "200");
    const auto durableFsyncsBefore = WAL::getStats().entriesFsynced.load();
    const auto durableWritesBefore = WAL::getStats().entriesWritten.load();
    const auto durableAcksBefore = WAL::getStats().entriesAcknowledged.load();
    WAL::init();

    constexpr int durableCallers = 32;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<int> returnedBeforeFsync{0};
    std::vector<std::thread> writers;
    writers.reserve(durableCallers);
    for (int i = 0; i < durableCallers; ++i) {
        writers.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            WAL::log(walFile, {
                {"userId", "wal_tester"},
                {"db", "testdb"},
                {"collection", "events"},
                {"data", {{"id", "durable_" + std::to_string(i)}, {"v", i}}}
            });
            if (WAL::getStats().entriesFsynced.load() == durableFsyncsBefore) {
                returnedBeforeFsync.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != durableCallers) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& writer : writers) writer.join();

    expect(returnedBeforeFsync.load() == 0,
           "durable grouped calls return only after their fsync batch");
    expect(WAL::readAll(walFile).size() == durableCallers,
           "durable grouped acknowledgements are immediately readable");
    expect(WAL::getStats().entriesWritten.load() - durableWritesBefore == durableCallers &&
               WAL::getStats().entriesAcknowledged.load() - durableAcksBefore == durableCallers,
           "durable written and acknowledged counters match logical entries");
    WAL::shutdown();

    // Direct mode check
    WAL::clear(walFile);
    setEnvKV("WAL_GROUP_COMMIT", "false");
    setEnvKV("WAL_BATCH_INTERVAL_MS", "0");

    const int directCount = 25;
    for (int i = 0; i < directCount; ++i) {
        json e = {
            {"userId", "wal_tester"},
            {"db", "testdb"},
            {"collection", "events"},
            {"data", {{"id", "d_" + std::to_string(i)}, {"v", i}}}
        };
        WAL::log(walFile, e);
    }

    auto direct = WAL::readAll(walFile);
    expect(static_cast<int>(direct.size()) == directCount,
           "direct WAL mode persists all WAL entries");

    WAL::clear(walFile);
    const std::string binaryValue{"\x01\x89\xff", 3};
    WAL::log(walFile, {
        {"userId", "wal_tester"},
        {"db", "testdb"},
        {"collection", "events"},
        {"data", {{"id", "binary"}, {"value", binaryValue}}}
    });
    bool binaryRoundTripped = false;
    const auto binaryScan = WAL::scan(walFile, [&](const WalReplayRecord& record) {
        binaryRoundTripped = record.entry["data"]["value"].get<std::string>() == binaryValue;
        return true;
    });
    expect(binaryScan.status == WalScanStatus::OK && binaryRoundTripped,
           "binary JSON strings survive WAL checksum and replay");

    WAL::clear(walFile);
    std::vector<json> batchDocuments;
    batchDocuments.reserve(500);
    for (int i = 0; i < 500; ++i) {
        batchDocuments.push_back({{"id", "batch_" + std::to_string(i)}, {"v", i}});
    }
    const auto logicalBefore = WAL::getStats().entriesWritten.load();
    const auto physicalBefore = WAL::getStats().physicalRecordsWritten.load();
    const auto compactBatch = WAL::logPutBatch(
        walFile, "wal_tester", "testdb", "events", batchDocuments);
    bool compactBatchDecoded = false;
    const auto compactScan = WAL::scan(walFile, [&](const WalReplayRecord& record) {
        compactBatchDecoded = record.op == WalOp::BATCH_COMPRESSED &&
            record.entry.value("op", std::string()) == "PUT_BATCH" &&
            record.entry.contains("data") && record.entry["data"].size() == 500;
        return true;
    });
    expect(compactBatch.entries == 500 && compactBatch.physicalRecords == 1 &&
               WAL::getStats().entriesWritten.load() - logicalBefore == 500 &&
               WAL::getStats().physicalRecordsWritten.load() - physicalBefore == 1,
           "insertMany is encoded as one physical record with logical-entry accounting");
    expect(compactScan.status == WalScanStatus::OK && compactScan.records == 1 &&
               compactBatchDecoded,
           "contiguous binary insertMany record survives checksum and scan");

    WAL::clear(walFile);
    setEnvKV("WAL_GROUP_COMMIT", "true");
    setEnvKV("WAL_GROUP_COMMIT_MAX_MS", "200");
    setEnvKV("WAL_GROUP_COMMIT_MAX_BATCH", "2");
    WAL::init();
    const auto batchSyncsBefore = WAL::getStats().physicalSyncs.load();
    const auto batchRecordsBefore = WAL::getStats().physicalRecordsWritten.load();
    const auto commitGroupsBefore = WAL::getStats().batchesCommitted.load();
    std::atomic<int> batchReady{0};
    std::atomic<bool> batchStart{false};
    std::vector<std::thread> batchWriters;
    for (int writer = 0; writer < 2; ++writer) {
        batchWriters.emplace_back([&, writer] {
            std::vector<json> documents;
            for (int i = 0; i < 50; ++i) {
                documents.push_back({
                    {"id", "coalesced_" + std::to_string(writer) + "_" + std::to_string(i)},
                    {"v", i}
                });
            }
            batchReady.fetch_add(1, std::memory_order_release);
            while (!batchStart.load(std::memory_order_acquire)) std::this_thread::yield();
            WAL::logPutBatch(walFile, "wal_tester", "testdb", "events", documents);
        });
    }
    while (batchReady.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    batchStart.store(true, std::memory_order_release);
    for (auto& writer : batchWriters) writer.join();
    expect(WAL::getStats().physicalRecordsWritten.load() - batchRecordsBefore == 2 &&
               WAL::getStats().physicalSyncs.load() - batchSyncsBefore == 1 &&
               WAL::getStats().batchesCommitted.load() - commitGroupsBefore == 1,
           "concurrent insertMany requests share one append group and one fdatasync");
    WAL::shutdown();
    setEnvKV("WAL_GROUP_COMMIT", "false");

    // The canonical enable switch must override both the legacy alias and a
    // configured interval. Operators need a reliable off switch for incident
    // diagnosis and latency comparisons.
    WAL::clear(walFile);
    setEnvKV("WAL_GROUP_COMMIT_ENABLED", "false");
    setEnvKV("WAL_GROUP_COMMIT", "true");
    setEnvKV("WAL_GROUP_COMMIT_INTERVAL_MS", "200");
    unsetEnvKV("WAL_BATCH_INTERVAL_MS");
    WAL::init();
    const auto disabledSyncsBefore = WAL::getStats().physicalSyncs.load();
    const auto disabledGroupsBefore = WAL::getStats().batchesCommitted.load();
    const auto disabledCoalescedBefore = WAL::getStats().coalescedRequests.load();
    std::atomic<int> disabledReady{0};
    std::atomic<bool> disabledStart{false};
    std::vector<std::thread> disabledWriters;
    for (int writer = 0; writer < 2; ++writer) {
        disabledWriters.emplace_back([&, writer] {
            disabledReady.fetch_add(1, std::memory_order_release);
            while (!disabledStart.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            WAL::log(walFile, {
                {"userId", "wal_tester"},
                {"db", "testdb"},
                {"collection", "events"},
                {"data", {{"id", "disabled_" + std::to_string(writer)}}}
            });
        });
    }
    while (disabledReady.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    disabledStart.store(true, std::memory_order_release);
    for (auto& writer : disabledWriters) writer.join();
    expect(WAL::getStats().physicalSyncs.load() - disabledSyncsBefore == 2 &&
               WAL::getStats().batchesCommitted.load() - disabledGroupsBefore == 2 &&
               WAL::getStats().coalescedRequests.load() - disabledCoalescedBefore == 0,
           "WAL_GROUP_COMMIT_ENABLED=false disables coalescing despite an interval");
    WAL::shutdown();
    unsetEnvKV("WAL_GROUP_COMMIT_ENABLED");
    unsetEnvKV("WAL_GROUP_COMMIT_INTERVAL_MS");
    setEnvKV("WAL_GROUP_COMMIT", "false");

    WAL::clear(walFile);
    WAL::log(walFile, {{"userId", "wal_tester"}, {"data", {{"id", "after-clear"}}}});
    expect(WAL::readAll(walFile).size() == 1,
           "clear closes the persistent WAL handle before recreating the file");

    const auto failedBefore = WAL::getStats().entriesFailed.load();
    const fs::path blockedParent = root / "not-a-directory";
    std::ofstream(blockedParent) << "blocked";
    bool appendFailureReachedCaller = false;
    try {
        WAL::log((blockedParent / "events.wal").string(), {
            {"userId", "wal_tester"},
            {"db", "testdb"},
            {"collection", "events"},
            {"data", {{"id", "full"}}}
        });
    } catch (const std::runtime_error&) {
        appendFailureReachedCaller = true;
    }
    expect(appendFailureReachedCaller,
           "append failure reaches the caller before acknowledgement");
    expect(WAL::getStats().entriesFailed.load() - failedBefore == 1,
           "failed WAL entries are counted without acknowledgement");

    unsetEnvKV("WAL_FSYNC_ENABLED");
    unsetEnvKV("WAL_FSYNC");
    const auto fsyncsBefore = WAL::getStats().entriesFsynced.load();
    WAL::log(walFile, {{"userId", "wal_tester"}, {"data", {{"id", "durable-default"}}}});
    expect(WAL::getStats().entriesFsynced.load() > fsyncsBefore,
           "WAL fsync is enabled by default");
    WAL::shutdown();

    std::cout << "✅ wal_group_commit_test passed" << std::endl;
    return 0;
}
