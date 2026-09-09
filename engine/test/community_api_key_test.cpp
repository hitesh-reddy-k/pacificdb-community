#include "owned_test_root.hpp"
#include "security_manager.hpp"

#include <cassert>
#include <fstream>
#include <iostream>

int main() {
    const OwnedTestRoot root("community-api-keys");
    auto& security = pacificdb::security::SecurityManager::instance();
    security.initialize(root.dataRoot().string());

    const auto created = security.createApiKey("ci", "readwrite", "admin");
    const std::string key = created.at("key");
    assert(key.rfind("pdb_", 0) == 0);
    assert(created.dump().find("secret_hash") == std::string::npos);

    const auto validated = security.validateApiKey(key);
    assert(validated && validated->role == pacificdb::security::Role::WRITE);
    assert(security.validateToken(key));
    assert(security.hasPermission(key, pacificdb::security::Permission::READ));
    assert(!security.hasPermission(key, pacificdb::security::Permission::ADMIN));
    assert(security.getTokenUsername(key) == "admin");
    assert(security.listApiKeys().dump().find(key) == std::string::npos);
    assert(security.revokeApiKey(created.at("id"), "admin"));
    assert(!security.validateApiKey(key));

    std::ifstream persisted(root.dataRoot() / "security" / "api_keys.json");
    const std::string contents((std::istreambuf_iterator<char>(persisted)), {});
    assert(contents.find(key) == std::string::npos);

    std::cout << "COMMUNITY_API_KEY_PASS\n";
    return 0;
}
