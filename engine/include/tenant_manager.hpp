#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pacificdb {
namespace tenant {

// ============================================================================
// TENANT-SCOPED ROLES (per-tenant, not global)
// ============================================================================

enum class TenantRole {
    TENANT_OWNER,       // Full control over the tenant - like MongoDB's dbOwner
    TENANT_ADMIN,       // Admin within tenant (can manage users, databases, collections)
    TENANT_READWRITE,   // Read and write access to all databases in tenant
    TENANT_READONLY,    // Read-only access to all databases in tenant
    TENANT_DBADMIN,     // Can create/drop databases and collections, but not manage users
    TENANT_USERADMIN,   // Can only manage users within the tenant
    TENANT_BACKUP,      // Backup/restore within tenant only
    TENANT_CUSTOM       // Custom permissions defined per user
};

inline std::string tenantRoleToString(TenantRole role) {
    switch (role) {
        case TenantRole::TENANT_OWNER: return "tenant_owner";
        case TenantRole::TENANT_ADMIN: return "tenant_admin";
        case TenantRole::TENANT_READWRITE: return "tenant_readwrite";
        case TenantRole::TENANT_READONLY: return "tenant_readonly";
        case TenantRole::TENANT_DBADMIN: return "tenant_dbadmin";
        case TenantRole::TENANT_USERADMIN: return "tenant_useradmin";
        case TenantRole::TENANT_BACKUP: return "tenant_backup";
        case TenantRole::TENANT_CUSTOM: return "tenant_custom";
        default: return "tenant_readonly";
    }
}

inline TenantRole stringToTenantRole(const std::string& s) {
    if (s == "tenant_owner") return TenantRole::TENANT_OWNER;
    if (s == "tenant_admin") return TenantRole::TENANT_ADMIN;
    if (s == "tenant_readwrite") return TenantRole::TENANT_READWRITE;
    if (s == "tenant_readonly") return TenantRole::TENANT_READONLY;
    if (s == "tenant_dbadmin") return TenantRole::TENANT_DBADMIN;
    if (s == "tenant_useradmin") return TenantRole::TENANT_USERADMIN;
    if (s == "tenant_backup") return TenantRole::TENANT_BACKUP;
    if (s == "tenant_custom") return TenantRole::TENANT_CUSTOM;
    return TenantRole::TENANT_READONLY;
}

// ============================================================================
// TENANT-SCOPED PERMISSIONS
// ============================================================================

enum class TenantPermission {
    // User management within tenant
    CREATE_USER,
    DELETE_USER,
    UPDATE_USER,
    LIST_USERS,

    // Database management within tenant
    CREATE_DATABASE,
    DROP_DATABASE,
    LIST_DATABASES,

    // Collection management within tenant
    CREATE_COLLECTION,
    DROP_COLLECTION,
    LIST_COLLECTIONS,

    // Data operations within tenant
    INSERT,
    UPDATE,
    DELETE,
    READ,

    // Advanced operations within tenant
    CREATE_INDEX,
    DROP_INDEX,
    AGGREGATE,

    // Maintenance within tenant
    BACKUP,
    RESTORE,
    VIEW_STATS,
    COMPACT,

    // Full control
    ALL
};

inline std::unordered_set<TenantPermission> getPermissionsForTenantRole(TenantRole role) {
    std::unordered_set<TenantPermission> perms;

    switch (role) {
        case TenantRole::TENANT_OWNER:
            perms.insert(TenantPermission::ALL);
            // Fall through for explicit permissions
        case TenantRole::TENANT_ADMIN:
            perms.insert(TenantPermission::CREATE_USER);
            perms.insert(TenantPermission::DELETE_USER);
            perms.insert(TenantPermission::UPDATE_USER);
            perms.insert(TenantPermission::LIST_USERS);
            perms.insert(TenantPermission::CREATE_DATABASE);
            perms.insert(TenantPermission::DROP_DATABASE);
            perms.insert(TenantPermission::LIST_DATABASES);
            perms.insert(TenantPermission::CREATE_COLLECTION);
            perms.insert(TenantPermission::DROP_COLLECTION);
            perms.insert(TenantPermission::LIST_COLLECTIONS);
            perms.insert(TenantPermission::INSERT);
            perms.insert(TenantPermission::UPDATE);
            perms.insert(TenantPermission::DELETE);
            perms.insert(TenantPermission::READ);
            perms.insert(TenantPermission::CREATE_INDEX);
            perms.insert(TenantPermission::DROP_INDEX);
            perms.insert(TenantPermission::AGGREGATE);
            perms.insert(TenantPermission::BACKUP);
            perms.insert(TenantPermission::RESTORE);
            perms.insert(TenantPermission::VIEW_STATS);
            perms.insert(TenantPermission::COMPACT);
            break;

        case TenantRole::TENANT_DBADMIN:
            perms.insert(TenantPermission::CREATE_DATABASE);
            perms.insert(TenantPermission::DROP_DATABASE);
            perms.insert(TenantPermission::LIST_DATABASES);
            perms.insert(TenantPermission::CREATE_COLLECTION);
            perms.insert(TenantPermission::DROP_COLLECTION);
            perms.insert(TenantPermission::LIST_COLLECTIONS);
            perms.insert(TenantPermission::CREATE_INDEX);
            perms.insert(TenantPermission::DROP_INDEX);
            perms.insert(TenantPermission::VIEW_STATS);
            perms.insert(TenantPermission::COMPACT);
            perms.insert(TenantPermission::READ);
            break;

        case TenantRole::TENANT_USERADMIN:
            perms.insert(TenantPermission::CREATE_USER);
            perms.insert(TenantPermission::DELETE_USER);
            perms.insert(TenantPermission::UPDATE_USER);
            perms.insert(TenantPermission::LIST_USERS);
            perms.insert(TenantPermission::READ);
            break;

        case TenantRole::TENANT_READWRITE:
            perms.insert(TenantPermission::INSERT);
            perms.insert(TenantPermission::UPDATE);
            perms.insert(TenantPermission::DELETE);
            perms.insert(TenantPermission::READ);
            perms.insert(TenantPermission::LIST_DATABASES);
            perms.insert(TenantPermission::LIST_COLLECTIONS);
            perms.insert(TenantPermission::AGGREGATE);
            break;

        case TenantRole::TENANT_READONLY:
            perms.insert(TenantPermission::READ);
            perms.insert(TenantPermission::LIST_DATABASES);
            perms.insert(TenantPermission::LIST_COLLECTIONS);
            perms.insert(TenantPermission::VIEW_STATS);
            break;

        case TenantRole::TENANT_BACKUP:
            perms.insert(TenantPermission::BACKUP);
            perms.insert(TenantPermission::RESTORE);
            perms.insert(TenantPermission::READ);
            perms.insert(TenantPermission::LIST_DATABASES);
            perms.insert(TenantPermission::LIST_COLLECTIONS);
            break;

        case TenantRole::TENANT_CUSTOM:
            // No default permissions - must be set explicitly
            break;
    }

    return perms;
}

// ============================================================================
// TENANT USER (user within a tenant's isolated space)
// ============================================================================

struct TenantUser {
    std::string username;           // Unique within tenant
    std::string tenantId;           // The tenant this user belongs to
    std::string passwordHash;
    std::string salt;
    TenantRole role;
    std::unordered_set<TenantPermission> customPermissions;  // For TENANT_CUSTOM role
    std::vector<std::string> allowedDatabases;  // Empty = all within tenant
    bool isActive;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point lastLogin;
    int failedLoginAttempts;
    bool isLocked;
    json metadata;                  // Additional user metadata

    json toJson() const {
        json permsArray = json::array();
        for (auto p : customPermissions) {
            permsArray.push_back(static_cast<int>(p));
        }

        return {
            {"username", username},
            {"tenantId", tenantId},
            {"passwordHash", passwordHash},
            {"salt", salt},
            {"role", tenantRoleToString(role)},
            {"customPermissions", permsArray},
            {"allowedDatabases", allowedDatabases},
            {"isActive", isActive},
            {"createdAt", std::chrono::duration_cast<std::chrono::seconds>(
                createdAt.time_since_epoch()).count()},
            {"lastLogin", std::chrono::duration_cast<std::chrono::seconds>(
                lastLogin.time_since_epoch()).count()},
            {"failedLoginAttempts", failedLoginAttempts},
            {"isLocked", isLocked},
            {"metadata", metadata}
        };
    }

    static TenantUser fromJson(const json& j) {
        TenantUser u;
        u.username = j.value("username", "");
        u.tenantId = j.value("tenantId", "");
        u.passwordHash = j.value("passwordHash", "");
        u.salt = j.value("salt", "");
        u.role = stringToTenantRole(j.value("role", "tenant_readonly"));

        if (j.contains("customPermissions") && j["customPermissions"].is_array()) {
            for (auto& p : j["customPermissions"]) {
                u.customPermissions.insert(static_cast<TenantPermission>(p.get<int>()));
            }
        }

        u.allowedDatabases = j.value("allowedDatabases", std::vector<std::string>{});
        u.isActive = j.value("isActive", true);
        u.createdAt = std::chrono::system_clock::time_point(
            std::chrono::seconds(j.value("createdAt", 0L)));
        u.lastLogin = std::chrono::system_clock::time_point(
            std::chrono::seconds(j.value("lastLogin", 0L)));
        u.failedLoginAttempts = j.value("failedLoginAttempts", 0);
        u.isLocked = j.value("isLocked", false);
        u.metadata = j.value("metadata", json::object());
        return u;
    }
};

// ============================================================================
// TENANT (isolated workspace)
// ============================================================================

struct Tenant {
    std::string tenantId;           // Unique tenant identifier
    std::string name;               // Human-readable name
    std::string ownerUsername;      // The admin/owner of this tenant
    std::chrono::system_clock::time_point createdAt;
    bool isActive;

    // Resource limits (like MongoDB Atlas quotas)
    size_t maxDatabases;
    size_t maxCollectionsPerDb;
    size_t maxStorageBytes;
    size_t maxUsersPerTenant;

    // Usage tracking
    size_t currentDatabaseCount;
    size_t currentStorageBytes;
    size_t currentUserCount;

    json metadata;

    json toJson() const {
        return {
            {"tenantId", tenantId},
            {"name", name},
            {"ownerUsername", ownerUsername},
            {"createdAt", std::chrono::duration_cast<std::chrono::seconds>(
                createdAt.time_since_epoch()).count()},
            {"isActive", isActive},
            {"maxDatabases", maxDatabases},
            {"maxCollectionsPerDb", maxCollectionsPerDb},
            {"maxStorageBytes", maxStorageBytes},
            {"maxUsersPerTenant", maxUsersPerTenant},
            {"currentDatabaseCount", currentDatabaseCount},
            {"currentStorageBytes", currentStorageBytes},
            {"currentUserCount", currentUserCount},
            {"metadata", metadata}
        };
    }

    static Tenant fromJson(const json& j) {
        Tenant t;
        t.tenantId = j.value("tenantId", "");
        t.name = j.value("name", "");
        t.ownerUsername = j.value("ownerUsername", "");
        t.createdAt = std::chrono::system_clock::time_point(
            std::chrono::seconds(j.value("createdAt", 0L)));
        t.isActive = j.value("isActive", true);
        t.maxDatabases = j.value("maxDatabases", 100UL);
        t.maxCollectionsPerDb = j.value("maxCollectionsPerDb", 500UL);
        t.maxStorageBytes = j.value("maxStorageBytes", 10UL * 1024 * 1024 * 1024); // 10GB default
        t.maxUsersPerTenant = j.value("maxUsersPerTenant", 100UL);
        t.currentDatabaseCount = j.value("currentDatabaseCount", 0UL);
        t.currentStorageBytes = j.value("currentStorageBytes", 0UL);
        t.currentUserCount = j.value("currentUserCount", 0UL);
        t.metadata = j.value("metadata", json::object());
        return t;
    }
};

// ============================================================================
// TENANT SESSION TOKEN
// ============================================================================

struct TenantSession {
    std::string token;
    std::string tenantId;
    std::string username;
    TenantRole role;
    std::chrono::system_clock::time_point issuedAt;
    std::chrono::system_clock::time_point expiresAt;
    std::string clientIP;

    bool isExpired() const {
        return std::chrono::system_clock::now() > expiresAt;
    }

    json toJson() const {
        return {
            {"token", token},
            {"tenantId", tenantId},
            {"username", username},
            {"role", tenantRoleToString(role)},
            {"issuedAt", std::chrono::duration_cast<std::chrono::seconds>(
                issuedAt.time_since_epoch()).count()},
            {"expiresAt", std::chrono::duration_cast<std::chrono::seconds>(
                expiresAt.time_since_epoch()).count()},
            {"clientIP", clientIP}
        };
    }
};

// ============================================================================
// TENANT MANAGER - Core isolation engine
// ============================================================================

class TenantManager {
public:
    static TenantManager& instance() {
        static TenantManager inst;
        return inst;
    }

    // Initialization
    void initialize(const std::string& dataRoot);
    void loadTenants();
    void saveTenants();
    void loadTenantUsers(const std::string& tenantId);
    void saveTenantUsers(const std::string& tenantId);

    // ==================== TENANT OPERATIONS ====================

    // Create a new isolated tenant workspace
    json createTenant(const std::string& tenantId,
                      const std::string& name,
                      const std::string& ownerUsername,
                      const std::string& ownerPassword,
                      const json& options = json::object());

    // Delete a tenant and ALL its data (dangerous!)
    json deleteTenant(const std::string& tenantId, const std::string& confirmedBy);

    // Get tenant info
    std::optional<Tenant> getTenant(const std::string& tenantId);
    json listTenants();

    // Update tenant settings
    json updateTenant(const std::string& tenantId, const json& updates);

    // ==================== TENANT USER OPERATIONS ====================

    // Create user within a tenant (only tenant admins can do this)
    json createTenantUser(const std::string& tenantId,
                          const std::string& username,
                          const std::string& password,
                          TenantRole role,
                          const std::string& createdBy);

    // Delete user within a tenant
    json deleteTenantUser(const std::string& tenantId,
                          const std::string& username,
                          const std::string& deletedBy);

    // Update user role within tenant
    json updateTenantUserRole(const std::string& tenantId,
                              const std::string& username,
                              TenantRole newRole,
                              const std::string& updatedBy);

    // Update user password
    json updateTenantUserPassword(const std::string& tenantId,
                                   const std::string& username,
                                   const std::string& newPassword,
                                   const std::string& updatedBy);

    // Set database access for user within tenant
    json setTenantUserDatabaseAccess(const std::string& tenantId,
                                      const std::string& username,
                                      const std::vector<std::string>& databases,
                                      const std::string& updatedBy);

    // Get user within tenant
    std::optional<TenantUser> getTenantUser(const std::string& tenantId,
                                             const std::string& username);

    // List users within tenant
    json listTenantUsers(const std::string& tenantId);

    // ==================== AUTHENTICATION ====================

    // Authenticate user within tenant scope
    json authenticateTenantUser(const std::string& tenantId,
                                const std::string& username,
                                const std::string& password,
                                const std::string& clientIP);

    // Validate session token
    bool validateSession(const std::string& token);

    // Get session info
    std::optional<TenantSession> getSession(const std::string& token);

    // Revoke session
    void revokeSession(const std::string& token);

    // Revoke all sessions for a user
    void revokeAllUserSessions(const std::string& tenantId, const std::string& username);

    // ==================== AUTHORIZATION ====================

    // Check if session has permission within its tenant
    bool hasPermission(const std::string& token, TenantPermission perm);

    // Check if session can access specific database within tenant
    bool canAccessDatabase(const std::string& token, const std::string& database);

    // Check if session can perform action on resource
    json checkAccess(const std::string& token,
                     const std::string& action,
                     const std::string& database = "",
                     const std::string& collection = "");

    // ==================== ISOLATED OPERATIONS ====================
    // These ensure operations stay within tenant boundaries

    // Create database within tenant
    json createDatabaseInTenant(const std::string& token,
                                const std::string& dbName,
                                const std::string& dbType = "document");

    // Drop database within tenant
    json dropDatabaseInTenant(const std::string& token, const std::string& dbName);

    // List databases within tenant
    json listDatabasesInTenant(const std::string& token);

    // Create collection within tenant's database
    json createCollectionInTenant(const std::string& token,
                                   const std::string& dbName,
                                   const std::string& collName);

    // Drop collection within tenant
    json dropCollectionInTenant(const std::string& token,
                                 const std::string& dbName,
                                 const std::string& collName);

    // Insert document within tenant
    json insertInTenant(const std::string& token,
                        const std::string& dbName,
                        const std::string& collName,
                        const json& document);

    // Find documents within tenant
    json findInTenant(const std::string& token,
                      const std::string& dbName,
                      const std::string& collName,
                      const json& filter);

    // Update documents within tenant
    json updateInTenant(const std::string& token,
                        const std::string& dbName,
                        const std::string& collName,
                        const json& filter,
                        const json& update);

    // Delete documents within tenant
    json deleteInTenant(const std::string& token,
                        const std::string& dbName,
                        const std::string& collName,
                        const json& filter);

    // Get tenant statistics
    json getTenantStats(const std::string& token);

    // ==================== AUDIT LOGGING ====================

    void logTenantAudit(const std::string& tenantId,
                        const std::string& username,
                        const std::string& action,
                        const std::string& resource,
                        const std::string& details,
                        bool success);

    json getTenantAuditLog(const std::string& tenantId, int limit = 100);

private:
    TenantManager() = default;

    std::string generateSalt();
    std::string hashPassword(const std::string& password, const std::string& salt);
    bool verifyPassword(const std::string& password, const std::string& salt,
                        const std::string& encodedHash);
    std::string generateToken();
    std::string getTenantDataPath(const std::string& tenantId);

    // Data storage
    std::string dataRoot_;
    std::unordered_map<std::string, Tenant> tenants_;
    std::unordered_map<std::string, std::unordered_map<std::string, TenantUser>> tenantUsers_;
    std::unordered_map<std::string, TenantSession> activeSessions_;

    // Thread safety
    std::mutex tenantMutex_;
    std::mutex sessionMutex_;

    // Settings
    int sessionExpiryMinutes_ = 60;
    int maxLoginAttempts_ = 5;
    int lockoutMinutes_ = 30;
};

} // namespace tenant
} // namespace pacificdb
