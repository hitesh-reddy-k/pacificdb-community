#include "env_config.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

// Static member definitions
std::unordered_map<std::string, std::string> EnvConfig::values_;
std::mutex EnvConfig::mutex_;
bool EnvConfig::loaded_ = false;

void EnvConfig::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return;

    // Runtime configuration must not change with process cwd. Load only an
    // explicitly named absolute file and, when DATA_ROOT itself is explicitly
    // absolute, the root-owned .env. Process environment still wins below.
    std::vector<std::pair<std::string, bool>> searchPaths;
    if (const char* explicitFile = std::getenv("PACIFICDB_ENV_FILE")) {
        const std::filesystem::path file(explicitFile);
        if (!file.is_absolute()) {
            throw std::runtime_error(
                "PACIFICDB_ENV_FILE must be an absolute path");
        }
        searchPaths.emplace_back(file.string(), true);
    }

    // Add DATA_ROOT/.env if DATA_ROOT is set
    const char* dataRoot = std::getenv("DATA_ROOT");
    if (dataRoot && *dataRoot) {
        const std::filesystem::path root(dataRoot);
        if (root.is_absolute()) {
            searchPaths.emplace_back((root / ".env").string(), false);
        }
    }

    for (const auto& [path, required] : searchPaths) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            if (required) {
                throw std::runtime_error(
                    "cannot inspect PACIFICDB_ENV_FILE: " + error.message());
            }
            continue;
        }
        if (!exists) {
            if (required) {
                throw std::runtime_error("PACIFICDB_ENV_FILE does not exist");
            }
            continue;
        }
        std::cout << "[EnvConfig] Loading: " << path << "\n";
        parseEnvFile(path);
    }

    // Process environment variables ALWAYS take precedence
    loadProcessEnvironment();

    loaded_ = true;
    std::cout << "[EnvConfig] Loaded " << values_.size() << " configuration entries\n";
}

void EnvConfig::loadFromFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    parseEnvFile(filepath);
}

void EnvConfig::reload() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        values_.clear();
        loaded_ = false;
    }
    load();
}

void EnvConfig::parseEnvFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return;

    std::string line;
    int lineNum = 0;
    while (std::getline(file, line)) {
        lineNum++;
        line = trim(line);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));

        // Strip surrounding quotes
        if (value.size() >= 2) {
            if ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }

        if (!key.empty()) {
            values_[key] = value;
        }
    }
}

void EnvConfig::loadProcessEnvironment() {
    // Process env vars override .env file values, but ONLY for keys that we
    // already know about (defined in .env files) OR have known PacificDB prefixes.
    // This prevents pollution from unrelated system environment variables.
    static const std::vector<std::string> knownPrefixes = {
        "PACIFICDB_", "PACIFIC_", "ENGINE_", "DATA_", "WAL_", "SST_", "SNAPSHOT_",
        "BACKUP_", "RESTORE_", "TMP_", "LOG_", "RAFT_",
        "TLS_", "RBAC_", "JWT_", "API_KEYS_", "AUDIT_", "ENCRYPTION_",
        "MEMTABLE_", "BLOCK_CACHE_",
        "INDEX_", "MAX_", "LEVEL_", "SST_", "COMPACTION_", "BLOOM_",
        "QUERY_", "SLOW_", "CONN_", "DBQ_", "MULTI_", "TENANT_",
        "DEFAULT_TENANT_", "PROMETHEUS_", "TRACING_", "HEALTH_",
        "READY_", "LIVE_", "STRUCTURED_", "REQUEST_", "SNAPSHOT_",
        "ADMISSION_", "ANTI_",
        "DEGRADED_", "OOM_", "DISK_", "TXN_", "ENABLE_",
        "MIN_QUORUM_", "PRESIGNED_", "REGION", "ZONE", "NODE_NAME",
        "CLUSTER_NAME", "READ_ONLY_MODE", "MEMORY_PRESSURE_THRESHOLD_PCT",
        "MEMORY_BACKPRESSURE_ENABLED", "METRICS_PORT", "METRICS_INTERVAL_MS",
        "FAILURE_DETECTOR_", "HEARTBEAT_MESH_",
        "NODE_ENV", "LOG_LEVEL",
    };

    auto matchesKnownPrefix = [](const std::string& key) -> bool {
        for (const auto& prefix : knownPrefixes) {
            if (key.rfind(prefix, 0) == 0) return true;
        }
        return false;
    };

    extern char** environ;
    for (char** env = environ; *env != nullptr; env++) {
        std::string entry(*env);
        auto pos = entry.find('=');
        if (pos == std::string::npos) continue;

        std::string key = entry.substr(0, pos);
        std::string value = entry.substr(pos + 1);

        if (key.empty()) continue;

        // Override only if:
        //   1. Key is already loaded from .env file (already in values_), OR
        //   2. Key matches a known PacificDB prefix
        bool alreadyKnown = values_.find(key) != values_.end();
        bool hasKnownPrefix = matchesKnownPrefix(key);

        if (alreadyKnown || hasKnownPrefix) {
            values_[key] = value;
        }
    }
}

std::string EnvConfig::trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string EnvConfig::getString(const std::string& key, const std::string& defaultValue) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    if (it != values_.end() && !it->second.empty()) return it->second;
    return defaultValue;
}

int EnvConfig::getInt(const std::string& key, int defaultValue) {
    std::string val = getString(key, "");
    if (val.empty()) return defaultValue;
    try {
        return std::stoi(val);
    } catch (...) {
        return defaultValue;
    }
}

int64_t EnvConfig::getInt64(const std::string& key, int64_t defaultValue) {
    std::string val = getString(key, "");
    if (val.empty()) return defaultValue;
    try {
        return std::stoll(val);
    } catch (...) {
        return defaultValue;
    }
}

size_t EnvConfig::getSize(const std::string& key, size_t defaultValue) {
    std::string val = getString(key, "");
    if (val.empty()) return defaultValue;
    try {
        return std::stoull(val);
    } catch (...) {
        return defaultValue;
    }
}

bool EnvConfig::getBool(const std::string& key, bool defaultValue) {
    std::string val = getString(key, "");
    if (val.empty()) return defaultValue;

    // Normalize to lowercase
    std::transform(val.begin(), val.end(), val.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (val == "true" || val == "1" || val == "yes" || val == "on" || val == "enabled") return true;
    if (val == "false" || val == "0" || val == "no" || val == "off" || val == "disabled") return false;
    return defaultValue;
}

double EnvConfig::getDouble(const std::string& key, double defaultValue) {
    std::string val = getString(key, "");
    if (val.empty()) return defaultValue;
    try {
        return std::stod(val);
    } catch (...) {
        return defaultValue;
    }
}

std::vector<std::string> EnvConfig::getList(const std::string& key, const std::vector<std::string>& defaultValue) {
    std::string val = getString(key, "");
    if (val.empty()) return defaultValue;

    std::vector<std::string> result;
    std::stringstream ss(val);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) result.push_back(item);
    }
    return result;
}

bool EnvConfig::has(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return values_.find(key) != values_.end();
}

void EnvConfig::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = value;
}

std::unordered_map<std::string, std::string> EnvConfig::getAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    return values_;
}

bool EnvConfig::isSensitive(const std::string& key) {
    static const std::vector<std::string> sensitivePatterns = {
        "SECRET", "PASSWORD", "PASSWD", "TOKEN", "PRIVATE_KEY",
        "ACCESS_KEY", "API_KEY",
    };
    for (const auto& pattern : sensitivePatterns) {
        if (key.find(pattern) != std::string::npos) return true;
    }
    return false;
}

std::string EnvConfig::maskSensitive(const std::string& key, const std::string& value) {
    if (!isSensitive(key) || value.empty()) return value;
    if (value.size() <= 4) return "****";
    return value.substr(0, 2) + std::string(value.size() - 4, '*') + value.substr(value.size() - 2);
}

void EnvConfig::dump() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "\n========== EnvConfig Dump ==========\n";
    std::vector<std::string> keys;
    for (const auto& [k, v] : values_) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    for (const auto& key : keys) {
        std::cout << "  " << key << " = " << maskSensitive(key, values_[key]) << "\n";
    }
    std::cout << "====================================\n\n";
}

std::string EnvConfig::toJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::stringstream ss;
    ss << "{";
    bool first = true;
    std::vector<std::string> keys;
    for (const auto& [k, v] : values_) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    for (const auto& key : keys) {
        if (!first) ss << ",";
        first = false;
        std::string value = maskSensitive(key, values_[key]);
        // Escape quotes and backslashes for JSON
        std::string escaped;
        for (char c : value) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        ss << "\"" << key << "\":\"" << escaped << "\"";
    }
    ss << "}";
    return ss.str();
}

// ==================== STRUCTURED ACCESSORS ====================

EnvConfig::StorageConfig EnvConfig::getStorageConfig() {
    namespace fs = std::filesystem;
    auto requireAbsolute = [](const std::string& value,
                              const std::string& field) -> std::string {
        if (value.empty()) {
            throw std::runtime_error(field + " is required");
        }
        const fs::path root(value);
        if (!root.is_absolute()) {
            throw std::runtime_error(
                field + " must be an explicit absolute path");
        }
        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(root, ec);
        return (ec ? root.lexically_normal() : canonical).string();
    };

    StorageConfig cfg;
    const std::string configuredDataRoot = getString("DATA_ROOT", "");
    cfg.dataRoot = requireAbsolute(configuredDataRoot, "DATA_ROOT");

    // DATA_DIR remains a compatibility alias only when it resolves to exactly
    // the same canonical root. It may never silently override DATA_ROOT.
    std::string dataDir = getString("DATA_DIR", "");
    if (!dataDir.empty()) {
        const std::string canonicalDataDir =
            requireAbsolute(dataDir, "DATA_DIR");
        if (canonicalDataDir != cfg.dataRoot) {
            throw std::runtime_error(
                "DATA_DIR conflicts with canonical DATA_ROOT");
        }
    }

    const std::string backupRoot = getString("BACKUP_ROOT", "");
    const std::string backupDir = getString("BACKUP_DIR", "");
    if (backupRoot.empty() && backupDir.empty()) {
        throw std::runtime_error(
            "BACKUP_ROOT or BACKUP_DIR is required");
    }
    const std::string canonicalBackup = requireAbsolute(
        backupRoot.empty() ? backupDir : backupRoot,
        backupRoot.empty() ? "BACKUP_DIR" : "BACKUP_ROOT");
    if (!backupRoot.empty() && !backupDir.empty() &&
        canonicalBackup != requireAbsolute(backupDir, "BACKUP_DIR")) {
        throw std::runtime_error(
            "BACKUP_DIR conflicts with canonical BACKUP_ROOT");
    }

    cfg.walDir = requireAbsolute(
        getString("WAL_DIR", cfg.dataRoot + "/wal"), "WAL_DIR");
    cfg.sstDir = requireAbsolute(
        getString("SST_DIR", cfg.dataRoot + "/sst"), "SST_DIR");
    cfg.snapshotDir = requireAbsolute(
        getString("SNAPSHOT_DIR", cfg.dataRoot + "/snapshots"),
        "SNAPSHOT_DIR");
    cfg.backupDir = canonicalBackup;
    cfg.restoreDir = requireAbsolute(
        getString("RESTORE_DIR", cfg.dataRoot + "/restores"),
        "RESTORE_DIR");
    cfg.tmpDir = requireAbsolute(
        getString("TMP_DIR", cfg.dataRoot + "/tmp"), "TMP_DIR");
    cfg.logDir = requireAbsolute(
        getString("LOG_DIR", cfg.dataRoot + "/logs"), "LOG_DIR");
    return cfg;
}

EnvConfig::NetworkConfig EnvConfig::getNetworkConfig() {
    NetworkConfig cfg;
    cfg.engineHost   = getString("ENGINE_HOST", "0.0.0.0");
    cfg.enginePort   = getInt("ENGINE_PORT", 9000);
    cfg.raftHost     = getString("RAFT_LISTEN_HOST", "0.0.0.0");
    cfg.raftPort     = getInt("RAFT_LISTEN_PORT", 9001);
    cfg.metricsPort  = getInt("METRICS_PORT", 9090);
    return cfg;
}

EnvConfig::RaftConfig EnvConfig::getRaftConfig() {
    RaftConfig cfg;
    cfg.nodeId                    = getString("RAFT_NODE_ID", "node-0");
    cfg.peers                     = getList("RAFT_PEERS", {});
    cfg.isLeader                  = getBool("RAFT_IS_LEADER", false);
    cfg.electionTimeoutMs         = getInt("RAFT_ELECTION_TIMEOUT_MS", 3000);
    cfg.heartbeatIntervalMs       = getInt("RAFT_HEARTBEAT_INTERVAL_MS", 1000);
    cfg.appendTimeoutMs           = getInt("RAFT_APPEND_TIMEOUT_MS", 5000);
    cfg.snapshotIntervalMs        = getInt("RAFT_SNAPSHOT_INTERVAL_MS", 300000);
    cfg.maxInflightAppend         = getInt("RAFT_MAX_INFLIGHT_APPEND", 256);
    cfg.minQuorumSize             = getInt("MIN_QUORUM_SIZE", 1);
    cfg.splitBrainFencerEnabled   = getBool("ENABLE_SPLIT_BRAIN_FENCER", true);
    cfg.visibilityBarrierEnabled  = getBool("ENABLE_VISIBILITY_BARRIER", true);
    return cfg;
}

EnvConfig::WalConfig EnvConfig::getWalConfig() {
    WalConfig cfg;
    cfg.enabled                = getBool("WAL_ENABLED", true);
    cfg.fsyncEnabled           = getBool("WAL_FSYNC_ENABLED", true);
    cfg.segmentSizeMb          = getInt("WAL_SEGMENT_SIZE_MB", 128);
    cfg.bufferSizeBytes        = getSize("WAL_BUFFER_SIZE_BYTES", 8 * 1024 * 1024);
    cfg.rotationIntervalMs     = getInt("WAL_ROTATION_INTERVAL_MS", 3600000);
    cfg.compressionEnabled     = getBool("WAL_COMPRESSION_ENABLED", true);
    cfg.checksumEnabled        = getBool("WAL_CHECKSUM_ENABLED", true);
    cfg.crcValidation          = getBool("WAL_CRC_VALIDATION", true);
    cfg.corruptionQuarantine   = getBool("WAL_CORRUPTION_QUARANTINE", true);
    cfg.groupCommitEnabled     = getBool("WAL_GROUP_COMMIT_ENABLED", true);
    cfg.groupCommitIntervalMs  = getInt("WAL_GROUP_COMMIT_INTERVAL_MS", 10);
    return cfg;
}

EnvConfig::SecurityConfig EnvConfig::getSecurityConfig() {
    auto requireAbsoluteIfSet = [](const std::string& configured,
                                   const char* key) {
        if (configured.empty()) return configured;
        const std::filesystem::path candidate(configured);
        if (!candidate.is_absolute()) {
            throw std::runtime_error(
                std::string(key) + " must be an explicit absolute path");
        }
        return candidate.lexically_normal().string();
    };

    SecurityConfig cfg;
    cfg.tlsEnabled            = getBool("TLS_ENABLED", false);
    cfg.tlsCertPath           = requireAbsoluteIfSet(
        getString("TLS_CERT_PATH", ""), "TLS_CERT_PATH");
    cfg.tlsKeyPath            = requireAbsoluteIfSet(
        getString("TLS_KEY_PATH", ""), "TLS_KEY_PATH");
    cfg.tlsCaPath             = requireAbsoluteIfSet(
        getString("TLS_CA_PATH", ""), "TLS_CA_PATH");
    if (cfg.tlsEnabled &&
        (cfg.tlsCertPath.empty() || cfg.tlsKeyPath.empty())) {
        throw std::runtime_error(
            "TLS_ENABLED requires absolute TLS_CERT_PATH and TLS_KEY_PATH");
    }
    cfg.rbacEnabled           = getBool("RBAC_ENABLED", true);
    cfg.jwtSecret             = getString("JWT_SECRET", "");
    cfg.apiKeysEnabled        = getBool("API_KEYS_ENABLED", true);
    cfg.auditLoggingEnabled   = getBool("AUDIT_LOGGING_ENABLED", true);
    cfg.encryptionEnabled     = getBool("ENCRYPTION_ENABLED", false);
    cfg.encryptionAlgorithm   = getString("ENCRYPTION_ALGORITHM", "AES-256-GCM");
    const std::string dataRoot = requireAbsoluteIfSet(
        getString("DATA_ROOT", ""), "DATA_ROOT");
    cfg.masterKeyPath         = requireAbsoluteIfSet(
        getString(
            "MASTER_KEY_PATH",
            dataRoot.empty() ? "" : dataRoot + "/security/master.key"),
        "MASTER_KEY_PATH");
    if (cfg.encryptionEnabled) {
        throw std::runtime_error(
            "ENCRYPTION_ENABLED is unavailable: native WAL, SST, snapshot, and "
            "backup writes are not encrypted; use independently verified "
            "encrypted storage and keep this flag false");
    }
    cfg.authRequired          = getBool("ENGINE_AUTH_REQUIRED", false);
    cfg.readOnlyMode          = getBool("READ_ONLY_MODE", false);
    return cfg;
}
