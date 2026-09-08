#pragma once

#include <string>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <vector>

/**
 * @brief Comprehensive environment-driven configuration system for PacificDB.
 *
 * This class provides a single source of truth for all configurable parameters.
 * It supports:
 *   - Loading from .env files (auto-detected at startup)
 *   - Loading from process environment variables
 *   - Type-safe accessors (string, int, bool, double, size_t)
 *   - Default values for every setting
 *   - Runtime reload without restart
 *   - JSON dump for inspection
 *
 * Precedence (highest to lowest):
 *   1. Process environment variables (set via shell, systemd, k8s)
 *   2. .env file in working directory
 *   3. Explicit absolute PACIFICDB_ENV_FILE and DATA_ROOT/.env only
 *   4. Compiled-in defaults
 *
 * Usage:
 *   EnvConfig::load();                               // auto-detects .env
 *   std::string dataRoot = EnvConfig::getString("DATA_ROOT", "");
 *   int port = EnvConfig::getInt("ENGINE_PORT", 9000);
 *   bool tls = EnvConfig::getBool("TLS_ENABLED", false);
 */
class EnvConfig {
public:
    // ==================== INITIALIZATION ====================

    /**
     * Load configuration from .env files and environment variables.
     * Loads only explicit absolute PACIFICDB_ENV_FILE and $DATA_ROOT/.env.
     */
    static void load();

    /**
     * Load from a specific .env file path.
     */
    static void loadFromFile(const std::string& filepath);

    /**
     * Reload configuration (useful for SIGHUP handlers).
     */
    static void reload();

    // ==================== TYPED ACCESSORS ====================

    /**
     * Get a string value with default fallback.
     */
    static std::string getString(const std::string& key, const std::string& defaultValue = "");

    /**
     * Get an integer value with default fallback.
     */
    static int getInt(const std::string& key, int defaultValue = 0);

    /**
     * Get a 64-bit signed integer.
     */
    static int64_t getInt64(const std::string& key, int64_t defaultValue = 0);

    /**
     * Get an unsigned size value (for buffer sizes, limits).
     */
    static size_t getSize(const std::string& key, size_t defaultValue = 0);

    /**
     * Get a boolean value (accepts: true/false, 1/0, yes/no, on/off).
     */
    static bool getBool(const std::string& key, bool defaultValue = false);

    /**
     * Get a double value with default fallback.
     */
    static double getDouble(const std::string& key, double defaultValue = 0.0);

    /**
     * Get a comma-separated list as a vector of strings.
     */
    static std::vector<std::string> getList(const std::string& key, const std::vector<std::string>& defaultValue = {});

    // ==================== KEY MANAGEMENT ====================

    /**
     * Check if a key is defined (in env or .env file).
     */
    static bool has(const std::string& key);

    /**
     * Set a key/value at runtime (overrides loaded value).
     */
    static void set(const std::string& key, const std::string& value);

    /**
     * Get all loaded keys/values (useful for /config endpoint).
     */
    static std::unordered_map<std::string, std::string> getAll();

    /**
     * Print all configuration to stdout (for debugging).
     * Sensitive keys (containing SECRET, KEY, PASSWORD) are masked.
     */
    static void dump();

    /**
     * Export configuration as JSON string.
     */
    static std::string toJson();

    // ==================== STRUCTURED ACCESS ====================

    /**
     * Get all storage path settings (DATA_ROOT, WAL_DIR, etc.).
     */
    struct StorageConfig {
        std::string dataRoot;
        std::string walDir;
        std::string sstDir;
        std::string snapshotDir;
        std::string backupDir;
        std::string restoreDir;
        std::string tmpDir;
        std::string logDir;
    };
    static StorageConfig getStorageConfig();

    /**
     * Get all networking settings.
     */
    struct NetworkConfig {
        std::string engineHost;
        int enginePort;
        std::string raftHost;
        int raftPort;
        int metricsPort;
    };
    static NetworkConfig getNetworkConfig();

    /**
     * Get all Raft consensus settings.
     */
    struct RaftConfig {
        std::string nodeId;
        std::vector<std::string> peers;
        bool isLeader;
        int electionTimeoutMs;
        int heartbeatIntervalMs;
        int appendTimeoutMs;
        int snapshotIntervalMs;
        int maxInflightAppend;
        int minQuorumSize;
        bool splitBrainFencerEnabled;
        bool visibilityBarrierEnabled;
    };
    static RaftConfig getRaftConfig();

    /**
     * Get all WAL settings.
     */
    struct WalConfig {
        bool enabled;
        bool fsyncEnabled;
        int segmentSizeMb;
        size_t bufferSizeBytes;
        int rotationIntervalMs;
        bool compressionEnabled;
        bool checksumEnabled;
        bool crcValidation;
        bool corruptionQuarantine;
        bool groupCommitEnabled;
        int groupCommitIntervalMs;
    };
    static WalConfig getWalConfig();

    /**
     * Get all security settings.
     */
    struct SecurityConfig {
        bool tlsEnabled;
        std::string tlsCertPath;
        std::string tlsKeyPath;
        std::string tlsCaPath;
        bool rbacEnabled;
        std::string jwtSecret;
        bool apiKeysEnabled;
        bool auditLoggingEnabled;
        bool encryptionEnabled;
        std::string encryptionAlgorithm;
        std::string masterKeyPath;
        bool authRequired;
        bool readOnlyMode;
    };
    static SecurityConfig getSecurityConfig();

private:
    static std::unordered_map<std::string, std::string> values_;
    static std::mutex mutex_;
    static bool loaded_;

    static void parseEnvFile(const std::string& filepath);
    static void loadProcessEnvironment();
    static std::string trim(const std::string& s);
    static std::string maskSensitive(const std::string& key, const std::string& value);
    static bool isSensitive(const std::string& key);
};
