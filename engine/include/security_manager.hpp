#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>
#include <functional>
#include <atomic>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pacificdb {
namespace security {

// ============================================================================
// ROLE-BASED ACCESS CONTROL (RBAC)
// ============================================================================

enum class Permission {
    READ,
    WRITE,
    DELETE,
    ADMIN,
    CREATE_DB,
    DROP_DB,
    CREATE_COLLECTION,
    DROP_COLLECTION,
    MANAGE_USERS,
    VIEW_METRICS,
    BACKUP,
    RESTORE
};

enum class Role {
    SUPERADMIN,    // All permissions
    ADMIN,         // All except MANAGE_USERS
    WRITE,         // READ + WRITE + DELETE
    READ_ONLY,     // READ only
    BACKUP_OPERATOR, // BACKUP + RESTORE + READ
    METRICS_VIEWER   // VIEW_METRICS only
};

inline std::string roleToString(Role role) {
    switch (role) {
        case Role::SUPERADMIN: return "superadmin";
        case Role::ADMIN: return "admin";
        case Role::WRITE: return "write";
        case Role::READ_ONLY: return "read_only";
        case Role::BACKUP_OPERATOR: return "backup_operator";
        case Role::METRICS_VIEWER: return "metrics_viewer";
        default: return "unknown";
    }
}

inline Role stringToRole(const std::string& s) {
    if (s == "superadmin") return Role::SUPERADMIN;
    if (s == "admin") return Role::ADMIN;
    if (s == "write") return Role::WRITE;
    if (s == "read_only") return Role::READ_ONLY;
    if (s == "backup_operator") return Role::BACKUP_OPERATOR;
    if (s == "metrics_viewer") return Role::METRICS_VIEWER;
    return Role::READ_ONLY;
}

inline std::unordered_set<Permission> getPermissionsForRole(Role role) {
    std::unordered_set<Permission> perms;

    switch (role) {
        case Role::SUPERADMIN:
            perms.insert(Permission::MANAGE_USERS);
            // Fall through
        case Role::ADMIN:
            perms.insert(Permission::CREATE_DB);
            perms.insert(Permission::DROP_DB);
            perms.insert(Permission::CREATE_COLLECTION);
            perms.insert(Permission::DROP_COLLECTION);
            perms.insert(Permission::VIEW_METRICS);
            perms.insert(Permission::BACKUP);
            perms.insert(Permission::RESTORE);
            perms.insert(Permission::ADMIN);
            // Fall through
        case Role::WRITE:
            perms.insert(Permission::WRITE);
            perms.insert(Permission::DELETE);
            // Fall through
        case Role::READ_ONLY:
            perms.insert(Permission::READ);
            break;
        case Role::BACKUP_OPERATOR:
            perms.insert(Permission::READ);
            perms.insert(Permission::BACKUP);
            perms.insert(Permission::RESTORE);
            break;
        case Role::METRICS_VIEWER:
            perms.insert(Permission::VIEW_METRICS);
            perms.insert(Permission::READ);
            break;
    }

    return perms;
}

// ============================================================================
// USER MANAGEMENT
// ============================================================================

struct User {
    std::string username;
    std::string passwordHash;  // SHA-256 hashed
    std::string salt;
    Role role;
    std::vector<std::string> allowedDatabases;  // Empty = all databases
    bool isActive;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point lastLogin;
    int failedLoginAttempts;
    bool isLocked;

    json toJson() const {
        return {
            {"username", username},
            {"passwordHash", passwordHash},
            {"salt", salt},
            {"role", roleToString(role)},
            {"allowedDatabases", allowedDatabases},
            {"isActive", isActive},
            {"createdAt", std::chrono::duration_cast<std::chrono::seconds>(
                createdAt.time_since_epoch()).count()},
            {"lastLogin", std::chrono::duration_cast<std::chrono::seconds>(
                lastLogin.time_since_epoch()).count()},
            {"failedLoginAttempts", failedLoginAttempts},
            {"isLocked", isLocked}
        };
    }

    static User fromJson(const json& j) {
        User u;
        u.username = j.value("username", "");
        u.passwordHash = j.value("passwordHash", "");
        u.salt = j.value("salt", "");
        u.role = stringToRole(j.value("role", "read_only"));
        u.allowedDatabases = j.value("allowedDatabases", std::vector<std::string>{});
        u.isActive = j.value("isActive", true);
        u.createdAt = std::chrono::system_clock::time_point(
            std::chrono::seconds(j.value("createdAt", 0L)));
        u.lastLogin = std::chrono::system_clock::time_point(
            std::chrono::seconds(j.value("lastLogin", 0L)));
        u.failedLoginAttempts = j.value("failedLoginAttempts", 0);
        u.isLocked = j.value("isLocked", false);
        return u;
    }
};

// ============================================================================
// JWT TOKEN
// ============================================================================

struct JWTToken {
    std::string token;
    std::string username;
    Role role;
    std::chrono::system_clock::time_point issuedAt;
    std::chrono::system_clock::time_point expiresAt;
    std::string sessionId;

    bool isExpired() const {
        return std::chrono::system_clock::now() > expiresAt;
    }
};

struct ApiKeyRecord {
    std::string id;
    std::string name;
    Role role{Role::READ_ONLY};
    std::string secretHash;
    std::string createdBy;
    long long createdAt{0};
    long long lastUsedAt{0};
    long long revokedAt{0};

    json toPublicJson() const {
        const std::string apiRole = role == Role::ADMIN ? "admin" :
                                    role == Role::WRITE ? "readwrite" : "read";
        return {{"id", id}, {"name", name}, {"role", apiRole},
                {"created_by", createdBy}, {"created_at", createdAt},
                {"last_used_at", lastUsedAt}, {"revoked_at", revokedAt},
                {"active", revokedAt == 0}};
    }
};

// ============================================================================
// AUDIT LOG
// ============================================================================

enum class AuditAction {
    LOGIN_SUCCESS,
    LOGIN_FAILED,
    LOGOUT,
    CREATE_USER,
    DELETE_USER,
    UPDATE_USER,
    CREATE_DATABASE,
    DROP_DATABASE,
    CREATE_COLLECTION,
    DROP_COLLECTION,
    INSERT,
    UPDATE,
    DELETE,
    QUERY,
    BACKUP_CREATED,
    RESTORE_PERFORMED,
    PERMISSION_DENIED,
    TOKEN_REVOKED,
    ACCOUNT_LOCKED,
    CONFIG_CHANGED
};

inline std::string auditActionToString(AuditAction action) {
    switch (action) {
        case AuditAction::LOGIN_SUCCESS: return "LOGIN_SUCCESS";
        case AuditAction::LOGIN_FAILED: return "LOGIN_FAILED";
        case AuditAction::LOGOUT: return "LOGOUT";
        case AuditAction::CREATE_USER: return "CREATE_USER";
        case AuditAction::DELETE_USER: return "DELETE_USER";
        case AuditAction::UPDATE_USER: return "UPDATE_USER";
        case AuditAction::CREATE_DATABASE: return "CREATE_DATABASE";
        case AuditAction::DROP_DATABASE: return "DROP_DATABASE";
        case AuditAction::CREATE_COLLECTION: return "CREATE_COLLECTION";
        case AuditAction::DROP_COLLECTION: return "DROP_COLLECTION";
        case AuditAction::INSERT: return "INSERT";
        case AuditAction::UPDATE: return "UPDATE";
        case AuditAction::DELETE: return "DELETE";
        case AuditAction::QUERY: return "QUERY";
        case AuditAction::BACKUP_CREATED: return "BACKUP_CREATED";
        case AuditAction::RESTORE_PERFORMED: return "RESTORE_PERFORMED";
        case AuditAction::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case AuditAction::TOKEN_REVOKED: return "TOKEN_REVOKED";
        case AuditAction::ACCOUNT_LOCKED: return "ACCOUNT_LOCKED";
        case AuditAction::CONFIG_CHANGED: return "CONFIG_CHANGED";
        default: return "UNKNOWN";
    }
}

struct AuditLogEntry {
    std::chrono::system_clock::time_point timestamp;
    std::string username;
    std::string clientIP;
    AuditAction action;
    std::string resource;  // database/collection affected
    std::string details;   // Additional context
    bool success;

    json toJson() const {
        auto time = std::chrono::system_clock::to_time_t(timestamp);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");

        return {
            {"timestamp", ss.str()},
            {"username", username},
            {"clientIP", clientIP},
            {"action", auditActionToString(action)},
            {"resource", resource},
            {"details", details},
            {"success", success}
        };
    }
};

// ============================================================================
// SECURITY MANAGER
// ============================================================================

class SecurityManager {
public:
    static SecurityManager& instance() {
        static SecurityManager inst;
        return inst;
    }

    // Initialization
    void initialize(const std::string& configPath);
    void loadUsers();
    void saveUsers();
    // Body of saveUsers() for callers that already hold userMutex_.
    void saveUsersLocked();

    // User Management
    bool createUser(const std::string& username, const std::string& password,
                    Role role, const std::string& createdBy);
    bool deleteUser(const std::string& username, const std::string& deletedBy);
    bool updateUserRole(const std::string& username, Role newRole,
                        const std::string& updatedBy);
    bool updateUserPassword(const std::string& username, const std::string& newPassword,
                            const std::string& updatedBy);
    bool setUserDatabaseAccess(const std::string& username,
                               const std::vector<std::string>& databases,
                               const std::string& updatedBy);
    std::optional<User> getUser(const std::string& username);
    std::vector<User> listUsers();

    // Authentication
    std::optional<JWTToken> authenticate(const std::string& username,
                                          const std::string& password,
                                          const std::string& clientIP);
    bool validateToken(const std::string& token);
    std::optional<JWTToken> refreshToken(const std::string& token);
    void revokeToken(const std::string& token, const std::string& reason);
    void revokeAllTokens(const std::string& username);

    // Authorization
    bool hasPermission(const std::string& token, Permission perm);
    bool hasPermission(const std::string& token, Permission perm,
                       const std::string& database);
    bool checkAccess(const std::string& token, const std::string& action,
                     const std::string& database, const std::string& collection);
    Role getTokenRole(const std::string& token);
    std::string getTokenUsername(const std::string& token);

    // Local Community API keys. The full key is returned once by create only.
    json createApiKey(const std::string& name, const std::string& role,
                      const std::string& createdBy);
    json listApiKeys();
    json getApiKey(const std::string& id);
    bool revokeApiKey(const std::string& id, const std::string& revokedBy);
    std::optional<ApiKeyRecord> validateApiKey(const std::string& key);

    // Audit Logging
    void logAudit(const std::string& username, const std::string& clientIP,
                  AuditAction action, const std::string& resource,
                  const std::string& details, bool success);
    std::vector<AuditLogEntry> getAuditLog(int limit = 100,
                                            const std::string& username = "",
                                            const std::string& action = "");
    void rotateAuditLog();

    // Security Settings
    void setMaxLoginAttempts(int max) { maxLoginAttempts_ = max; }
    void setTokenExpiryMinutes(int minutes) { tokenExpiryMinutes_ = minutes; }
    void setLockoutDurationMinutes(int minutes) { lockoutDurationMinutes_ = minutes; }
    void enableAuditLogging(bool enable) { auditEnabled_ = enable; }

    // Metrics
    json getSecurityMetrics();

private:
    SecurityManager() : maxLoginAttempts_(5), tokenExpiryMinutes_(60),
                        lockoutDurationMinutes_(30), auditEnabled_(true) {}

    void initializeDefaultAdmin();
    std::string generateSalt();
    std::string hashPassword(const std::string& password, const std::string& salt);
    bool verifyPassword(const std::string& password, const std::string& salt,
                        const std::string& encodedHash);
    std::string generateToken();
    std::string generateSessionId();
    bool unlockAccountIfExpired(User& user);
    void loadApiKeys();
    void saveApiKeysLocked();

    std::unordered_map<std::string, User> users_;
    std::unordered_map<std::string, JWTToken> activeTokens_;
    std::unordered_set<std::string> revokedTokens_;
    std::unordered_map<std::string, ApiKeyRecord> apiKeys_;
    std::vector<AuditLogEntry> auditLog_;

    std::string configPath_;
    std::mutex userMutex_;
    std::mutex tokenMutex_;
    std::mutex auditMutex_;
    std::mutex apiKeyMutex_;

    int maxLoginAttempts_;
    int tokenExpiryMinutes_;
    int lockoutDurationMinutes_;
    bool auditEnabled_;

    // Metrics
    std::atomic<uint64_t> totalLogins_{0};
    std::atomic<uint64_t> failedLogins_{0};
    std::atomic<uint64_t> permissionDenials_{0};
};

} // namespace security
} // namespace pacificdb
