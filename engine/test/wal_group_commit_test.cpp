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
