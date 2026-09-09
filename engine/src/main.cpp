#include "server.hpp"
#include "database_engine.hpp"
#include "recovery_manager.hpp"
#include "snapshot_manager.hpp"
#include "txid_allocator.hpp"
#include "garbage_collector.hpp"
#include "shard_manager.hpp"
#include "metrics.hpp"
#include "env_config.hpp"
#include <iostream>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include "lsm.hpp"
#include "raft_core.hpp"
#include "wal.hpp"
#include "query_limiter.hpp"
#include "memory_manager.hpp"
#include "backup_manager.hpp"
#include "community_catalog.hpp"
#include "security_manager.hpp"
#include "storage_path.hpp"
#include "storage_root_guard.hpp"
#include "test_failpoint.hpp"
#include "build_identity.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

// Extern accessors from server.cpp for live connection/RPS metrics
extern int64_t Server_getActiveConnections();
extern double Server_getCurrentRPS();

// Global MVCC system
static TXIDAllocator* g_txidAllocator = nullptr;
static SnapshotManager* g_snapshotManager = nullptr;
static GarbageCollector* g_garbageCollector = nullptr;
static volatile std::sig_atomic_t g_terminationSignal = 0;

static void handleTerminationSignal(int signalNumber) {
    g_terminationSignal = signalNumber;
    pacificdb::test::requestShutdown();
    requestServerShutdown();
#ifndef _WIN32
    // Before server lifecycle ownership begins there are no listener/worker
    // resources to unwind. Exit immediately; recovery treats this as an
    // interrupted startup and never writes a false clean-shutdown marker.
    if (!serverLifecycleStarted()) {
        _Exit(128 + signalNumber);
    }
#endif
}

// Cross-platform setenv
static void setEnvironmentVariable(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static bool productionFlag(const char* name, bool fallback = false) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    throw std::runtime_error(std::string(name) + " must be a boolean value");
}

static void validateProductionAtRestEvidence() {
    const std::string evidenceValue = EnvConfig::getString(
        "PACIFICDB_AT_REST_ENCRYPTION_EVIDENCE_PATH", "");
    if (evidenceValue.empty()) {
        throw std::runtime_error(
            "production requires PACIFICDB_AT_REST_ENCRYPTION_EVIDENCE_PATH");
    }

    const std::filesystem::path evidencePath(evidenceValue);
    if (!evidencePath.is_absolute() ||
        std::filesystem::is_symlink(evidencePath) ||
        !std::filesystem::is_regular_file(evidencePath)) {
        throw std::runtime_error(
            "at-rest encryption evidence must be an absolute, regular, non-symlink file");
    }
#ifndef _WIN32
    struct stat evidenceStat {};
    if (lstat(evidencePath.c_str(), &evidenceStat) != 0 ||
        evidenceStat.st_uid != 0 ||
        (evidenceStat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw std::runtime_error(
            "at-rest encryption evidence must be root-owned and not group/world writable");
    }
#endif

    nlohmann::json evidence;
    try {
        std::ifstream input(evidencePath);
        input >> evidence;
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("cannot parse at-rest encryption evidence: ") + error.what());
    }

    const std::string configuredRoot = std::filesystem::path(
        EnvConfig::getString("DATA_ROOT", "")).lexically_normal().string();
    const std::string observedRoot = std::filesystem::path(
        evidence.value("dataRoot", "")).lexically_normal().string();
    const std::string protection = evidence.value("protection", "");
    static const std::set<std::string> allowedProtection = {
        "dm-crypt-luks", "fscrypt", "encrypted-managed-volume"};
    if (!evidence.value("pass", false) || configuredRoot.empty() ||
        configuredRoot != observedRoot ||
        allowedProtection.find(protection) == allowedProtection.end() ||
        evidence.value("reviewedBy", "").empty() ||
        evidence.value("reviewedAt", "").empty()) {
        throw std::runtime_error(
            "at-rest encryption evidence is not a passing named review for this DATA_ROOT");
    }
}

static void validateProductionConfiguration() {
    std::string environment = EnvConfig::getString("PACIFICDB_ENVIRONMENT", "development");
    std::transform(environment.begin(), environment.end(), environment.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (environment != "production") return;

    const auto security = EnvConfig::getSecurityConfig();
    if (!security.tlsEnabled) {
        throw std::runtime_error("production requires TLS_ENABLED=true");
    }
    if (security.tlsCaPath.empty()) {
        throw std::runtime_error("production requires an explicit TLS_CA_PATH");
    }
    for (const auto& [name, value] : {
             std::pair<const char*, std::string>{"TLS_CERT_PATH", security.tlsCertPath},
             std::pair<const char*, std::string>{"TLS_KEY_PATH", security.tlsKeyPath},
             std::pair<const char*, std::string>{"TLS_CA_PATH", security.tlsCaPath}}) {
        if (value.empty() || !std::filesystem::path(value).is_absolute()) {
            throw std::runtime_error(std::string("production requires absolute ") + name);
        }
    }
    if (!productionFlag("TLS_REQUIRE_CLIENT_CERT", false)) {
        throw std::runtime_error(
            "production requires TLS_REQUIRE_CLIENT_CERT=true for client mTLS");
    }
    if (!security.authRequired) {
        throw std::runtime_error("production requires ENGINE_AUTH_REQUIRED=true");
    }
    if (!security.rbacEnabled || !security.auditLoggingEnabled) {
        throw std::runtime_error(
            "production requires RBAC_ENABLED=true and AUDIT_LOGGING_ENABLED=true");
    }
    validateProductionAtRestEvidence();

    const bool legacyRaftTls = productionFlag("RAFT_TLS_ENABLE", false);
    if (!productionFlag("RAFT_TLS_ENABLED", legacyRaftTls)) {
        throw std::runtime_error("production requires RAFT_TLS_ENABLED=true");
    }
    for (const char* pathKey : {
             "RAFT_TLS_CERT_PATH", "RAFT_TLS_KEY_PATH", "RAFT_TLS_CA_PATH"}) {
        const std::string value = EnvConfig::getString(pathKey, "");
        if (value.empty() || !std::filesystem::path(value).is_absolute()) {
            throw std::runtime_error(std::string("production requires absolute ") +
                                     pathKey);
        }
    }

    if (!productionFlag("WAL_FSYNC_ENABLED", true) ||
        !productionFlag("WAL_FLUSH_ON_COMMIT", true)) {
        throw std::runtime_error(
            "production requires WAL_FSYNC_ENABLED=true and WAL_FLUSH_ON_COMMIT=true");
    }
    if (EnvConfig::getString("RAFT_REPLICATION_MODE", "") != "sync") {
        throw std::runtime_error("production requires RAFT_REPLICATION_MODE=sync");
    }
    if (EnvConfig::getInt("MIN_QUORUM_SIZE", 1) < 2) {
        throw std::runtime_error("production RF3 requires MIN_QUORUM_SIZE>=2");
    }

    for (const char* bindKey : {"ENGINE_BIND_HOST", "RAFT_BIND_HOST"}) {
        const std::string value = EnvConfig::getString(bindKey, "");
        if (value.empty() || value == "0.0.0.0" || value == "*") {
            throw std::runtime_error(std::string("production requires an explicit non-wildcard ") +
                                     bindKey);
        }
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "PacificDB Engine " << PACIFICDB_ENGINE_VERSION << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--build-info") {
        std::cout
            << "{\"product\":\"PacificDB Engine\","
            << "\"engineVersion\":\"" << PACIFICDB_ENGINE_VERSION << "\","
            << "\"gitCommit\":\"" << PACIFICDB_GIT_COMMIT << "\","
            << "\"storageFormatVersion\":\"" << PACIFICDB_STORAGE_FORMAT_VERSION << "\","
            << "\"raftProtocolVersion\":\"" << PACIFICDB_RAFT_PROTOCOL_VERSION << "\","
            << "\"buildTimestamp\":\"" << PACIFICDB_BUILD_TIMESTAMP << "\","
            << "\"compiler\":\"" << PACIFICDB_COMPILER << "\"}\n";
        return 0;
    }
    std::cout << "[MAIN] Starting PacificDB Engine...\n";

    // v5.5P-R2C: ignore SIGPIPE process-wide. The engine writes to many client and Raft
    // peer sockets; when a peer closes mid-write the default SIGPIPE disposition
    // TERMINATES the process. Under R2C's higher write throughput (the commit-order
    // cascade fix removed the old write throttle) plus keepalive connection churn this
    // fired reliably ~30s into a 100-client load and silently killed the leader. Ignoring
    // SIGPIPE makes the offending write() simply fail with EPIPE, which the socket code
    // already handles. (Belt-and-suspenders to MSG_NOSIGNAL on individual sends.)
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::signal(SIGTERM, handleTerminationSignal);
    std::signal(SIGINT, handleTerminationSignal);

    // ============================================
    // PHASE 0: Load Environment Configuration
    // ============================================
    EnvConfig::load();
    if (EnvConfig::getBool("ENGINE_VERBOSE", false)) {
        EnvConfig::dump();
    }
    try {
        validateProductionConfiguration();
    } catch (const std::exception& error) {
        std::cerr << "[MAIN] PRODUCTION_CONFIGURATION_REFUSED: "
                  << error.what() << "\n";
        return 78;
    }

    EnvConfig::StorageConfig storageCfg;
    std::string dataRoot;
    std::string nodeIdStr;
    std::string clusterId;
    StorageRootGuard storageRootGuard;
    try {
        storageCfg = EnvConfig::getStorageConfig();
        nodeIdStr = EnvConfig::getString("RAFT_NODE_ID", "");
        if (nodeIdStr.empty()) {
            nodeIdStr = EnvConfig::getString("NODE_ID", "");
        }
        clusterId = EnvConfig::getString("RAFT_CLUSTER_ID", "");
        if (nodeIdStr.empty()) {
            throw std::runtime_error("RAFT_NODE_ID or NODE_ID is required");
        }
        if (clusterId.empty()) {
            throw std::runtime_error("RAFT_CLUSTER_ID is required");
        }
        storageRootGuard = StorageRootGuard::acquire(
            storageCfg.dataRoot,
            clusterId,
            nodeIdStr,
            PACIFICDB_STORAGE_FORMAT_VERSION);
        dataRoot = storageRootGuard.canonicalRoot().string();
        storageCfg.dataRoot = dataRoot;
        EnvConfig::set("DATA_ROOT", dataRoot);
        EnvConfig::set("DATA_DIR", dataRoot);
        setEnvironmentVariable("DATA_ROOT", dataRoot.c_str());
        setEnvironmentVariable("DATA_DIR", dataRoot.c_str());
    } catch (const std::exception& error) {
        std::cerr << "[MAIN] STORAGE_CONFIGURATION_REFUSED: "
                  << error.what() << "\n";
        return 78;
    }
    const char* nodeIdEnv = nodeIdStr.c_str();

    std::cout << "[MAIN] Data root: " << dataRoot << "\n";
    std::cout << "[MAIN] Root identity: "
              << storageRootGuard.identityPath().string() << "\n";
    std::cout << "[MAIN] Root lock: " << storageRootGuard.lockPath().string() << "\n";
    std::cout << "[MAIN] WAL dir:   " << storageCfg.walDir << "\n";
    std::cout << "[MAIN] SST dir:   " << storageCfg.sstDir << "\n";
    std::cout << "[MAIN] Backups:   " << storageCfg.backupDir << "\n";

    // ============================================
    // PHASE 0: Crash Recovery
    // ============================================
    std::cout << "\n[MAIN] ========== CRASH RECOVERY PHASE ==========\n";
    pacificdb::test::hitFailpoint("FP_SHUTDOWN_BEFORE_LISTENER_BIND", 1);
    const char* raftLogPath = std::getenv("RAFT_LOG_PATH");
    std::filesystem::path raftLogFile =
        raftLogPath && *raftLogPath
        ? std::filesystem::path(raftLogPath)
        : std::filesystem::path(dataRoot) / "raft.log";
    try {
        if (!raftLogFile.is_absolute()) {
            throw std::runtime_error("RAFT_LOG_PATH must be absolute");
        }
        raftLogFile = validateContainedStoragePath(dataRoot, raftLogFile);
    } catch (const std::exception& error) {
        std::cerr << "[MAIN] STORAGE_CONFIGURATION_REFUSED: "
                  << error.what() << "\n";
        return 78;
    }

    // Recovery replays committed entries through DatabaseEngine and LSM. Their
    // canonical roots must be configured before replay, but logical system
    // catalog creation remains after recovery so startup does not manufacture
    // state ahead of persisted history.
    try {
        DatabaseEngine::configureStorageRoot(dataRoot);
        LSM::init(dataRoot);
    } catch (const std::exception& error) {
        std::cerr << "[MAIN] STORAGE_CONFIGURATION_REFUSED: cannot configure "
                     "recovery storage: "
                  << error.what() << "\n";
        return 78;
    }

    RecoveryManager::RecoveryState recoveryState;
    try {
        recoveryState =
            RecoveryManager::executeRecovery(dataRoot, raftLogFile.string());
    } catch (const std::exception& error) {
        // Recovery walks every persisted artifact before the listener starts.
        // A symlink or special-file escape is a storage-configuration refusal,
        // not a reason to terminate through an uncaught exception or continue
        // with a partially inspected root.
        std::cerr << "[MAIN] STORAGE_CONFIGURATION_REFUSED: unsafe recovery "
                     "path: "
                  << error.what() << "\n";
        return 78;
    } catch (...) {
        std::cerr << "[MAIN] STORAGE_CONFIGURATION_REFUSED: unsafe recovery "
                     "path: unknown validation failure\n";
        return 78;
    }
    if (!recoveryState.success) {
        std::cerr << "\n[MAIN] ✗ Recovery failed: " << recoveryState.errorMsg << "\n";
        std::cerr << "[MAIN] Node entering READ-ONLY mode for safety\n";
        // Set env var to make server read-only
        setEnvironmentVariable("READ_ONLY_MODE", "true");
    } else {
        std::cout << "\n[MAIN] ✓ Recovery successful, node ready to serve\n";
    }

    DatabaseEngine::init(dataRoot, /*restoreWal=*/!recoveryState.wasCleanShutdown);
    pacificdb::community::CommunityCatalog::instance().initialize("system");

    // Initialize Query Limiter with environment configuration
    QueryLimiter::init();

    // Initialize Memory Manager for backpressure control
    MemoryManager::init();
    MemoryManager::start();

    // Initialize engine-level security (optional enforcement via ENGINE_AUTH_REQUIRED=1)
    pacificdb::security::SecurityManager::instance().initialize(dataRoot);

    // Provision least-privilege service accounts from an operator-owned file.
    // This keeps credentials out of unit files, process arguments and logs.
    if (const char* acctPath = std::getenv("PACIFICDB_ENGINE_ACCOUNTS_FILE")) {
        try {
            std::ifstream acctFile(acctPath);
            if (!acctFile) {
                throw std::runtime_error("accounts file is unreadable");
            }
            const auto accountPermissions =
                std::filesystem::status(acctPath).permissions();
            using Perms = std::filesystem::perms;
            if ((accountPermissions &
                 (Perms::group_read | Perms::group_write |
                  Perms::group_exec | Perms::others_read |
                  Perms::others_write | Perms::others_exec)) != Perms::none) {
                throw std::runtime_error(
                    "accounts file must not be accessible by group or others");
            }
            nlohmann::json accounts;
            acctFile >> accounts;
            if (!accounts.is_array()) {
                throw std::runtime_error("accounts file must contain a JSON array");
            }
            auto& security =
                pacificdb::security::SecurityManager::instance();
            int provisioned = 0;
            for (const auto& account : accounts) {
                const std::string username =
                    account.value("username", std::string());
                const std::string password =
                    account.value("password", std::string());
                const std::string roleName =
                    account.value("role", std::string("READ_ONLY"));
                if (username.empty() || password.empty()) continue;

                using pacificdb::security::Role;
                Role role = Role::READ_ONLY;
                if (roleName == "SUPERADMIN") role = Role::SUPERADMIN;
                else if (roleName == "ADMIN") role = Role::ADMIN;
                else if (roleName == "WRITE") role = Role::WRITE;
                else if (roleName == "BACKUP_OPERATOR") {
                    role = Role::BACKUP_OPERATOR;
                } else if (roleName == "METRICS_VIEWER") {
                    role = Role::METRICS_VIEWER;
                }

                if (!security.createUser(username, password, role,
                                         "startup-provisioner")) {
                    continue;
                }
                std::vector<std::string> databases;
                if (account.contains("databases") &&
                    account["databases"].is_array()) {
                    for (const auto& database : account["databases"]) {
                        if (database.is_string()) {
                            databases.push_back(database.get<std::string>());
                        }
                    }
                }
                if (!databases.empty()) {
                    security.setUserDatabaseAccess(
                        username, databases, "startup-provisioner");
                }
                ++provisioned;
                std::cout << "[ENGINE] provisioned engine account "
                          << username << " role=" << roleName
                          << " databases=" << databases.size() << std::endl;
            }
            std::cout << "[ENGINE] engine account provisioning complete: "
                      << provisioned << " created" << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "[ENGINE] account provisioning failed: "
                      << error.what() << std::endl;
            return 1;
        }
    }

    // Manual snapshot backup and restore. Operators initiate every operation.
    BackupManager::instance().init(
        dataRoot, storageCfg.backupDir, storageCfg.restoreDir);

    // ============================================
    // PHASE 1: Initialize MVCC System
    // ============================================
    std::cout << "\n[MAIN] ========== MVCC INITIALIZATION ==========\n";

    g_txidAllocator = new TXIDAllocator();
    g_txidAllocator->setNextTxid(recoveryState.appliedLSN);
    std::cout << "[MAIN] ✓ TXIDAllocator initialized, next TXID: " << (recoveryState.appliedLSN + 1) << "\n";

    g_snapshotManager = new SnapshotManager();
    g_snapshotManager->setTXIDAllocator(g_txidAllocator);
    std::cout << "[MAIN] ✓ SnapshotManager initialized\n";

    g_garbageCollector = new GarbageCollector(g_snapshotManager);
    g_garbageCollector->start();
    std::cout << "[MAIN] ✓ GarbageCollector started\n";

    // initialize LSM layer with same data root
    LSM::init(dataRoot);
    LSM::startBackgroundTasks();

    // initialize WAL background flusher if configured
    WAL::init();

    // ============================================
    // PHASE 2: Initialize Dynamic Shard Manager
    // ============================================
    std::cout << "\n[MAIN] ========== SHARD MANAGER INITIALIZATION ==========\n";
    ShardManager::instance().init("");  // Empty = auto-configure
    ShardManager::instance().start();
    std::cout << "[MAIN] ✓ ShardManager started with dynamic shard allocation\n";

    // Register only the explicitly configured local node. Cluster membership and
    // shard placement are changed through manual administrative operations.
    const std::string nodeId = nodeIdStr;
    ClusterNode thisNode;
    thisNode.nodeId = nodeId;
    thisNode.host = EnvConfig::getString("ENGINE_HOST", "127.0.0.1");
    thisNode.port = EnvConfig::getInt("ENGINE_PORT", 9000);
    thisNode.lastHeartbeat = std::chrono::steady_clock::now();
    ShardManager::instance().registerNode(thisNode);

    // Initialize RaftCore if RAFT_PEERS provided
    const char* peersEnv = std::getenv("RAFT_PEERS");
    const char* raftPortEnv = std::getenv("RAFT_LISTEN_PORT");
    if (peersEnv) {
        std::string s(peersEnv);
        std::vector<std::string> peers;
        size_t start = 0;
        while (true) {
            auto pos = s.find(',', start);
            if (pos == std::string::npos) {
                std::string p = s.substr(start);
                // Skip empty peers for single-node config
                if (!p.empty()) peers.push_back(p);
                break;
            }
            std::string p = s.substr(start, pos-start);
            if (!p.empty()) peers.push_back(p);
            start = pos + 1;
        }
        if (peers.size() > 1) {
            std::set<std::string> peerNodeIds;
            for (const auto& peer : peers) {
                std::string nodePart;
                std::string endpoint = peer;
                auto at = peer.find('@');
                if (at != std::string::npos) {
                    nodePart = peer.substr(0, at);
                    endpoint = peer.substr(at + 1);
                    if (nodePart.empty() || !peerNodeIds.insert(nodePart).second) {
                        std::cerr << "[MAIN] FATAL: Duplicate or empty NODE_ID in RAFT_PEERS: " << peer << "\n";
                        return 78;
                    }
                }

                auto colon = endpoint.rfind(':');
                std::string host = colon == std::string::npos ? endpoint : endpoint.substr(0, colon);
                const bool peerIsSelf = (!nodePart.empty() && nodePart == nodeIdStr);
                const bool localhost = host == "127.0.0.1" || host == "localhost" || host == "::1";
                if (localhost && !peerIsSelf) {
                    std::cerr << "[MAIN] FATAL: Remote RAFT_PEERS entry advertises localhost: " << peer << "\n";
                    return 78;
                }
            }
        }
        int port = raftPortEnv ? std::stoi(raftPortEnv) : 9001;
        // Check RAFT_IS_LEADER env var for single-node / test deployments
        // For production clusters with multiple peers, always start as follower.
        bool startAsLeader = false;
        const char* isLeaderEnv = std::getenv("RAFT_IS_LEADER");
        if (peers.empty() || (isLeaderEnv && (std::string(isLeaderEnv) == "true" || std::string(isLeaderEnv) == "1"))) {
            startAsLeader = true;
            std::cout << "[MAIN] Single-node or RAFT_IS_LEADER=true, starting as LEADER\n";
        }
        RaftCore::instance().setCleanShutdownRecovery(
            recoveryState.wasCleanShutdown);
        RaftCore::instance().init(peers, nodeId, port, startAsLeader);
        if (recoveryState.wasCleanShutdown) {
            try {
                WALIntegrity::clearCleanShutdownMarker(dataRoot);
            } catch (const std::exception& error) {
                std::cerr << "[MAIN] STARTUP_REFUSED: " << error.what() << "\n";
                return 78;
            }
        }
        RaftCore::instance().start();
        bool standaloneBypass = startAsLeader && peers.empty() && EnvConfig::getBool("RAFT_STANDALONE_BYPASS", true);
        std::cout << "[MAIN] standalone_raft_bypass=" << (standaloneBypass ? "true" : "false") << "\n";
    } else {
        // No RAFT_PEERS = standalone single-node mode, always leader
        int port = raftPortEnv ? std::stoi(raftPortEnv) : 9001;
        std::cout << "[MAIN] No RAFT_PEERS configured, running in standalone LEADER mode\n";
        RaftCore::instance().setCleanShutdownRecovery(
            recoveryState.wasCleanShutdown);
        RaftCore::instance().init({}, nodeId, port, true);
        if (recoveryState.wasCleanShutdown) {
            try {
                WALIntegrity::clearCleanShutdownMarker(dataRoot);
            } catch (const std::exception& error) {
                std::cerr << "[MAIN] STARTUP_REFUSED: " << error.what() << "\n";
                return 78;
            }
        }
        RaftCore::instance().start();
        std::cout << "[MAIN] standalone_raft_bypass="
                  << (EnvConfig::getBool("RAFT_STANDALONE_BYPASS", true) ? "true" : "false")
                  << "\n";
    }

    std::cout << "\n[MAIN] ========== SERVER STARTUP ==========\n";
    startServer();  // socket server loop

    // ============================================
    // CLEANUP ON EXIT
    // ============================================
    std::cout << "\n[MAIN] ========== SHUTDOWN PHASE ==========\n";
    const auto shutdownStartedAt = std::chrono::steady_clock::now();
    const auto logShutdownElapsed = [&shutdownStartedAt](const char* stage) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shutdownStartedAt).count();
        std::cerr << "[MAIN][SHUTDOWN] " << stage << " elapsed_ms=" << elapsed << std::endl;
    };

    RaftCore::instance().stop();
    std::cout << "[MAIN] ✓ Raft core stopped" << std::endl;
    logShutdownElapsed("raft_stopped");

    // Stop shard manager
    ShardManager::instance().stop();
    std::cout << "[MAIN] ✓ ShardManager stopped" << std::endl;
    logShutdownElapsed("shard_manager_stopped");

    // Stop garbage collector
    if (g_garbageCollector) {
        g_garbageCollector->stop();
        delete g_garbageCollector;
        g_garbageCollector = nullptr;
    }
    logShutdownElapsed("garbage_collector_stopped");

    // The clean marker is a durable promise that collection state no longer
    // exists only in memory. Legacy shutdown wrote that marker without draining
    // active memtables, which made a fast restart silently lose the WAL-only
    // suffix. No marker is written if the drain fails.
    bool cleanFlushComplete = false;
    try {
        LSM::forceFlush();
        cleanFlushComplete = true;
        std::cout << "[MAIN] ✓ Active LSM memtables force-flushed" << std::endl;
        logShutdownElapsed("lsm_force_flushed");
    } catch (const std::exception& error) {
        std::cerr << "[MAIN] ✗ LSM force flush failed; clean marker withheld: "
                  << error.what() << std::endl;
    } catch (...) {
        std::cerr << "[MAIN] ✗ LSM force flush failed; clean marker withheld: unknown error"
                  << std::endl;
    }

    LSM::stopBackgroundTasks();
    std::cout << "[MAIN] ✓ LSM background tasks stopped" << std::endl;
    logShutdownElapsed("lsm_stopped");

    // Flush and stop WAL background flusher
    WAL::shutdown();
    std::cout << "[MAIN] ✓ WAL flusher stopped" << std::endl;
    logShutdownElapsed("wal_stopped");

    // Mark clean shutdown only after the force-flush contract is proven.
    if (cleanFlushComplete) {
        try {
            RecoveryManager::markCleanShutdown(dataRoot);
            std::cout << "[MAIN] ✓ Clean shutdown marker v2 written" << std::endl;
            logShutdownElapsed("clean_marker_written");
        } catch (const std::exception& error) {
            std::cerr << "[MAIN] Clean shutdown marker withheld: "
                      << error.what() << std::endl;
        }
    } else {
        std::cerr << "[MAIN] Clean shutdown marker withheld; next start will replay"
                  << std::endl;
    }

    MemoryManager::stop();
    logShutdownElapsed("memory_manager_stopped");
#ifndef _WIN32
    if (g_terminationSignal) {
        // All owned services, queues, storage workers and durability markers
        // have been stopped above. Do not enter unordered process-lifetime
        // static destruction while other legacy singletons may still exist.
        // Their OS resources are process-scoped and no durable work remains.
        const std::string message = "[MAIN] Shutdown complete after signal "
            + std::to_string(g_terminationSignal) + "\n";
        const ssize_t shutdownLogBytes =
            ::write(STDERR_FILENO, message.data(), message.size());
        (void)shutdownLogBytes;
        _Exit(EXIT_SUCCESS);
    }
#endif

    std::cout << "[MAIN] Shutdown complete" << std::endl;

    return 0;
}
