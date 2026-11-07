#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <iostream>

static json metadata() {
    const auto database = DatabaseEngine::getDatabaseMetadata("alice", "app");
    for (const auto& entry : database.at("collections"))
        if (entry.at("name") == "vectors") return entry;
    return {};
}
int main() {
    OwnedTestRoot root("collection-vector-identity");
    DatabaseEngine::init(root.dataRoot().string());
    DatabaseEngine::ensureUserRoot("alice");
    assert(DatabaseEngine::createDatabase("alice", "app", "vector"));
    json create{{"op", "CREATE_COLLECTION"}, {"userId", "alice"}, {"db", "app"},
                {"collection", "vectors"}, {"_raft_commit_index", 101}};
    assert(DatabaseEngine::applyReplicatedEntry(create));
    const auto first = metadata();
    assert(first.at("vector_incarnation") == "raft-create:101");
    assert(first.at("id").is_string() && first.at("id") != first.at("vector_incarnation"));
    assert(DatabaseEngine::createCollection("alice", "app", "vectors") == first.at("id").get<std::string>());
    assert(metadata() == first);
    const auto snapshot = LSM::exportSnapshotJson(101);
    assert(DatabaseEngine::applyReplicatedEntry(create));
    assert(metadata() == first);
    // Duplicate create entries are idempotent, not a new namespace incarnation.
    create["_raft_commit_index"] = 102;
    assert(DatabaseEngine::applyReplicatedEntry(create)); assert(metadata() == first);
    assert(DatabaseEngine::dropCollection("alice", "app", "vectors"));
    create["_raft_commit_index"] = 103;
    assert(DatabaseEngine::applyReplicatedEntry(create));
    assert(metadata().at("vector_incarnation") == "raft-create:103");
    assert(metadata().at("id") != first.at("id"));
    assert(LSM::applySnapshot(snapshot));
    assert(metadata() == first);
    // Legacy standalone metadata is preserved, never assigned a guessed identity.
    assert(DatabaseEngine::dropCollection("alice", "app", "vectors"));
    DatabaseEngine::createCollection("alice", "app", "vectors");
    const auto legacy = metadata(); assert(!legacy.contains("vector_incarnation"));
    create["_raft_commit_index"] = 104;
    assert(DatabaseEngine::applyReplicatedEntry(create)); assert(metadata() == legacy);
    std::cout << "COLLECTION_VECTOR_IDENTITY_PASS\n";
}
