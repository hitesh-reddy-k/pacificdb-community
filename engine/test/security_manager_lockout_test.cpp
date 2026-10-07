#include "owned_test_root.hpp"
#include "security_manager.hpp"

#include <cassert>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

using pacificdb::security::Role;
using pacificdb::security::SecurityManager;

int main() {
    const OwnedTestRoot root("security-manager-lockout");
    auto& security = SecurityManager::instance();
    security.initialize(root.dataRoot().string());
    security.setMaxLoginAttempts(2);
    security.setLockoutDurationMinutes(30);

    assert(security.createUser("alice", "correct", Role::READ_ONLY, "test"));
    assert(!security.authenticate("alice", "wrong", "127.0.0.1"));
    assert(!security.authenticate("alice", "wrong", "127.0.0.1"));
    const auto locked = security.getUser("alice");
    assert(locked && locked->isLocked && locked->failedLoginAttempts == 2);
    assert(locked->lockedUntil > std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    assert(!security.authenticate("alice", "correct", "127.0.0.1"));
    security.loadUsers();
    assert(!security.authenticate("alice", "correct", "127.0.0.1"));

    security.setLockoutDurationMinutes(0);
    assert(security.createUser("bob", "correct", Role::READ_ONLY, "test"));
    assert(!security.authenticate("bob", "wrong", "127.0.0.1"));
    assert(!security.authenticate("bob", "wrong", "127.0.0.1"));
    assert(security.authenticate("bob", "correct", "127.0.0.1"));
    const auto expired = security.getUser("bob");
    assert(expired && !expired->isLocked && expired->failedLoginAttempts == 0 &&
           expired->lockedUntil == 0);

    security.setLockoutDurationMinutes(30);
    assert(security.createUser("carol", "old", Role::READ_ONLY, "test"));
    assert(!security.authenticate("carol", "wrong", "127.0.0.1"));
    assert(!security.authenticate("carol", "wrong", "127.0.0.1"));
    assert(security.updateUserPassword("carol", "new", "admin"));
    const auto reset = security.getUser("carol");
    assert(reset && !reset->isLocked && reset->failedLoginAttempts == 0 &&
           reset->lockedUntil == 0);
    assert(security.authenticate("carol", "new", "127.0.0.1"));

    assert(security.createUser("legacy", "correct", Role::READ_ONLY, "test"));
    const auto usersFile = root.dataRoot() / "security" / "users.json";
    nlohmann::json users;
    {
        std::ifstream input(usersFile);
        input >> users;
    }
    for (auto& user : users.at("users")) {
        if (user.value("username", "") != "legacy") continue;
        user["isLocked"] = true;
        user["failedLoginAttempts"] = 2;
        user.erase("lockedUntil");
    }
    {
        std::ofstream output(usersFile, std::ios::trunc);
        output << users.dump(2);
    }
    security.loadUsers();
    const auto legacy = security.getUser("legacy");
    assert(legacy && legacy->isLocked && legacy->lockedUntil == 0);
    assert(!security.authenticate("legacy", "correct", "127.0.0.1"));
    assert(security.updateUserPassword("legacy", "reset", "admin"));
    assert(security.authenticate("legacy", "reset", "127.0.0.1"));
}
