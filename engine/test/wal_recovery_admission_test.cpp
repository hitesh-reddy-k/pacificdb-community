#include "wal.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>

template<class F> void rejected(F operation) {
    bool rejected = false;
    try { operation(); } catch (const std::exception& e) {
        rejected = std::string(e.what()).find("wal_recovery_in_progress") != std::string::npos;
    }
    assert(rejected);
}
int main() {
#ifndef _WIN32
    setenv("WAL_GROUP_COMMIT", "false", 1);
    setenv("WAL_FSYNC_ENABLED", "true", 1);
#endif
    OwnedTestRoot root("wal-recovery-admission");
    const auto file = (root.dataRoot() / "collection.wal").string();
    const nlohmann::json record{{"op", "PUT"}, {"userId", "alice"}, {"db", "app"},
                      {"collection", "vectors"}, {"data", {{"id", "a"}}}};
    WAL::log(file, record);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto scan = std::async(std::launch::async, [&] {
        return WAL::scanRecovery(file, [&](const WalReplayRecord&) {
            entered.set_value(); released.wait(); return true;
        });
    });
    assert(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    // Fail before altering buffers or bytes; never wait holding a collection lock.
    rejected([&] { WAL::log(file, record); });
    rejected([&] { WAL::clear(file); });
    rejected([&] { WAL::flush(file, true); });
    std::string error;
    assert(!WAL::reclaimThrough(file, 1, &error) && error == "wal_recovery_in_progress");
    assert(WAL::scan(file, {}).status == WalScanStatus::IO_ERROR);
    assert(WAL::scanRecovery(file, {}).status == WalScanStatus::IO_ERROR);
    release.set_value();
    assert(scan.get().status == WalScanStatus::OK);
    assert(WAL::scan(file, {}).records == 1);
    WAL::log(file, record);
    // A failed recovery admission must not cancel an already admitted writer.
    std::promise<void> readerEntered, readerRelease;
    auto readerReleased = readerRelease.get_future().share();
    auto reader = std::async(std::launch::async, [&] {
        bool first = true;
        return WAL::scan(file, [&](const WalReplayRecord&) {
            if (first) { first = false; readerEntered.set_value(); readerReleased.wait(); }
            return true;
        });
    });
    assert(readerEntered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto busy = WAL::scanRecovery(file, {});
    assert(busy.status == WalScanStatus::IO_ERROR && busy.error == "wal_recovery_requires_idle_log");
    auto writer = std::async(std::launch::async, [&] { return WAL::log(file, record); });
    readerRelease.set_value();
    assert(reader.get().records == 2);
    assert(writer.get().lastLsn == 3);
    // Exceptions must release the exclusive reservation, including callbacks.
    bool threw = false;
    try {
        WAL::scanRecovery(file, [](const WalReplayRecord&) -> bool {
            throw std::runtime_error("injected callback error");
        });
    } catch (const std::exception&) { threw = true; }
    assert(threw);
    WAL::log(file, record);
    const auto final = WAL::scanRecovery(file, {});
    assert(final.status == WalScanStatus::OK && final.records == 4 && final.lastLsn == 4);
    std::cout << "WAL_RECOVERY_ADMISSION_PASS\n";
}
