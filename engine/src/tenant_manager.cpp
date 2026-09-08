#include "tenant_manager.hpp"
#include <fstream>
#include "storage_path.hpp"
#include <sstream>
#include <iomanip>
#include <random>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace fs = std::filesystem;

namespace pacificdb {
namespace tenant {

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Simple SHA-256 implementation for password hashing
// (Simplified - in production use OpenSSL or similar)
static std::string sha256_simple(const std::string& input) {
    // Using a simple hash for demo - in production use proper crypto library
    unsigned int hash[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    for (size_t i = 0; i < input.size(); i++) {
        unsigned char c = input[i];
        for (int j = 0; j < 8; j++) {
            hash[j] = ((hash[j] << 5) | (hash[j] >> 27)) ^ (c + j);
        }
    }

    std::stringstream ss;
    for (int i = 0; i < 8; i++) {
        ss << std::hex << std::setw(8) << std::setfill('0') << hash[i];
    }
    return ss.str();
}

std::string TenantManager::generateSalt() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("CSPRNG unavailable while generating salt");
    }
    std::stringstream ss;
    for (unsigned char byte : bytes) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(byte);
    }
    return ss.str();
}

std::string TenantManager::hashPassword(const std::string& password, const std::string& salt) {
    constexpr int iterations = 310000;
    unsigned char derived[32];
    if (PKCS5_PBKDF2_HMAC(
            password.data(), static_cast<int>(password.size()),
            reinterpret_cast<const unsigned char*>(salt.data()),
            static_cast<int>(salt.size()), iterations, EVP_sha256(),
            sizeof(derived), derived) != 1) {
        throw std::runtime_error("PBKDF2 password derivation failed");
    }
    std::stringstream encoded;
    encoded << "pbkdf2-sha256$" << iterations << '$';
    for (unsigned char byte : derived) {
        encoded << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(byte);
    }
    return encoded.str();
}

bool TenantManager::verifyPassword(const std::string& password,
                                   const std::string& salt,
                                   const std::string& encodedHash) {
    if (encodedHash.rfind("pbkdf2-sha256$", 0) == 0) {
        const std::string computed = hashPassword(password, salt);
        return computed.size() == encodedHash.size() &&
               CRYPTO_memcmp(computed.data(), encodedHash.data(),
                             computed.size()) == 0;
    }
    const std::string legacy = sha256_simple(password + salt);
    return legacy.size() == encodedHash.size() &&
           CRYPTO_memcmp(legacy.data(), encodedHash.data(), legacy.size()) == 0;
}

std::string TenantManager::generateToken() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("CSPRNG unavailable while generating token");
    }
    std::stringstream ss;
    for (unsigned char byte : bytes) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(byte);
    }
    return ss.str();
}

std::string TenantManager::getTenantDataPath(const std::string& tenantId) {
    const std::string safeTenant =
        validateStorageIdentifier(tenantId, "tenantId");
    return validateContainedStoragePath(
        fs::path(dataRoot_),
        fs::path(dataRoot_) / "tenants" / safeTenant).string();
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void TenantManager::initialize(const std::string& dataRoot) {
    std::lock_guard<std::mutex> lock(tenantMutex_);
    const fs::path configured(dataRoot);
    if (dataRoot.empty() || !configured.is_absolute()) {
        throw std::invalid_argument(
            "TenantManager requires an explicit absolute DATA_ROOT");
    }
    std::error_code ec;
    fs::create_directories(configured, ec);
    if (ec) {
        throw std::runtime_error(
            "TenantManager cannot create DATA_ROOT: " + ec.message());
    }
    dataRoot_ = fs::canonical(configured, ec).string();
    if (ec || dataRoot_.empty()) {
        throw std::runtime_error(
            "TenantManager cannot canonicalize DATA_ROOT");
    }

    // Create tenants directory
    createContainedStorageDirectories(
        fs::path(dataRoot_), fs::path(dataRoot_) / "tenants");

    // Load existing tenants
    loadTenants();

    std::cout << "[TenantManager] Initialized with " << tenants_.size() << " tenants" << std::endl;
}

void TenantManager::loadTenants() {
    std::string tenantsFile = dataRoot_ + "/tenants/tenants.json";

    if (!fs::exists(tenantsFile)) {
        return;
    }

    try {
        std::ifstream file(tenantsFile);
        json tenantsJson;
        file >> tenantsJson;

        if (tenantsJson.contains("tenants") && tenantsJson["tenants"].is_array()) {
            for (auto& tj : tenantsJson["tenants"]) {
                Tenant t = Tenant::fromJson(tj);
                tenants_[t.tenantId] = t;

                // Load users for this tenant
                loadTenantUsers(t.tenantId);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[TenantManager] Error loading tenants: " << e.what() << std::endl;
    }
}

void TenantManager::saveTenants() {
    std::string tenantsFile = dataRoot_ + "/tenants/tenants.json";

    json tenantsJson;
    tenantsJson["tenants"] = json::array();

    for (auto& [id, tenant] : tenants_) {
        tenantsJson["tenants"].push_back(tenant.toJson());
    }

    std::ofstream file(tenantsFile);
    file << tenantsJson.dump(2);
}

void TenantManager::loadTenantUsers(const std::string& tenantId) {
    std::string usersFile = getTenantDataPath(tenantId) + "/users.json";

    if (!fs::exists(usersFile)) {
        return;
    }

    try {
        std::ifstream file(usersFile);
        json usersJson;
        file >> usersJson;

        tenantUsers_[tenantId].clear();

        if (usersJson.contains("users") && usersJson["users"].is_array()) {
            for (auto& uj : usersJson["users"]) {
                TenantUser u = TenantUser::fromJson(uj);
                tenantUsers_[tenantId][u.username] = u;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[TenantManager] Error loading users for tenant "
                  << tenantId << ": " << e.what() << std::endl;
    }
}

void TenantManager::saveTenantUsers(const std::string& tenantId) {
    std::string usersFile = getTenantDataPath(tenantId) + "/users.json";

    json usersJson;
    usersJson["users"] = json::array();

    if (tenantUsers_.find(tenantId) != tenantUsers_.end()) {
        for (auto& [username, user] : tenantUsers_[tenantId]) {
            usersJson["users"].push_back(user.toJson());
        }
    }

    std::ofstream file(usersFile);
    file << usersJson.dump(2);
}

// ============================================================================
// TENANT OPERATIONS
// ============================================================================

json TenantManager::createTenant(const std::string& tenantId,
                                  const std::string& name,
                                  const std::string& ownerUsername,
                                  const std::string& ownerPassword,
                                  const json& options) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    // Check if tenant already exists
    if (tenants_.find(tenantId) != tenants_.end()) {
        return {{"success", false}, {"error", "Tenant already exists"}};
    }

    try {
        validateStorageIdentifier(tenantId, "tenantId");
    } catch (const std::invalid_argument& error) {
        return {{"success", false}, {"error", error.what()}};
    }
    // Preserve the documented tenant character contract.
    for (char c : tenantId) {
        if (!std::isalnum(c) && c != '_' && c != '-') {
            return {{"success", false}, {"error", "Invalid tenant ID. Use alphanumeric, underscore, or hyphen only"}};
        }
    }

    // Create tenant
    Tenant tenant;
    tenant.tenantId = tenantId;
    tenant.name = name;
    tenant.ownerUsername = ownerUsername;
    tenant.createdAt = std::chrono::system_clock::now();
    tenant.isActive = true;

    // Set limits from options or defaults
    tenant.maxDatabases = options.value("maxDatabases", 100);
    tenant.maxCollectionsPerDb = options.value("maxCollectionsPerDb", 500);
    tenant.maxStorageBytes = options.value("maxStorageBytes", 10UL * 1024 * 1024 * 1024); // 10GB
    tenant.maxUsersPerTenant = options.value("maxUsersPerTenant", 100);
    tenant.currentDatabaseCount = 0;
    tenant.currentStorageBytes = 0;
    tenant.currentUserCount = 1; // The owner
    tenant.metadata = options.value("metadata", json::object());

    // Create tenant directory structure
    std::string tenantPath = getTenantDataPath(tenantId);
    createContainedStorageDirectories(fs::path(dataRoot_), fs::path(tenantPath) / "data");
    createContainedStorageDirectories(fs::path(dataRoot_), fs::path(tenantPath) / "wal");
    createContainedStorageDirectories(fs::path(dataRoot_), fs::path(tenantPath) / "logs");
    createContainedStorageDirectories(fs::path(dataRoot_), fs::path(tenantPath) / "backup");

    // Create owner user
    TenantUser owner;
    owner.username = ownerUsername;
    owner.tenantId = tenantId;
    owner.salt = generateSalt();
    owner.passwordHash = hashPassword(ownerPassword, owner.salt);
    owner.role = TenantRole::TENANT_OWNER;
    owner.isActive = true;
    owner.createdAt = std::chrono::system_clock::now();
    owner.lastLogin = std::chrono::system_clock::time_point{};
    owner.failedLoginAttempts = 0;
    owner.isLocked = false;
    owner.metadata = json::object();

    // Store
    tenants_[tenantId] = tenant;
    tenantUsers_[tenantId][ownerUsername] = owner;

    // Persist
    saveTenants();
    saveTenantUsers(tenantId);

    // Audit log
    logTenantAudit(tenantId, ownerUsername, "CREATE_TENANT", tenantId,
                   "Tenant created with owner " + ownerUsername, true);

    std::cout << "[TenantManager] Created tenant: " << tenantId
              << " with owner: " << ownerUsername << std::endl;

    return {
        {"success", true},
        {"tenantId", tenantId},
        {"name", name},
        {"ownerUsername", ownerUsername},
        {"message", "Tenant created successfully. Owner has full control."}
    };
}

json TenantManager::deleteTenant(const std::string& tenantId, const std::string& confirmedBy) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    auto it = tenants_.find(tenantId);
    if (it == tenants_.end()) {
        return {{"success", false}, {"error", "Tenant not found"}};
    }

    // Only owner can delete tenant
    if (it->second.ownerUsername != confirmedBy) {
        return {{"success", false}, {"error", "Only the tenant owner can delete the tenant"}};
    }

    // Revoke all sessions for this tenant
    {
        std::lock_guard<std::mutex> slock(sessionMutex_);
        std::vector<std::string> tokensToRemove;
        for (auto& [token, session] : activeSessions_) {
            if (session.tenantId == tenantId) {
                tokensToRemove.push_back(token);
            }
        }
        for (auto& token : tokensToRemove) {
            activeSessions_.erase(token);
        }
    }

    // Remove data directory
    std::string tenantPath = getTenantDataPath(tenantId);
    try {
        fs::remove_all(tenantPath);
    } catch (const std::exception& e) {
        std::cerr << "[TenantManager] Warning: Could not fully remove tenant data: "
                  << e.what() << std::endl;
    }

    // Remove from memory
    tenantUsers_.erase(tenantId);
    tenants_.erase(tenantId);

    // Persist
    saveTenants();

    std::cout << "[TenantManager] Deleted tenant: " << tenantId << std::endl;

    return {{"success", true}, {"message", "Tenant deleted successfully"}};
}

std::optional<Tenant> TenantManager::getTenant(const std::string& tenantId) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    auto it = tenants_.find(tenantId);
    if (it != tenants_.end()) {
        return it->second;
    }
    return std::nullopt;
}

json TenantManager::listTenants() {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    json result = json::array();
    for (auto& [id, tenant] : tenants_) {
        result.push_back({
            {"tenantId", tenant.tenantId},
            {"name", tenant.name},
            {"ownerUsername", tenant.ownerUsername},
            {"isActive", tenant.isActive},
            {"currentDatabaseCount", tenant.currentDatabaseCount},
            {"currentUserCount", tenant.currentUserCount}
        });
    }

    return {{"success", true}, {"tenants", result}, {"count", result.size()}};
}

json TenantManager::updateTenant(const std::string& tenantId, const json& updates) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    auto it = tenants_.find(tenantId);
    if (it == tenants_.end()) {
        return {{"success", false}, {"error", "Tenant not found"}};
    }

    Tenant& tenant = it->second;

    if (updates.contains("name")) tenant.name = updates["name"];
    if (updates.contains("isActive")) tenant.isActive = updates["isActive"];
    if (updates.contains("maxDatabases")) tenant.maxDatabases = updates["maxDatabases"];
    if (updates.contains("maxCollectionsPerDb")) tenant.maxCollectionsPerDb = updates["maxCollectionsPerDb"];
    if (updates.contains("maxStorageBytes")) tenant.maxStorageBytes = updates["maxStorageBytes"];
    if (updates.contains("maxUsersPerTenant")) tenant.maxUsersPerTenant = updates["maxUsersPerTenant"];
    if (updates.contains("metadata")) tenant.metadata = updates["metadata"];

    saveTenants();

    return {{"success", true}, {"message", "Tenant updated"}};
}

// ============================================================================
// TENANT USER OPERATIONS
// ============================================================================

json TenantManager::createTenantUser(const std::string& tenantId,
                                      const std::string& username,
                                      const std::string& password,
                                      TenantRole role,
                                      const std::string& createdBy) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    // Check tenant exists
    auto tenantIt = tenants_.find(tenantId);
    if (tenantIt == tenants_.end()) {
        return {{"success", false}, {"error", "Tenant not found"}};
    }

    // Check user limit
    if (tenantIt->second.currentUserCount >= tenantIt->second.maxUsersPerTenant) {
        return {{"success", false}, {"error", "User limit reached for this tenant"}};
    }

    // Check creator has permission
    if (tenantUsers_.find(tenantId) != tenantUsers_.end()) {
        auto creatorIt = tenantUsers_[tenantId].find(createdBy);
        if (creatorIt == tenantUsers_[tenantId].end()) {
            return {{"success", false}, {"error", "Creator not found in tenant"}};
        }

        TenantRole creatorRole = creatorIt->second.role;
        if (creatorRole != TenantRole::TENANT_OWNER &&
            creatorRole != TenantRole::TENANT_ADMIN &&
            creatorRole != TenantRole::TENANT_USERADMIN) {
            return {{"success", false}, {"error", "No permission to create users"}};
        }

        // Non-owners cannot create owners
        if (role == TenantRole::TENANT_OWNER && creatorRole != TenantRole::TENANT_OWNER) {
            return {{"success", false}, {"error", "Only owner can create another owner"}};
        }
    }

    // Check user doesn't exist
    if (tenantUsers_[tenantId].find(username) != tenantUsers_[tenantId].end()) {
        return {{"success", false}, {"error", "User already exists in this tenant"}};
    }

    // Create user
    TenantUser user;
    user.username = username;
    user.tenantId = tenantId;
    user.salt = generateSalt();
    user.passwordHash = hashPassword(password, user.salt);
    user.role = role;
    user.isActive = true;
    user.createdAt = std::chrono::system_clock::now();
    user.lastLogin = std::chrono::system_clock::time_point{};
    user.failedLoginAttempts = 0;
    user.isLocked = false;

    tenantUsers_[tenantId][username] = user;
    tenantIt->second.currentUserCount++;

    saveTenantUsers(tenantId);
    saveTenants();

    logTenantAudit(tenantId, createdBy, "CREATE_USER", username,
                   "Created user with role " + tenantRoleToString(role), true);

    std::cout << "[TenantManager] Created user " << username
              << " in tenant " << tenantId << std::endl;

    return {
        {"success", true},
        {"username", username},
        {"tenantId", tenantId},
        {"role", tenantRoleToString(role)},
        {"message", "User created successfully"}
    };
}

json TenantManager::deleteTenantUser(const std::string& tenantId,
                                      const std::string& username,
                                      const std::string& deletedBy) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    auto tenantIt = tenants_.find(tenantId);
    if (tenantIt == tenants_.end()) {
        return {{"success", false}, {"error", "Tenant not found"}};
    }

    // Cannot delete the owner
    if (tenantIt->second.ownerUsername == username) {
        return {{"success", false}, {"error", "Cannot delete tenant owner"}};
    }

    // Check deleter has permission
    if (tenantUsers_.find(tenantId) != tenantUsers_.end()) {
        auto deleterIt = tenantUsers_[tenantId].find(deletedBy);
        if (deleterIt == tenantUsers_[tenantId].end()) {
            return {{"success", false}, {"error", "Deleter not found in tenant"}};
        }

        TenantRole deleterRole = deleterIt->second.role;
        if (deleterRole != TenantRole::TENANT_OWNER &&
            deleterRole != TenantRole::TENANT_ADMIN &&
            deleterRole != TenantRole::TENANT_USERADMIN) {
            return {{"success", false}, {"error", "No permission to delete users"}};
        }
    }

    // Check user exists
    if (tenantUsers_[tenantId].find(username) == tenantUsers_[tenantId].end()) {
        return {{"success", false}, {"error", "User not found"}};
    }

    // Revoke sessions
    revokeAllUserSessions(tenantId, username);

    tenantUsers_[tenantId].erase(username);
    tenantIt->second.currentUserCount--;

    saveTenantUsers(tenantId);
    saveTenants();

    logTenantAudit(tenantId, deletedBy, "DELETE_USER", username, "User deleted", true);

    return {{"success", true}, {"message", "User deleted successfully"}};
}

json TenantManager::updateTenantUserRole(const std::string& tenantId,
                                          const std::string& username,
                                          TenantRole newRole,
                                          const std::string& updatedBy) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    auto tenantIt = tenants_.find(tenantId);
    if (tenantIt == tenants_.end()) {
        return {{"success", false}, {"error", "Tenant not found"}};
    }

    // Check updater has permission
    if (tenantUsers_.find(tenantId) != tenantUsers_.end()) {
        auto updaterIt = tenantUsers_[tenantId].find(updatedBy);
        if (updaterIt == tenantUsers_[tenantId].end()) {
            return {{"success", false}, {"error", "Updater not found"}};
        }

        TenantRole updaterRole = updaterIt->second.role;
        if (updaterRole != TenantRole::TENANT_OWNER &&
            updaterRole != TenantRole::TENANT_ADMIN &&
            updaterRole != TenantRole::TENANT_USERADMIN) {
            return {{"success", false}, {"error", "No permission to update user roles"}};
        }

        // Only owner can set owner role
        if (newRole == TenantRole::TENANT_OWNER && updaterRole != TenantRole::TENANT_OWNER) {
            return {{"success", false}, {"error", "Only owner can set owner role"}};
        }
    }

    auto userIt = tenantUsers_[tenantId].find(username);
    if (userIt == tenantUsers_[tenantId].end()) {
        return {{"success", false}, {"error", "User not found"}};
    }

    userIt->second.role = newRole;
    saveTenantUsers(tenantId);

    logTenantAudit(tenantId, updatedBy, "UPDATE_USER_ROLE", username,
                   "Role changed to " + tenantRoleToString(newRole), true);

    return {{"success", true}, {"message", "User role updated"}};
}

json TenantManager::updateTenantUserPassword(const std::string& tenantId,
                                               const std::string& username,
                                               const std::string& newPassword,
                                               const std::string& updatedBy) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    // Can update own password or admin can update others
    bool isSelf = (username == updatedBy);

    if (!isSelf) {
        auto updaterIt = tenantUsers_[tenantId].find(updatedBy);
        if (updaterIt == tenantUsers_[tenantId].end()) {
            return {{"success", false}, {"error", "Updater not found"}};
        }

        TenantRole updaterRole = updaterIt->second.role;
        if (updaterRole != TenantRole::TENANT_OWNER &&
            updaterRole != TenantRole::TENANT_ADMIN &&
            updaterRole != TenantRole::TENANT_USERADMIN) {
            return {{"success", false}, {"error", "No permission to update other users' passwords"}};
        }
    }

    auto userIt = tenantUsers_[tenantId].find(username);
    if (userIt == tenantUsers_[tenantId].end()) {
        return {{"success", false}, {"error", "User not found"}};
    }

    userIt->second.salt = generateSalt();
    userIt->second.passwordHash = hashPassword(newPassword, userIt->second.salt);

    saveTenantUsers(tenantId);

    // Revoke all sessions for security
    revokeAllUserSessions(tenantId, username);

    logTenantAudit(tenantId, updatedBy, "UPDATE_PASSWORD", username, "Password changed", true);

    return {{"success", true}, {"message", "Password updated"}};
}

json TenantManager::setTenantUserDatabaseAccess(const std::string& tenantId,
                                                  const std::string& username,
                                                  const std::vector<std::string>& databases,
                                                  const std::string& updatedBy) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    // Check permission
    auto updaterIt = tenantUsers_[tenantId].find(updatedBy);
    if (updaterIt == tenantUsers_[tenantId].end()) {
        return {{"success", false}, {"error", "Updater not found"}};
    }

    TenantRole updaterRole = updaterIt->second.role;
    if (updaterRole != TenantRole::TENANT_OWNER &&
        updaterRole != TenantRole::TENANT_ADMIN) {
        return {{"success", false}, {"error", "No permission"}};
    }

    auto userIt = tenantUsers_[tenantId].find(username);
    if (userIt == tenantUsers_[tenantId].end()) {
        return {{"success", false}, {"error", "User not found"}};
    }

    userIt->second.allowedDatabases = databases;
    saveTenantUsers(tenantId);

    logTenantAudit(tenantId, updatedBy, "UPDATE_DB_ACCESS", username,
                   "Database access updated", true);

    return {{"success", true}, {"message", "Database access updated"}};
}

std::optional<TenantUser> TenantManager::getTenantUser(const std::string& tenantId,
                                                         const std::string& username) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    if (tenantUsers_.find(tenantId) == tenantUsers_.end()) {
        return std::nullopt;
    }

    auto it = tenantUsers_[tenantId].find(username);
    if (it != tenantUsers_[tenantId].end()) {
        return it->second;
    }
    return std::nullopt;
}

json TenantManager::listTenantUsers(const std::string& tenantId) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    if (tenants_.find(tenantId) == tenants_.end()) {
        return {{"success", false}, {"error", "Tenant not found"}};
    }

    json users = json::array();

    if (tenantUsers_.find(tenantId) != tenantUsers_.end()) {
        for (auto& [username, user] : tenantUsers_[tenantId]) {
            users.push_back({
                {"username", user.username},
                {"role", tenantRoleToString(user.role)},
                {"isActive", user.isActive},
                {"isLocked", user.isLocked},
                {"allowedDatabases", user.allowedDatabases}
            });
        }
    }

    return {{"success", true}, {"users", users}, {"count", users.size()}};
}

// ============================================================================
// AUTHENTICATION
// ============================================================================

json TenantManager::authenticateTenantUser(const std::string& tenantId,
                                            const std::string& username,
                                            const std::string& password,
                                            const std::string& clientIP) {
    std::lock_guard<std::mutex> lock(tenantMutex_);

    // Check tenant
    auto tenantIt = tenants_.find(tenantId);
    if (tenantIt == tenants_.end() || !tenantIt->second.isActive) {
        return {{"success", false}, {"error", "Tenant not found or inactive"}};
    }

    // Check user
    if (tenantUsers_.find(tenantId) == tenantUsers_.end()) {
        return {{"success", false}, {"error", "Invalid credentials"}};
    }

    auto userIt = tenantUsers_[tenantId].find(username);
    if (userIt == tenantUsers_[tenantId].end()) {
        return {{"success", false}, {"error", "Invalid credentials"}};
    }

    TenantUser& user = userIt->second;

    // Check if locked
    if (user.isLocked) {
        return {{"success", false}, {"error", "Account is locked"}};
    }

    if (!user.isActive) {
        return {{"success", false}, {"error", "Account is inactive"}};
    }

    // Verify password
    if (!verifyPassword(password, user.salt, user.passwordHash)) {
        user.failedLoginAttempts++;
        if (user.failedLoginAttempts >= maxLoginAttempts_) {
            user.isLocked = true;
            saveTenantUsers(tenantId);
            logTenantAudit(tenantId, username, "LOGIN_FAILED", "auth",
                           "Account locked after " + std::to_string(maxLoginAttempts_) + " failed attempts", false);
        }
        return {{"success", false}, {"error", "Invalid credentials"}};
    }

    // Success - reset failed attempts and update last login
    if (user.passwordHash.rfind("pbkdf2-sha256$", 0) != 0) {
        user.passwordHash = hashPassword(password, user.salt);
    }
    user.failedLoginAttempts = 0;
    user.lastLogin = std::chrono::system_clock::now();
    saveTenantUsers(tenantId);

    // Create session
    TenantSession session;
    session.token = generateToken();
    session.tenantId = tenantId;
    session.username = username;
    session.role = user.role;
    session.issuedAt = std::chrono::system_clock::now();
    session.expiresAt = session.issuedAt + std::chrono::minutes(sessionExpiryMinutes_);
    session.clientIP = clientIP;

    {
        std::lock_guard<std::mutex> slock(sessionMutex_);
        activeSessions_[session.token] = session;
    }

    logTenantAudit(tenantId, username, "LOGIN", "auth", "Login from " + clientIP, true);

    std::cout << "[TenantManager] User " << username << " authenticated for tenant "
              << tenantId << std::endl;

    return {
        {"success", true},
        {"token", session.token},
        {"tenantId", tenantId},
        {"username", username},
        {"role", tenantRoleToString(user.role)},
        {"expiresAt", std::chrono::duration_cast<std::chrono::seconds>(
            session.expiresAt.time_since_epoch()).count()}
    };
}

bool TenantManager::validateSession(const std::string& token) {
    std::lock_guard<std::mutex> lock(sessionMutex_);

    auto it = activeSessions_.find(token);
    if (it == activeSessions_.end()) {
        return false;
    }

    if (it->second.isExpired()) {
        activeSessions_.erase(it);
        return false;
    }

    return true;
}

std::optional<TenantSession> TenantManager::getSession(const std::string& token) {
    std::lock_guard<std::mutex> lock(sessionMutex_);

    auto it = activeSessions_.find(token);
    if (it != activeSessions_.end() && !it->second.isExpired()) {
        return it->second;
    }
    return std::nullopt;
}

void TenantManager::revokeSession(const std::string& token) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    activeSessions_.erase(token);
}

void TenantManager::revokeAllUserSessions(const std::string& tenantId, const std::string& username) {
    std::lock_guard<std::mutex> lock(sessionMutex_);

    std::vector<std::string> tokensToRemove;
    for (auto& [token, session] : activeSessions_) {
        if (session.tenantId == tenantId && session.username == username) {
            tokensToRemove.push_back(token);
        }
    }

    for (auto& token : tokensToRemove) {
        activeSessions_.erase(token);
    }
}

// ============================================================================
// AUTHORIZATION
// ============================================================================

bool TenantManager::hasPermission(const std::string& token, TenantPermission perm) {
    auto sessionOpt = getSession(token);
    if (!sessionOpt) {
        return false;
    }

    TenantSession& session = *sessionOpt;
    auto permissions = getPermissionsForTenantRole(session.role);

    // ALL permission grants everything
    if (permissions.count(TenantPermission::ALL) > 0) {
        return true;
    }

    return permissions.count(perm) > 0;
}

bool TenantManager::canAccessDatabase(const std::string& token, const std::string& database) {
    auto sessionOpt = getSession(token);
    if (!sessionOpt) {
        return false;
    }

    std::lock_guard<std::mutex> lock(tenantMutex_);

    TenantSession& session = *sessionOpt;

    // Owner and admin can access all databases in tenant
    if (session.role == TenantRole::TENANT_OWNER ||
        session.role == TenantRole::TENANT_ADMIN) {
        return true;
    }

    // Check user's allowed databases
    auto userOpt = getTenantUser(session.tenantId, session.username);
    if (!userOpt) {
        return false;
    }

    // Empty = all databases
    if (userOpt->allowedDatabases.empty()) {
        return true;
    }

    return std::find(userOpt->allowedDatabases.begin(),
                     userOpt->allowedDatabases.end(),
                     database) != userOpt->allowedDatabases.end();
}

json TenantManager::checkAccess(const std::string& token,
                                 const std::string& action,
                                 const std::string& database,
                                 const std::string& collection) {
    auto sessionOpt = getSession(token);
    if (!sessionOpt) {
        return {{"allowed", false}, {"reason", "Invalid or expired session"}};
    }

    TenantSession& session = *sessionOpt;

    // Map action to permission
    TenantPermission perm = TenantPermission::READ;
    if (action == "insert" || action == "insertVector") {
        perm = TenantPermission::INSERT;
    } else if (action == "update") {
        perm = TenantPermission::UPDATE;
    } else if (action == "delete") {
        perm = TenantPermission::DELETE;
    } else if (action == "find" || action == "get" || action == "query") {
        perm = TenantPermission::READ;
    } else if (action == "createDatabase") {
        perm = TenantPermission::CREATE_DATABASE;
    } else if (action == "dropDatabase") {
        perm = TenantPermission::DROP_DATABASE;
    } else if (action == "createCollection") {
        perm = TenantPermission::CREATE_COLLECTION;
    } else if (action == "dropCollection") {
        perm = TenantPermission::DROP_COLLECTION;
    } else if (action == "createIndex") {
        perm = TenantPermission::CREATE_INDEX;
    } else if (action == "createUser") {
        perm = TenantPermission::CREATE_USER;
    } else if (action == "deleteUser") {
        perm = TenantPermission::DELETE_USER;
    } else if (action == "backup") {
        perm = TenantPermission::BACKUP;
    }

    // Check permission
    if (!hasPermission(token, perm)) {
        return {
            {"allowed", false},
            {"reason", "Permission denied for action: " + action}
        };
    }

    // Check database access if applicable
    if (!database.empty() && !canAccessDatabase(token, database)) {
        return {
            {"allowed", false},
            {"reason", "Access denied to database: " + database}
        };
    }

    return {
        {"allowed", true},
        {"tenantId", session.tenantId},
        {"username", session.username},
        {"role", tenantRoleToString(session.role)}
    };
}

// ============================================================================
// ISOLATED OPERATIONS
// ============================================================================

json TenantManager::createDatabaseInTenant(const std::string& token,
                                            const std::string& dbName,
                                            const std::string& dbType) {
    try {
        validateStorageIdentifier(dbName, "databaseName");
    } catch (const std::invalid_argument& error) {
        return {{"success", false}, {"error", error.what()}};
    }
    auto access = checkAccess(token, "createDatabase", "", "");
    if (!access.value("allowed", false)) {
        return access;
    }

    auto sessionOpt = getSession(token);
    std::string tenantId = sessionOpt->tenantId;

    std::lock_guard<std::mutex> lock(tenantMutex_);

    // Check limits
    auto& tenant = tenants_[tenantId];
    if (tenant.currentDatabaseCount >= tenant.maxDatabases) {
        return {{"success", false}, {"error", "Database limit reached"}};
    }

    // Create database directory within tenant isolation
    fs::path dbPath = validateContainedStoragePath(
        fs::path(dataRoot_),
        fs::path(getTenantDataPath(tenantId)) / "data" / dbName);

    try {
        createContainedStorageDirectories(fs::path(dataRoot_), dbPath);
        tenant.currentDatabaseCount++;
        saveTenants();

        logTenantAudit(tenantId, sessionOpt->username, "CREATE_DATABASE",
                       dbName, "Database created: " + dbType, true);

        return {
            {"success", true},
            {"database", dbName},
            {"tenantId", tenantId},
            {"type", dbType},
            {"path", dbPath.string()}
        };
    } catch (const std::exception& e) {
        return {{"success", false}, {"error", e.what()}};
    }
}

json TenantManager::dropDatabaseInTenant(const std::string& token, const std::string& dbName) {
    try {
        validateStorageIdentifier(dbName, "databaseName");
    } catch (const std::invalid_argument& error) {
        return {{"success", false}, {"error", error.what()}};
    }
    auto access = checkAccess(token, "dropDatabase", dbName, "");
    if (!access.value("allowed", false)) {
        return access;
    }

    auto sessionOpt = getSession(token);
    std::string tenantId = sessionOpt->tenantId;

    std::lock_guard<std::mutex> lock(tenantMutex_);

    fs::path dbPath = validateContainedStoragePath(
        fs::path(dataRoot_),
        fs::path(getTenantDataPath(tenantId)) / "data" / dbName);

    try {
        if (fs::exists(dbPath)) {
            fs::remove_all(dbPath);
            tenants_[tenantId].currentDatabaseCount--;
            saveTenants();

            logTenantAudit(tenantId, sessionOpt->username, "DROP_DATABASE",
                           dbName, "Database dropped", true);

            return {{"success", true}, {"message", "Database dropped"}};
        } else {
            return {{"success", false}, {"error", "Database not found"}};
        }
    } catch (const std::exception& e) {
        return {{"success", false}, {"error", e.what()}};
    }
}

json TenantManager::listDatabasesInTenant(const std::string& token) {
    auto access = checkAccess(token, "listDatabases", "", "");
    if (!access.value("allowed", false)) {
        return access;
    }

    auto sessionOpt = getSession(token);
    std::string tenantId = sessionOpt->tenantId;

    std::string dataPath = getTenantDataPath(tenantId) + "/data";

    json databases = json::array();

    if (fs::exists(dataPath)) {
        for (const auto& entry : fs::directory_iterator(dataPath)) {
            if (entry.is_directory()) {
                std::string dbName = entry.path().filename().string();

                // Check if user can access this database
                if (canAccessDatabase(token, dbName)) {
                    size_t size = 0;
                    for (const auto& file : fs::recursive_directory_iterator(entry.path())) {
                        if (file.is_regular_file()) {
                            size += file.file_size();
                        }
                    }

                    databases.push_back({
                        {"name", dbName},
                        {"sizeBytes", size}
                    });
                }
            }
        }
    }

    return {{"success", true}, {"databases", databases}, {"count", databases.size()}};
}

json TenantManager::createCollectionInTenant(const std::string& token,
                                               const std::string& dbName,
                                               const std::string& collName) {
    try {
        validateStorageIdentifier(dbName, "databaseName");
        validateStorageIdentifier(collName, "collectionName");
    } catch (const std::invalid_argument& error) {
        return {{"success", false}, {"error", error.what()}};
    }
    auto access = checkAccess(token, "createCollection", dbName, "");
    if (!access.value("allowed", false)) {
        return access;
    }

    auto sessionOpt = getSession(token);
    std::string tenantId = sessionOpt->tenantId;

    fs::path collPath = validateContainedStoragePath(
        fs::path(dataRoot_),
        fs::path(getTenantDataPath(tenantId)) / "data" / dbName / collName);

    try {
        createContainedStorageDirectories(fs::path(dataRoot_), collPath);

        logTenantAudit(tenantId, sessionOpt->username, "CREATE_COLLECTION",
                       dbName + "/" + collName, "Collection created", true);

        return {
            {"success", true},
            {"database", dbName},
            {"collection", collName},
            {"tenantId", tenantId}
        };
    } catch (const std::exception& e) {
        return {{"success", false}, {"error", e.what()}};
    }
}

json TenantManager::dropCollectionInTenant(const std::string& token,
                                             const std::string& dbName,
                                             const std::string& collName) {
    try {
        validateStorageIdentifier(dbName, "databaseName");
        validateStorageIdentifier(collName, "collectionName");
    } catch (const std::invalid_argument& error) {
        return {{"success", false}, {"error", error.what()}};
    }
    auto access = checkAccess(token, "dropCollection", dbName, collName);
    if (!access.value("allowed", false)) {
        return access;
    }

    auto sessionOpt = getSession(token);
    std::string tenantId = sessionOpt->tenantId;

    fs::path collPath = validateContainedStoragePath(
        fs::path(dataRoot_),
        fs::path(getTenantDataPath(tenantId)) / "data" / dbName / collName);

    try {
        if (fs::exists(collPath)) {
            fs::remove_all(collPath);

            logTenantAudit(tenantId, sessionOpt->username, "DROP_COLLECTION",
                           dbName + "/" + collName, "Collection dropped", true);

            return {{"success", true}, {"message", "Collection dropped"}};
        } else {
            return {{"success", false}, {"error", "Collection not found"}};
        }
    } catch (const std::exception& e) {
        return {{"success", false}, {"error", e.what()}};
    }
}

json TenantManager::getTenantStats(const std::string& token) {
    auto sessionOpt = getSession(token);
    if (!sessionOpt) {
        return {{"success", false}, {"error", "Invalid session"}};
    }

    std::string tenantId = sessionOpt->tenantId;

    std::lock_guard<std::mutex> lock(tenantMutex_);

    auto tenantIt = tenants_.find(tenantId);
    if (tenantIt == tenants_.end()) {
        return {{"success", false}, {"error", "Tenant not found"}};
    }

    Tenant& tenant = tenantIt->second;

    // Calculate actual storage
    std::string dataPath = getTenantDataPath(tenantId);
    size_t totalStorage = 0;

    if (fs::exists(dataPath)) {
        for (const auto& file : fs::recursive_directory_iterator(dataPath)) {
            if (file.is_regular_file()) {
                totalStorage += file.file_size();
            }
        }
    }

    tenant.currentStorageBytes = totalStorage;

    return {
        {"success", true},
        {"tenantId", tenantId},
        {"name", tenant.name},
        {"databases", {
            {"current", tenant.currentDatabaseCount},
            {"max", tenant.maxDatabases}
        }},
        {"users", {
            {"current", tenant.currentUserCount},
            {"max", tenant.maxUsersPerTenant}
        }},
        {"storage", {
            {"currentBytes", totalStorage},
            {"maxBytes", tenant.maxStorageBytes},
            {"usedPercent", (tenant.maxStorageBytes > 0) ?
                (100.0 * totalStorage / tenant.maxStorageBytes) : 0}
        }},
        {"isActive", tenant.isActive}
    };
}

// ============================================================================
// AUDIT LOGGING
// ============================================================================

void TenantManager::logTenantAudit(const std::string& tenantId,
                                    const std::string& username,
                                    const std::string& action,
                                    const std::string& resource,
                                    const std::string& details,
                                    bool success) {
    std::string auditFile = getTenantDataPath(tenantId) + "/logs/audit.log";

    // Ensure directory exists
    fs::create_directories(getTenantDataPath(tenantId) + "/logs");

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S");

    json logEntry = {
        {"timestamp", ss.str()},
        {"tenantId", tenantId},
        {"username", username},
        {"action", action},
        {"resource", resource},
        {"details", details},
        {"success", success}
    };

    std::ofstream file(auditFile, std::ios::app);
    file << logEntry.dump() << "\n";
}

json TenantManager::getTenantAuditLog(const std::string& tenantId, int limit) {
    std::string auditFile = getTenantDataPath(tenantId) + "/logs/audit.log";

    json logs = json::array();

    if (!fs::exists(auditFile)) {
        return {{"success", true}, {"logs", logs}, {"count", 0}};
    }

    std::ifstream file(auditFile);
    std::vector<json> allLogs;
    std::string line;

    while (std::getline(file, line)) {
        try {
            allLogs.push_back(json::parse(line));
        } catch (...) {}
    }

    // Return most recent entries
    int start = std::max(0, (int)allLogs.size() - limit);
    for (int i = allLogs.size() - 1; i >= start; i--) {
        logs.push_back(allLogs[i]);
    }

    return {{"success", true}, {"logs", logs}, {"count", logs.size()}};
}

// These placeholder implementations connect to the actual database engine
// In a full implementation, these would call the DatabaseEngine methods

json TenantManager::insertInTenant(const std::string& token,
                                    const std::string& dbName,
                                    const std::string& collName,
                                    const json& document) {
    auto access = checkAccess(token, "insert", dbName, collName);
    if (!access.value("allowed", false)) {
        return access;
    }

    // In full implementation, this would call:
    // DatabaseEngine::instance().insert(session.tenantId, dbName, collName, document);

    return {
        {"success", true},
        {"message", "Insert operation validated - forward to DatabaseEngine"},
        {"tenantId", access["tenantId"]},
        {"database", dbName},
        {"collection", collName}
    };
}

json TenantManager::findInTenant(const std::string& token,
                                  const std::string& dbName,
                                  const std::string& collName,
                                  const json& filter) {
    auto access = checkAccess(token, "find", dbName, collName);
    if (!access.value("allowed", false)) {
        return access;
    }

    return {
        {"success", true},
        {"message", "Find operation validated - forward to DatabaseEngine"},
        {"tenantId", access["tenantId"]},
        {"database", dbName},
        {"collection", collName}
    };
}

json TenantManager::updateInTenant(const std::string& token,
                                    const std::string& dbName,
                                    const std::string& collName,
                                    const json& filter,
                                    const json& update) {
    auto access = checkAccess(token, "update", dbName, collName);
    if (!access.value("allowed", false)) {
        return access;
    }

    return {
        {"success", true},
        {"message", "Update operation validated - forward to DatabaseEngine"},
        {"tenantId", access["tenantId"]},
        {"database", dbName},
        {"collection", collName}
    };
}

json TenantManager::deleteInTenant(const std::string& token,
                                    const std::string& dbName,
                                    const std::string& collName,
                                    const json& filter) {
    auto access = checkAccess(token, "delete", dbName, collName);
    if (!access.value("allowed", false)) {
        return access;
    }

    return {
        {"success", true},
        {"message", "Delete operation validated - forward to DatabaseEngine"},
        {"tenantId", access["tenantId"]},
        {"database", dbName},
        {"collection", collName}
    };
}

} // namespace tenant
} // namespace pacificdb
