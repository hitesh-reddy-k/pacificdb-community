#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "wal.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static json row(const std::string& id, int value, uint64_t commit) {
    return {
        {"nested", {{"z", value + 1}, {"a", value}}},
        {"value", value},
        {"id", id},
        {"_mvcc_version", commit},
        {"_raft_commit_index", commit},
        {"_visibility_floor", commit},
        {"committed", true},
        {"_visibility_state", "COMMITTED_VISIBLE"},
    };
}

int main() {
    const OwnedTestRoot ownedRoot("replica-digest");
    const std::string user = "digest_user";
    const std::string database = "digest_db";
    try {
#ifndef _WIN32
        setenv("WAL_GROUP_COMMIT", "false", 1);
        setenv("WAL_FSYNC_ENABLED", "false", 1);
#endif
        LSM::init(ownedRoot.dataRoot().string());

        LSM::put(user, database, "layout_memory", row("b", 2, 10));
        LSM::put(user, database, "layout_memory", row("a", 1, 10));

        LSM::put(user, database, "layout_sst", row("a", 1, 10));
        LSM::flush(user, database, "layout_sst");
        LSM::put(user, database, "layout_sst", row("b", 2, 10));
        LSM::flush(user, database, "layout_sst");

        const json memory = LSM::logicalDigest(
            user, database, "layout_memory", 10, 100);
        const json sst = LSM::logicalDigest(user, database, "layout_sst", 10, 100);
        require(memory.value("status", "") == "OK", "memory digest succeeds");
        require(memory.value("schema", "") == "pacificdb-logical-v1",
                "digest schema is explicit");
        require(memory.value("count", 0ULL) == 2, "digest counts live documents");
        require(memory.value("fence", 0ULL) == 10, "digest records fence");
        require(memory.value("bounded", false), "digest reports bounded scan");
        require(!memory.value("truncated", true), "complete digest is not truncated");
        require(memory.value("digest", "").size() == 64, "digest is SHA-256 hex");
        require(memory["digest"] == sst["digest"],
                "logical digest ignores insertion order and storage layout");

        LSM::put(user, database, "mutated", row("a", 999, 10));
        LSM::put(user, database, "mutated", row("b", 2, 10));
        const json mutated = LSM::logicalDigest(user, database, "mutated", 10, 100);
        require(mutated.value("digest", "") != memory.value("digest", ""),
                "mutating one logical value changes the digest");

        LSM::put(user, database, "history", row("one", 1, 5));
        LSM::flush(user, database, "history");
        const json beforeUpdate = LSM::logicalDigest(user, database, "history", 5, 100);
        LSM::put(user, database, "history", row("one", 2, 8));
        const json historical = LSM::logicalDigest(user, database, "history", 5, 100);
        const json current = LSM::logicalDigest(user, database, "history", 8, 100);
        require(historical.value("digest", "") == beforeUpdate.value("digest", ""),
                "later MVCC value is invisible before its fence");
        require(current.value("digest", "") != historical.value("digest", ""),
                "later MVCC value is visible at its fence");

        const json truncated = LSM::logicalDigest(
            user, database, "layout_memory", 10, 1);
        require(truncated.value("status", "") == "TRUNCATED",
                "maxDocs overflow is explicit");
        require(truncated.value("truncated", false), "truncation flag is true");
        require(!truncated.contains("digest"),
                "partial collection never emits a comparable digest");

        const json truncatedSst = LSM::logicalDigest(
            user, database, "layout_sst", 10, 1);
        require(truncatedSst.value("status", "") == "TRUNCATED",
                "SST maxDocs overflow is explicit");
        require(truncatedSst.value("truncated", false),
                "SST truncation flag is true");
        require(!truncatedSst.contains("digest"),
                "partial SST collection never emits a comparable digest");

        WAL::shutdown();
        std::cout << "REPLICA_DIGEST_PASS\n";
        return 0;
    } catch (const std::exception& error) {
        WAL::shutdown();
        std::cerr << "REPLICA_DIGEST_FAIL " << error.what() << '\n';
        return 1;
    }
}
