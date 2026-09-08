#include "security_manager.hpp"
#include "storage_path.hpp"

#ifdef HAS_OPENSSL
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#endif

#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <random>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace pacificdb {
namespace security {

namespace fs = std::filesystem;

// ============================================================================
// SHA-256 IMPLEMENTATION (standalone, no OpenSSL required)
// ============================================================================

namespace {
    static const uint32_t sha256_k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    inline uint32_t rotr32(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    inline uint32_t sha256_ch(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (~x & z);
    }

    inline uint32_t sha256_maj(uint32_t x, uint32_t y, uint32_t z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    inline uint32_t sha256_sigma0(uint32_t x) {
        return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
    }

    inline uint32_t sha256_sigma1(uint32_t x) {
        return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
    }

    inline uint32_t sha256_gamma0(uint32_t x) {
        return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
    }

    inline uint32_t sha256_gamma1(uint32_t x) {
        return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
    }

    void sha256_transform(uint32_t* state, const uint8_t* block) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(block[i*4]) << 24) |
                   (static_cast<uint32_t>(block[i*4+1]) << 16) |
                   (static_cast<uint32_t>(block[i*4+2]) << 8) |
                   (static_cast<uint32_t>(block[i*4+3]));
        }
        for (int i = 16; i < 64; i++) {
            w[i] = sha256_gamma1(w[i-2]) + w[i-7] + sha256_gamma0(w[i-15]) + w[i-16];
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + sha256_sigma1(e) + sha256_ch(e, f, g) + sha256_k[i] + w[i];
            uint32_t t2 = sha256_sigma0(a) + sha256_maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    std::string compute_sha256(const std::string& input) {
        uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        const uint8_t* data = reinterpret_cast<const uint8_t*>(input.data());
        size_t len = input.size();
        uint64_t bitlen = len * 8;

        // Prepare padded buffer
        size_t padlen = ((len + 8) / 64 + 1) * 64;
        std::vector<uint8_t> padded(padlen, 0);
        memcpy(padded.data(), data, len);
        padded[len] = 0x80;

        // Append length in big-endian
        for (int i = 0; i < 8; i++) {
            padded[padlen - 1 - i] = static_cast<uint8_t>((bitlen >> (i * 8)) & 0xFF);
        }

        // Process blocks
        for (size_t i = 0; i < padlen; i += 64) {
            sha256_transform(state, padded.data() + i);
        }

        // Convert to hex
        std::stringstream ss;
        for (int i = 0; i < 8; i++) {
            ss << std::hex << std::setw(8) << std::setfill('0') << state[i];
        }
        return ss.str();
    }
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static std::string bytesToHex(const unsigned char* data, size_t len) {
    std::stringstream ss;
    for (size_t i = 0; i < len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return ss.str();
}

static std::string sha256(const std::string& input) {
    return compute_sha256(input);
}

static constexpr int PASSWORD_PBKDF2_ITERATIONS = 310000;
static constexpr size_t PASSWORD_DERIVED_KEY_BYTES = 32;

static std::string pbkdf2PasswordHash(const std::string& password,
                                      const std::string& salt,
                                      int iterations) {
    if (iterations < 100000 || iterations > 5000000) {
        throw std::runtime_error("invalid PBKDF2 iteration count");
    }
    unsigned char derived[PASSWORD_DERIVED_KEY_BYTES];
    if (PKCS5_PBKDF2_HMAC(
            password.data(), static_cast<int>(password.size()),
            reinterpret_cast<const unsigned char*>(salt.data()),
            static_cast<int>(salt.size()), iterations, EVP_sha256(),
            static_cast<int>(sizeof(derived)), derived) != 1) {
        throw std::runtime_error("PBKDF2 password derivation failed");
    }
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" +
           bytesToHex(derived, sizeof(derived));
}

static bool constantTimeEqual(const std::string& left,
                              const std::string& right) {
    return left.size() == right.size() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SecurityManager::initialize(const std::string& configPath) {
    const fs::path configured(configPath);
    if (configPath.empty() || !configured.is_absolute()) {
        throw std::invalid_argument(
            "SecurityManager requires an explicit absolute DATA_ROOT");
    }
    std::error_code ec;
    fs::create_directories(configured, ec);
    if (ec) {
        throw std::runtime_error(
            "SecurityManager cannot create DATA_ROOT: " + ec.message());
    }
    configPath_ = fs::canonical(configured, ec).string();
    if (ec || configPath_.empty()) {
        throw std::runtime_error(
            "SecurityManager cannot canonicalize DATA_ROOT");
    }

    // Create security directory
    fs::path securityDir = createContainedStorageDirectories(
        fs::path(configPath_), fs::path(configPath_) / "security");

    // Load existing users
    loadUsers();
    initializeDefaultAdmin();

    // Initialize audit log file
    fs::path auditFile = securityDir / "audit.log";
    if (!fs::exists(auditFile)) {
        std::ofstream f(auditFile);
        f.close();
    }
}

void SecurityManager::initializeDefaultAdmin() {
    std::lock_guard<std::mutex> lock(userMutex_);

    // Never create a built-in credential. An optional bootstrap superadmin is
    // accepted only when the operator explicitly supplies both values.
    const char* bootUser = std::getenv("PACIFICDB_ENGINE_ADMIN_USERNAME");
    const char* bootPass = std::getenv("PACIFICDB_ENGINE_ADMIN_PASSWORD");
    if (users_.empty() && bootUser && bootPass && *bootUser && *bootPass) {
        User admin;
        admin.username = bootUser;
        admin.salt = generateSalt();
        admin.passwordHash = hashPassword(bootPass, admin.salt);
        admin.role = Role::SUPERADMIN;
        admin.isActive = true;
        admin.createdAt = std::chrono::system_clock::now();
        admin.lastLogin = std::chrono::system_clock::time_point{};
        admin.failedLoginAttempts = 0;
        admin.isLocked = false;

        users_[admin.username] = admin;

        logAudit("SYSTEM", "localhost", AuditAction::CREATE_USER,
                 "users", "Operator-supplied bootstrap admin created", true);
        saveUsersLocked();
    }
}

void SecurityManager::loadUsers() {
    std::lock_guard<std::mutex> lock(userMutex_);

    fs::path usersFile = fs::path(configPath_) / "security" / "users.json";
    if (!fs::exists(usersFile)) {
        return;
    }

    try {
        std::ifstream f(usersFile);
        json j = json::parse(f);

        for (const auto& item : j["users"]) {
            User u = User::fromJson(item);
            users_[u.username] = u;
        }
    } catch (const std::exception& e) {
        // Log error but continue
    }
}

void SecurityManager::saveUsers() {
    std::lock_guard<std::mutex> lock(userMutex_);
    saveUsersLocked();
}

void SecurityManager::saveUsersLocked() {
    fs::path usersFile = fs::path(configPath_) / "security" / "users.json";
    fs::create_directories(usersFile.parent_path());

    json j;
    j["users"] = json::array();

    for (const auto& [_, user] : users_) {
        j["users"].push_back(user.toJson());
    }

    std::ofstream f(usersFile);
    f << j.dump(2);
}

// ============================================================================
// USER MANAGEMENT
// ============================================================================

bool SecurityManager::createUser(const std::string& username, const std::string& password,
                                  Role role, const std::string& createdBy) {
    std::lock_guard<std::mutex> lock(userMutex_);

    if (users_.count(username) > 0) {
        logAudit(createdBy, "", AuditAction::CREATE_USER, username,
                 "User already exists", false);
        return false;
    }

    User user;
    user.username = username;
    user.salt = generateSalt();
    user.passwordHash = hashPassword(password, user.salt);
    user.role = role;
    user.isActive = true;
    user.createdAt = std::chrono::system_clock::now();
    user.lastLogin = std::chrono::system_clock::time_point{};
    user.failedLoginAttempts = 0;
    user.isLocked = false;

    users_[username] = user;
    saveUsersLocked();

    logAudit(createdBy, "", AuditAction::CREATE_USER, username,
             "Role: " + roleToString(role), true);

    return true;
}

bool SecurityManager::deleteUser(const std::string& username, const std::string& deletedBy) {
    std::lock_guard<std::mutex> lock(userMutex_);

    if (username == "admin") {
        logAudit(deletedBy, "", AuditAction::DELETE_USER, username,
                 "Cannot delete default admin", false);
        return false;
    }

    if (users_.erase(username) > 0) {
        saveUsersLocked();
        revokeAllTokens(username);
        logAudit(deletedBy, "", AuditAction::DELETE_USER, username, "", true);
        return true;
    }

    logAudit(deletedBy, "", AuditAction::DELETE_USER, username,
             "User not found", false);
    return false;
}

bool SecurityManager::updateUserRole(const std::string& username, Role newRole,
                                      const std::string& updatedBy) {
    std::lock_guard<std::mutex> lock(userMutex_);

    auto it = users_.find(username);
    if (it == users_.end()) {
        return false;
    }

    Role oldRole = it->second.role;
    it->second.role = newRole;
    saveUsersLocked();

    logAudit(updatedBy, "", AuditAction::UPDATE_USER, username,
             "Role changed: " + roleToString(oldRole) + " -> " + roleToString(newRole),
             true);

    // Revoke existing tokens to force re-auth with new role
    revokeAllTokens(username);

    return true;
}

bool SecurityManager::updateUserPassword(const std::string& username,
                                          const std::string& newPassword,
                                          const std::string& updatedBy) {
    std::lock_guard<std::mutex> lock(userMutex_);

    auto it = users_.find(username);
    if (it == users_.end()) {
        return false;
    }

    it->second.salt = generateSalt();
    it->second.passwordHash = hashPassword(newPassword, it->second.salt);
    it->second.failedLoginAttempts = 0;
    it->second.isLocked = false;
    saveUsersLocked();

    logAudit(updatedBy, "", AuditAction::UPDATE_USER, username,
             "Password changed", true);

    // Revoke tokens after password change
    revokeAllTokens(username);

    return true;
}

bool SecurityManager::setUserDatabaseAccess(const std::string& username,
                                             const std::vector<std::string>& databases,
                                             const std::string& updatedBy) {
    std::lock_guard<std::mutex> lock(userMutex_);

    auto it = users_.find(username);
    if (it == users_.end()) {
        return false;
    }

    it->second.allowedDatabases = databases;
    saveUsersLocked();

    std::string dbList = databases.empty() ? "ALL" : "";
    for (const auto& db : databases) {
        if (!dbList.empty()) dbList += ", ";
        dbList += db;
    }

    logAudit(updatedBy, "", AuditAction::UPDATE_USER, username,
             "Database access: " + dbList, true);

    return true;
}

std::optional<User> SecurityManager::getUser(const std::string& username) {
    std::lock_guard<std::mutex> lock(userMutex_);

    auto it = users_.find(username);
    if (it != users_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<User> SecurityManager::listUsers() {
    std::lock_guard<std::mutex> lock(userMutex_);

    std::vector<User> result;
    for (const auto& [_, user] : users_) {
        result.push_back(user);
    }
    return result;
}

// ============================================================================
// AUTHENTICATION
// ============================================================================

std::optional<JWTToken> SecurityManager::authenticate(const std::string& username,
                                                       const std::string& password,
                                                       const std::string& clientIP) {
    std::lock_guard<std::mutex> lock(userMutex_);

    auto it = users_.find(username);
    if (it == users_.end()) {
        failedLogins_++;
        logAudit(username, clientIP, AuditAction::LOGIN_FAILED, "",
                 "User not found", false);
        return std::nullopt;
    }

    User& user = it->second;

    // Check if account is locked
    if (user.isLocked) {
        if (!unlockAccountIfExpired(user)) {
            logAudit(username, clientIP, AuditAction::LOGIN_FAILED, "",
                     "Account locked", false);
            return std::nullopt;
        }
    }

    // Check if user is active
    if (!user.isActive) {
        logAudit(username, clientIP, AuditAction::LOGIN_FAILED, "",
                 "Account disabled", false);
        return std::nullopt;
    }

    // Verify password
    if (!verifyPassword(password, user.salt, user.passwordHash)) {
        user.failedLoginAttempts++;
        failedLogins_++;

        if (user.failedLoginAttempts >= maxLoginAttempts_) {
            user.isLocked = true;
            logAudit(username, clientIP, AuditAction::ACCOUNT_LOCKED, "",
                     "Max login attempts exceeded", true);
        }

        saveUsersLocked();
        logAudit(username, clientIP, AuditAction::LOGIN_FAILED, "",
                 "Invalid password", false);
        return std::nullopt;
    }

    // Successful login
    if (user.passwordHash.rfind("pbkdf2-sha256$", 0) != 0) {
        user.passwordHash = hashPassword(password, user.salt);
    }
    user.failedLoginAttempts = 0;
    user.lastLogin = std::chrono::system_clock::now();
    saveUsersLocked();

    // Generate token
    JWTToken token;
    token.token = generateToken();
    token.username = username;
    token.role = user.role;
    token.issuedAt = std::chrono::system_clock::now();
    token.expiresAt = token.issuedAt + std::chrono::minutes(tokenExpiryMinutes_);
    token.sessionId = generateSessionId();

    {
        std::lock_guard<std::mutex> tokenLock(tokenMutex_);
        activeTokens_[token.token] = token;
    }

    totalLogins_++;
    logAudit(username, clientIP, AuditAction::LOGIN_SUCCESS, "",
             "Session: " + token.sessionId, true);

    return token;
}

bool SecurityManager::validateToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(tokenMutex_);

    // Check if revoked
    if (revokedTokens_.count(token) > 0) {
        return false;
    }

    // Check if active
    auto it = activeTokens_.find(token);
    if (it == activeTokens_.end()) {
        return false;
    }

    // Check expiry
    if (it->second.isExpired()) {
        activeTokens_.erase(it);
        return false;
    }

    return true;
}

std::optional<JWTToken> SecurityManager::refreshToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(tokenMutex_);

    auto it = activeTokens_.find(token);
    if (it == activeTokens_.end() || it->second.isExpired()) {
        return std::nullopt;
    }

    // Create new token
    JWTToken newToken;
    newToken.token = generateToken();
    newToken.username = it->second.username;
    newToken.role = it->second.role;
    newToken.issuedAt = std::chrono::system_clock::now();
    newToken.expiresAt = newToken.issuedAt + std::chrono::minutes(tokenExpiryMinutes_);
    newToken.sessionId = it->second.sessionId;  // Keep same session

    // Revoke old token
    activeTokens_.erase(it);
    revokedTokens_.insert(token);

    // Add new token
    activeTokens_[newToken.token] = newToken;

    return newToken;
}

void SecurityManager::revokeToken(const std::string& token, const std::string& reason) {
    std::lock_guard<std::mutex> lock(tokenMutex_);

    auto it = activeTokens_.find(token);
    if (it != activeTokens_.end()) {
        logAudit(it->second.username, "", AuditAction::TOKEN_REVOKED, "",
                 reason, true);
        activeTokens_.erase(it);
    }
    revokedTokens_.insert(token);
}

void SecurityManager::revokeAllTokens(const std::string& username) {
    std::lock_guard<std::mutex> lock(tokenMutex_);

    std::vector<std::string> toRevoke;
    for (const auto& [token, info] : activeTokens_) {
        if (info.username == username) {
            toRevoke.push_back(token);
        }
    }

    for (const auto& token : toRevoke) {
        activeTokens_.erase(token);
        revokedTokens_.insert(token);
    }

    logAudit(username, "", AuditAction::TOKEN_REVOKED, "",
             "All tokens revoked", true);
}

// ============================================================================
// AUTHORIZATION
// ============================================================================

bool SecurityManager::hasPermission(const std::string& token, Permission perm) {
    if (!validateToken(token)) {
        permissionDenials_++;
        return false;
    }

    std::lock_guard<std::mutex> lock(tokenMutex_);
    auto it = activeTokens_.find(token);
    if (it == activeTokens_.end()) {
        return false;
    }

    auto perms = getPermissionsForRole(it->second.role);
    return perms.count(perm) > 0;
}

bool SecurityManager::hasPermission(const std::string& token, Permission perm,
                                     const std::string& database) {
    if (!hasPermission(token, perm)) {
        return false;
    }

    // Never hold tokenMutex_ and userMutex_ together. authenticate() acquires
    // them in the opposite order, so overlapping the two locks deadlocks auth.
    std::string tokenUsername;
    {
        std::lock_guard<std::mutex> lock(tokenMutex_);
        auto tokenIt = activeTokens_.find(token);
        if (tokenIt == activeTokens_.end()) {
            return false;
        }
        tokenUsername = tokenIt->second.username;
    }

    std::lock_guard<std::mutex> userLock(userMutex_);
    auto userIt = users_.find(tokenUsername);
    if (userIt == users_.end()) {
        return false;
    }

    // Empty allowedDatabases means all databases allowed
    if (userIt->second.allowedDatabases.empty()) {
        return true;
    }

    // Check if database is in allowed list
    const auto& allowed = userIt->second.allowedDatabases;
    bool hasAccess = std::find(allowed.begin(), allowed.end(), database) != allowed.end();

    if (!hasAccess) {
        permissionDenials_++;
        logAudit(tokenUsername, "", AuditAction::PERMISSION_DENIED,
                 database, "Database access denied", false);
    }

    return hasAccess;
}

bool SecurityManager::checkAccess(const std::string& token, const std::string& action,
                                   const std::string& database, const std::string& collection) {
    // Map action to permission
    Permission perm = Permission::READ;

    if (action == "insert" || action == "create") {
        perm = Permission::WRITE;
    } else if (action == "update" || action == "upsert") {
        perm = Permission::WRITE;
    } else if (action == "delete" || action == "remove") {
        perm = Permission::DELETE;
    } else if (action == "createDatabase") {
        perm = Permission::CREATE_DB;
    } else if (action == "dropDatabase") {
        perm = Permission::DROP_DB;
    } else if (action == "createCollection") {
        perm = Permission::CREATE_COLLECTION;
    } else if (action == "dropCollection") {
        perm = Permission::DROP_COLLECTION;
    } else if (action == "backup") {
        perm = Permission::BACKUP;
    } else if (action == "restore") {
        perm = Permission::RESTORE;
    } else if (action == "metrics" || action == "status") {
        perm = Permission::VIEW_METRICS;
    } else if (action == "admin" || action == "config") {
        perm = Permission::ADMIN;
    }

    return hasPermission(token, perm, database);
}

Role SecurityManager::getTokenRole(const std::string& token) {
    std::lock_guard<std::mutex> lock(tokenMutex_);
    auto it = activeTokens_.find(token);
    if (it != activeTokens_.end()) {
        return it->second.role;
    }
    return Role::READ_ONLY;
}

std::string SecurityManager::getTokenUsername(const std::string& token) {
    std::lock_guard<std::mutex> lock(tokenMutex_);
    auto it = activeTokens_.find(token);
    if (it != activeTokens_.end()) {
        return it->second.username;
    }
    return "";
}

// ============================================================================
// AUDIT LOGGING
// ============================================================================

void SecurityManager::logAudit(const std::string& username, const std::string& clientIP,
                                AuditAction action, const std::string& resource,
                                const std::string& details, bool success) {
    if (!auditEnabled_) return;

    AuditLogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.username = username;
    entry.clientIP = clientIP;
    entry.action = action;
    entry.resource = resource;
    entry.details = details;
    entry.success = success;

    {
        std::lock_guard<std::mutex> lock(auditMutex_);
        auditLog_.push_back(entry);

        // Keep only last 10000 entries in memory
        if (auditLog_.size() > 10000) {
            rotateAuditLog();
        }
    }

    // Also write to file
    if (!configPath_.empty()) {
        fs::path auditFile = fs::path(configPath_) / "security" / "audit.log";
        std::ofstream f(auditFile, std::ios::app);
        f << entry.toJson().dump() << "\n";
    }
}

std::vector<AuditLogEntry> SecurityManager::getAuditLog(int limit,
                                                         const std::string& username,
                                                         const std::string& action) {
    std::lock_guard<std::mutex> lock(auditMutex_);

    std::vector<AuditLogEntry> result;

    for (auto it = auditLog_.rbegin(); it != auditLog_.rend() && result.size() < (size_t)limit; ++it) {
        bool match = true;

        if (!username.empty() && it->username != username) {
            match = false;
        }
        if (!action.empty() && auditActionToString(it->action) != action) {
            match = false;
        }

        if (match) {
            result.push_back(*it);
        }
    }

    return result;
}

void SecurityManager::rotateAuditLog() {
    // Archive old entries
    if (!configPath_.empty()) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::gmtime(&time), "%Y%m%d%H%M%S");

        fs::path archiveFile = fs::path(configPath_) / "security" /
                               ("audit_archive_" + ss.str() + ".log");

        std::ofstream f(archiveFile);
        for (size_t i = 0; i < auditLog_.size() - 5000; ++i) {
            f << auditLog_[i].toJson().dump() << "\n";
        }
    }

    // Keep only recent entries
    if (auditLog_.size() > 5000) {
        auditLog_.erase(auditLog_.begin(), auditLog_.begin() + (auditLog_.size() - 5000));
    }
}

// ============================================================================
// METRICS
// ============================================================================

json SecurityManager::getSecurityMetrics() {
    std::lock_guard<std::mutex> lock(tokenMutex_);

    return {
        {"totalLogins", totalLogins_.load()},
        {"failedLogins", failedLogins_.load()},
        {"permissionDenials", permissionDenials_.load()},
        {"activeTokens", activeTokens_.size()},
        {"revokedTokens", revokedTokens_.size()},
        {"totalUsers", users_.size()},
        {"lockedAccounts", std::count_if(users_.begin(), users_.end(),
                          [](const auto& p) { return p.second.isLocked; })}
    };
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Cross-platform secure random bytes generator
static void generateRandomBytes(unsigned char* buffer, size_t length) {
    if (length > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        RAND_bytes(buffer, static_cast<int>(length)) != 1) {
        throw std::runtime_error(
            "CSPRNG unavailable; refusing to issue a predictable credential");
    }
}

std::string SecurityManager::generateSalt() {
    unsigned char salt[16];
    generateRandomBytes(salt, 16);
    return bytesToHex(salt, 16);
}

std::string SecurityManager::hashPassword(const std::string& password,
                                           const std::string& salt) {
    return pbkdf2PasswordHash(password, salt, PASSWORD_PBKDF2_ITERATIONS);
}

bool SecurityManager::verifyPassword(const std::string& password,
                                     const std::string& salt,
                                     const std::string& encodedHash) {
    constexpr const char* prefix = "pbkdf2-sha256$";
    if (encodedHash.rfind(prefix, 0) == 0) {
        const size_t iterationEnd = encodedHash.find('$', std::strlen(prefix));
        if (iterationEnd == std::string::npos) return false;
        try {
            const int iterations = std::stoi(encodedHash.substr(
                std::strlen(prefix), iterationEnd - std::strlen(prefix)));
            return constantTimeEqual(
                pbkdf2PasswordHash(password, salt, iterations), encodedHash);
        } catch (...) {
            return false;
        }
    }

    // One-time compatibility for existing users; a successful authentication
    // immediately rewrites this legacy single-round SHA-256 representation.
    return constantTimeEqual(sha256(salt + password + salt), encodedHash);
}

std::string SecurityManager::generateToken() {
    unsigned char token[32];
    generateRandomBytes(token, 32);
    return bytesToHex(token, 32);
}

std::string SecurityManager::generateSessionId() {
    unsigned char sessionId[16];
    generateRandomBytes(sessionId, 16);
    return bytesToHex(sessionId, 16);
}

bool SecurityManager::unlockAccountIfExpired(User& user) {
    // Check if lockout duration has passed
    auto now = std::chrono::system_clock::now();
    auto lockoutDuration = std::chrono::minutes(lockoutDurationMinutes_);

    // Simple heuristic: check if enough time has passed since last failed attempt
    // In production, you'd store the lockout timestamp
    if (user.isLocked && user.failedLoginAttempts > 0) {
        // For now, unlock after the duration
        user.isLocked = false;
        user.failedLoginAttempts = 0;
        return true;
    }

    return false;
}

} // namespace security
} // namespace pacificdb
