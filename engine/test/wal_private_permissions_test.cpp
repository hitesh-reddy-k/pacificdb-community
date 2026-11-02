#include "wal_integrity.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

int main() {
    OwnedTestRoot root("wal-private-permissions");
#ifndef _WIN32
    const auto previousMask = ::umask(0);
    struct RestoreMask { mode_t value; ~RestoreMask() { ::umask(value); } } restore{previousMask};
#endif
    const auto data = root.dataRoot();
    const auto wal = data / "records.wal";
    WALRecord first;
    first.lsn = 1; first.txnId = 1; first.type = "INSERT";
    first.payload = {{"id", "one"}}; first.isCommitted = true;
    assert(WALIntegrity::writeRecordWithFsync(wal.string(), first));
    first.lsn = 2; first.payload = {{"id", "two"}};
    assert(WALIntegrity::writeRecordWithFsync(wal.string(), first));
    const auto recovered = WALIntegrity::scanAndTruncateWAL(wal.string());
    assert(recovered.size() == 2 && recovered[0].lsn == 1 && recovered[1].lsn == 2);
    WALIntegrity::updateWALMetadata(data.string(), {{"lastLSN", 2}});
    assert(WALIntegrity::getWALMetadata(data.string()).at("lastLSN") == 2);
    WALIntegrity::markCleanShutdown(data.string());
    assert(WALIntegrity::wasCleanShutdown(data.string()));
#ifndef _WIN32
    for (const auto& name : {"records.wal", WALIntegrity::WAL_METADATA_FILE,
                             WALIntegrity::CLEAN_SHUTDOWN_MARKER}) {
        struct stat info{};
        assert(::stat((data / name).c_str(), &info) == 0);
        assert((info.st_mode & 0777) == 0600);
    }
    const auto target = data / "preserved";
    { std::ofstream file(target); file << "preserve"; }
    const auto link = data / "linked.wal";
    std::filesystem::create_symlink(target, link);
    assert(!WALIntegrity::writeRecordWithFsync(link.string(), first));
    std::filesystem::remove(data / WALIntegrity::CLEAN_SHUTDOWN_MARKER);
    std::filesystem::create_symlink(target, data / WALIntegrity::CLEAN_SHUTDOWN_MARKER);
    bool rejected = false;
    try { WALIntegrity::markCleanShutdown(data.string()); } catch (...) { rejected = true; }
    assert(rejected);
    std::ifstream preserved(target); std::string value; preserved >> value;
    assert(value == "preserve");
#endif
    std::cout << "WAL_PRIVATE_CREATION_APPEND_RECOVERY_METADATA_MARKER_PASS\n";
}
