#include "database_engine.hpp"
#include "owned_test_root.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

json security(std::initializer_list<const char*> owners,
              json grants = json::object()) {
    json ownerList = json::array();
    for (const char* owner : owners) ownerList.push_back(owner);
    return {{"version", 1}, {"owners", std::move(ownerList)},
            {"grants", std::move(grants)}};
}

}  // namespace

int main() {
    const OwnedTestRoot root("database-access");
    DatabaseEngine::init(root.dataRoot().string(), false);

    const json alice = security({"alice"});
    require(DatabaseEngine::createDatabase("system", "owned", "binary", alice),
            "owned database creation failed");
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") == alice,
            "creator ownership did not round-trip");

    const json bob = security({"bob"});
    require(DatabaseEngine::createDatabase("system", "owned", "binary", bob),
            "idempotent database creation failed");
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") == alice,
            "idempotent create replaced the original owner");

    require(DatabaseEngine::createDatabase("system", "legacy"),
            "legacy database creation failed");
    require(DatabaseEngine::getDatabaseSecurity("system", "legacy") ==
                security({}),
            "missing security metadata was not reported as unassigned");

    require(DatabaseEngine::applyReplicatedEntry({
                {"action", "createDatabase"}, {"op", "createDatabase"},
                {"legacyOp", "CREATE_DB"}, {"userId", "system"},
                {"db", "replicated"}, {"dbType", "binary"},
                {"security", bob}}),
            "replicated owned database creation failed");
    require(DatabaseEngine::getDatabaseSecurity("system", "replicated") == bob,
            "replicated create lost ownership metadata");

    require(!DatabaseEngine::setDatabaseSecurity("system", "owned", security({})),
            "removing the final owner was accepted");
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") == alice,
            "rejected ownership update changed metadata");
    require(!DatabaseEngine::setDatabaseSecurity(
                "system", "owned",
                {{"version", 2}, {"owners", {"alice"}},
                 {"grants", json::object()}}),
            "unknown database security version was accepted");
    require(!DatabaseEngine::setDatabaseSecurity(
                "system", "owned",
                {{"version", 1}, {"owners", {"alice", "alice"}},
                 {"grants", json::object()}}),
            "duplicate database owners were accepted");
    require(!DatabaseEngine::setDatabaseSecurity(
                "system", "owned",
                {{"version", 1}, {"owners", {"bad\nowner"}},
                 {"grants", json::object()}}),
            "control characters in an owner were accepted");
    require(!DatabaseEngine::setDatabaseSecurity(
                "system", "owned",
                {{"version", 1}, {"owners", {"alice"}},
                 {"grants", {{"bob", "administrator"}}}}),
            "unknown database grant level was accepted");
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") == alice,
            "invalid security updates changed metadata");

    const json shared = security(
        {"alice", "bob"}, {{"carol", "read-only"}, {"dave", "read-write"}});
    require(DatabaseEngine::setDatabaseSecurity("system", "owned", shared),
            "co-owner/grant update failed");
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") == shared,
            "co-owner/grant update did not round-trip");

    const json transferred = security({"carol"}, {{"alice", "read-write"}});
    require(DatabaseEngine::setDatabaseSecurity("system", "owned", transferred),
            "ownership transfer failed");
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") == transferred,
            "ownership transfer did not persist");

    const json replicatedUpdate = security({"dave"}, {{"carol", "read-only"}});
    require(DatabaseEngine::applyReplicatedEntry({
                {"action", "setDatabaseSecurity"},
                {"op", "SET_DATABASE_SECURITY"}, {"userId", "system"},
                {"db", "owned"}, {"security", replicatedUpdate}}),
            "replicated ownership update failed");
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") ==
                replicatedUpdate,
            "replicated ownership update did not persist");

    DatabaseEngine::configureStorageRoot(root.dataRoot().string());
    require(DatabaseEngine::getDatabaseSecurity("system", "owned") ==
                replicatedUpdate,
            "security metadata did not survive storage reopen");

    std::cout << "DATABASE_ACCESS_PASS\n";
    return 0;
}
