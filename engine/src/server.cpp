#include "server.hpp"
#include <array>
#include "database_engine.hpp"
#include "lsm.hpp"
#include "raft_core.hpp"
#include "connection_pool.hpp"
#include "query_limiter.hpp"
#include "db_task_queue_partitioned.hpp"
#include "metrics_exporter.hpp"
#include "id_generator.hpp"
#include "metrics.hpp"
#include "memory_manager.hpp"
#include "shard_manager.hpp"
#include "tenant_manager.hpp"
#include "backup_manager.hpp"
#include "security_manager.hpp"
#include "btree_index.hpp"
#include "wal.hpp"
#include "read_cache.hpp"
#include "request_timing.hpp"
#include "env_config.hpp"
#include "data_durability.hpp"
#include "test_failpoint.hpp"
#include "storage_path.hpp"
#include "tls_transport.hpp"

#include <openssl/evp.h>

// Cross-platform socket includes
#ifdef _WIN32
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <psapi.h>
    #pragma comment(lib, "Ws2_32.lib")
    typedef SOCKET socket_t;
    #define CLOSE_SOCKET closesocket
    #define SOCKET_ERROR_CODE WSAGetLastError()
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #ifdef __linux__
        #include <sys/epoll.h>
    #endif
    #include <sys/sysinfo.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>  // For TCP_NODELAY
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/resource.h>
    typedef int socket_t;
    typedef int SOCKET;
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR (-1)
    #define CLOSE_SOCKET close
    #define SOCKET_ERROR_CODE errno
    #define DWORD unsigned int
#endif

#include <thread>
#include <iostream>
#include <fstream>
#include <ctime>
#include <chrono>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <future>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <optional>
#include <shared_mutex>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <set>
#include <optional>
#include <cmath>
#include <string_view>

#include <mutex>

using json = nlohmann::json;

namespace {

constexpr char kEngineWireV2Magic[] = {'P', 'D', 'B', '2'};

constexpr std::uint64_t actionHash(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class HashedAction final : public std::string {
public:
    explicit HashedAction(std::string value)
        : std::string(std::move(value)), hash_(actionHash(*this)) {}

    template <std::size_t Size>
    bool operator==(const char (&literal)[Size]) const {
        return hash_ == actionHash(std::string_view(literal, Size - 1)) &&
               compare(0, size(), literal, Size - 1) == 0;
    }

private:
    std::uint64_t hash_;
};

volatile std::sig_atomic_t g_serverShutdownRequested = 0;
volatile std::sig_atomic_t g_serverLifecycleStarted = 0;
volatile std::sig_atomic_t g_serverReady = 0;
volatile std::sig_atomic_t g_serverListener = INVALID_SOCKET;
std::mutex g_clientSocketMutex;
std::set<SOCKET> g_clientSockets;
std::mutex g_clientTlsMutex;
std::unordered_map<SOCKET,
                   std::shared_ptr<pacificdb::transport::TlsSession>>
    g_clientTlsSessions;
std::shared_ptr<pacificdb::transport::TlsContext> g_clientTlsContext;

bool environmentFlag(const char* name, bool fallback = false) {
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

void initializeClientTls() {
    const auto security = EnvConfig::getSecurityConfig();
    std::lock_guard<std::mutex> lock(g_clientTlsMutex);
    g_clientTlsSessions.clear();
    g_clientTlsContext.reset();
    if (!security.tlsEnabled) return;

    pacificdb::transport::TlsConfig config;
    config.certificatePath = security.tlsCertPath;
    config.privateKeyPath = security.tlsKeyPath;
    config.caPath = security.tlsCaPath;
    config.requirePeerCertificate =
        environmentFlag("TLS_REQUIRE_CLIENT_CERT", false);
    config.verifyPeer = config.requirePeerCertificate;
    g_clientTlsContext = pacificdb::transport::TlsContext::create(
        pacificdb::transport::TlsRole::Server, config);
}

bool clientTlsEnabled() {
    std::lock_guard<std::mutex> lock(g_clientTlsMutex);
    return static_cast<bool>(g_clientTlsContext);
}

bool attachClientTls(SOCKET socket) {
    std::shared_ptr<pacificdb::transport::TlsContext> context;
    {
        std::lock_guard<std::mutex> lock(g_clientTlsMutex);
        context = g_clientTlsContext;
    }
    if (!context) return true;

    int timeoutMs = 5000;
    if (const char* raw = std::getenv("TLS_HANDSHAKE_TIMEOUT_MS")) {
        try { timeoutMs = std::max(250, std::stoi(raw)); } catch (...) {}
    }
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    try {
        auto session = std::make_shared<pacificdb::transport::TlsSession>(
            context, static_cast<int>(socket));
        if (!session->handshake()) {
            std::cerr << "[SERVER] TLS handshake refused: "
                      << session->lastError() << std::endl;
            return false;
        }
        std::lock_guard<std::mutex> lock(g_clientTlsMutex);
        g_clientTlsSessions[socket] = std::move(session);
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[SERVER] TLS handshake refused: " << error.what()
                  << std::endl;
        return false;
    }
}

std::shared_ptr<pacificdb::transport::TlsSession> clientTlsSession(
    SOCKET socket) {
    std::lock_guard<std::mutex> lock(g_clientTlsMutex);
    const auto found = g_clientTlsSessions.find(socket);
    return found == g_clientTlsSessions.end() ? nullptr : found->second;
}

void detachClientTls(SOCKET socket) {
    std::shared_ptr<pacificdb::transport::TlsSession> session;
    {
        std::lock_guard<std::mutex> lock(g_clientTlsMutex);
        const auto found = g_clientTlsSessions.find(socket);
        if (found != g_clientTlsSessions.end()) {
            session = std::move(found->second);
            g_clientTlsSessions.erase(found);
        }
    }
    if (session) session->shutdown();
}

int clientTransportSend(SOCKET socket, const void* buffer, size_t length,
                        int flags) {
    if (auto session = clientTlsSession(socket)) {
        return session->write(buffer, length);
    }
    // Once TLS is enabled, never emit a plaintext rejection before the worker
    // has completed the handshake and registered the session.
    if (clientTlsEnabled()) {
        errno = EPROTO;
        return -1;
    }
    return send(socket, static_cast<const char*>(buffer),
                static_cast<int>(length), flags);
}

int clientTransportRecv(SOCKET socket, void* buffer, size_t length,
                        int flags) {
    if (auto session = clientTlsSession(socket)) {
        return session->read(buffer, length);
    }
    if (clientTlsEnabled()) {
        errno = EPROTO;
        return -1;
    }
    return recv(socket, static_cast<char*>(buffer), static_cast<int>(length),
                flags);
}

// A syntactically safe identifier can still resolve through a symlink that was
// placed inside DATA_ROOT. Schema operations are replicated, so this check must
// run before Raft submission: rejecting only in the apply worker would leave a
// permanently unappliable committed entry at the head of the state machine.
void validateDatabaseStorageBeforeReplication(const std::string& userId,
                                               const std::string& dbName) {
    validateStorageIdentifier(userId, "userId");
    validateStorageIdentifier(dbName, "databaseName");

    const std::filesystem::path root(DatabaseEngine::getDataRoot());
    const std::filesystem::path database = root / userId / dbName;
    validateContainedStoragePath(root, root / userId);
    validateContainedStoragePath(root, database);
    for (const char* child : {"db.meta", "data", "wal", "logs", "media"}) {
        validateContainedStoragePath(root, database / child);
    }
}

void validateCollectionStorageBeforeReplication(const std::string& userId,
                                                 const std::string& dbName,
                                                 const std::string& collection) {
    validateDatabaseStorageBeforeReplication(userId, dbName);
    validateStorageIdentifier(collection, "collectionName");

    const std::filesystem::path root(DatabaseEngine::getDataRoot());
    const std::filesystem::path database = root / userId / dbName;
    for (const auto& artifact : {
             database / "data" / (collection + ".bin"),
             database / "wal" / (collection + ".wal"),
             database / (collection + ".lsm"),
             database / (collection + ".idx"),
         }) {
        validateContainedStoragePath(root, artifact);
    }
}

void trackClientSocket(SOCKET socket) {
    if (socket == INVALID_SOCKET) return;
    std::lock_guard<std::mutex> lock(g_clientSocketMutex);
    g_clientSockets.insert(socket);
}

void untrackClientSocket(SOCKET socket) {
    std::lock_guard<std::mutex> lock(g_clientSocketMutex);
    g_clientSockets.erase(socket);
}

void interruptTrackedClientSockets() {
    std::lock_guard<std::mutex> lock(g_clientSocketMutex);
    for (SOCKET socket : g_clientSockets) {
#ifdef _WIN32
        shutdown(socket, SD_BOTH);
#else
        shutdown(socket, SHUT_RDWR);
#endif
    }
}

std::optional<size_t> countAllRegisteredDocuments() {
    const std::filesystem::path root(DatabaseEngine::getDataRoot());
    if (!std::filesystem::is_directory(root)) return std::nullopt;

    size_t total = 0;
    size_t databases = 0;
    try {
        for (const auto& userEntry : std::filesystem::directory_iterator(root)) {
            if (!userEntry.is_directory()) continue;
            const std::string userId = userEntry.path().filename().string();
            for (const auto& databaseEntry :
                 std::filesystem::directory_iterator(userEntry.path())) {
                if (!databaseEntry.is_directory()) continue;
                const auto metadataPath = databaseEntry.path() / "db.meta";
                if (!std::filesystem::is_regular_file(metadataPath)) continue;

                std::ifstream metadataFile(metadataPath);
                json metadata;
                metadataFile >> metadata;
                if (!metadata.contains("collections") ||
                    !metadata["collections"].is_array()) {
                    return std::nullopt;
                }

                const std::string dbName =
                    databaseEntry.path().filename().string();
                ++databases;
                for (const auto& collection : metadata["collections"]) {
                    if (!collection.is_object() ||
                        !collection.contains("name") ||
                        !collection["name"].is_string()) {
                        return std::nullopt;
                    }
                    total += DatabaseEngine::count(
                        userId,
                        dbName,
                        collection["name"].get<std::string>(),
                        json::object());
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "[BackupManager] Exact document inventory failed: "
                  << error.what() << std::endl;
        return std::nullopt;
    }

    return databases == 0 ? std::nullopt : std::optional<size_t>(total);
}

}  // namespace

void requestServerShutdown() noexcept {
    g_serverShutdownRequested = 1;
    const SOCKET listener = static_cast<SOCKET>(g_serverListener);
    if (listener == INVALID_SOCKET) return;
    g_serverListener = INVALID_SOCKET;
#ifdef _WIN32
    shutdown(listener, SD_BOTH);
    closesocket(listener);
#else
    shutdown(listener, SHUT_RDWR);
    close(listener);
#endif
}

bool serverShutdownRequested() noexcept {
    return g_serverShutdownRequested != 0;
}

bool serverLifecycleStarted() noexcept {
    return g_serverLifecycleStarted != 0;
}

// --- Global connection and request-rate counters ---
static std::atomic<int64_t> g_activeConnections{0};
static std::atomic<uint64_t> g_totalRequestCount{0};
static std::atomic<uint64_t> g_lastRequestCount{0};
static std::atomic<double> g_currentRPS{0.0};
static std::atomic<double> g_requestLatencyEwmaMs{0.0};
static std::atomic<uint64_t> g_acceptQueueDepth{0};
static std::atomic<uint64_t> g_responsePending{0};
static std::atomic<uint64_t> g_inflightFutures{0};
static std::atomic<uint64_t> g_connectionLimitRejects{0};
static std::atomic<uint64_t> g_queueHighRejects{0};
static std::atomic<uint64_t> g_serverOverloadedRejects{0};
static std::atomic<uint64_t> g_connectionLimitWaits{0};
static std::atomic<uint64_t> g_connectionLimitWaitSuccesses{0};
static std::atomic<uint64_t> g_connectionLimitWaitTotalMs{0};
static std::atomic<uint64_t> g_connectionLimitWaitMaxMs{0};
static std::atomic<size_t> g_configuredMaxConnections{0};
static std::atomic<size_t> g_dynamicMaxConnectionsGauge{0};
static std::atomic<size_t> g_configuredQueueHighWatermark{0};
static std::atomic<size_t> g_dynamicQueueHighWatermarkGauge{0};
static std::chrono::steady_clock::time_point g_lastRPSTime = std::chrono::steady_clock::now();
static std::mutex g_rpsMutex;

// Phase C: Global read cache for hot-key acceleration
static pacificdb::ReadCache g_readCache(50000, 15000);

// Public accessors for metrics collection
int64_t Server_getActiveConnections() { return g_activeConnections.load(); }
double Server_getCurrentRPS() {
    // Calculate RPS from delta since last call
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_rpsMutex);
    double elapsed = std::chrono::duration<double>(now - g_lastRPSTime).count();
    if (elapsed >= 1.0) {
        uint64_t current = g_totalRequestCount.load();
        uint64_t last = g_lastRequestCount.load();
        g_currentRPS.store(static_cast<double>(current - last) / elapsed);
        g_lastRequestCount.store(current);
        g_lastRPSTime = now;
    }
    return g_currentRPS.load();
}

static inline void recordPipelineCounter(const std::string& metric, double delta = 1.0) {
    MetricsExporter::incrementCounter(metric, delta);
}

static inline uint64_t steadyNowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static inline void recordLifecycleCounter(const std::string& edge) {
    recordPipelineCounter("pacificdb_pipeline_" + edge + "_total");
}

static inline void decrementGaugeSafe(std::atomic<uint64_t>& gauge) {
    uint64_t current = gauge.load(std::memory_order_relaxed);
    while (current > 0 && !gauge.compare_exchange_weak(current, current - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

static inline void recordTimeoutOriginCounter(const std::string& origin) {
    recordPipelineCounter("pacificdb_pipeline_timeout_" + origin + "_total");
}

struct SocketSendResult {
    bool ok = false;
    bool partial = false;
    bool eagain = false;
    bool failed = false;
    int errorCode = 0;
    size_t bytesSent = 0;
};

static SocketSendResult sendTrackedPayload(SOCKET sock, const std::string& payload, int sendFlags) {
    SocketSendResult result;
    const char* cursor = payload.c_str();
    size_t remaining = payload.size();

    while (remaining > 0) {
        int sent = clientTransportSend(sock, cursor, remaining, sendFlags);
        if (sent > 0) {
            result.bytesSent += static_cast<size_t>(sent);
            cursor += sent;
            remaining -= static_cast<size_t>(sent);
            if (remaining > 0) {
                result.partial = true;
            }
            continue;
        }

        if (sent == 0) {
            result.failed = true;
            break;
        }

#ifdef _WIN32
        const int err = SOCKET_ERROR_CODE;
        result.errorCode = err;
        if (err == WSAEINTR) {
            continue;
        }
        if (err == WSAEWOULDBLOCK) {
            result.eagain = true;
            break;
        }
#else
        const int err = SOCKET_ERROR_CODE;
        result.errorCode = err;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            result.eagain = true;
            break;
        }
#endif

        result.failed = true;
        break;
    }

    result.ok = remaining == 0;
    if (!result.ok && result.errorCode == 0) {
        result.errorCode = SOCKET_ERROR_CODE;
    }
    return result;
}

static bool looksLikeCompleteJsonObject(const std::string& buffer) {
    bool started = false;
    bool inString = false;
    bool escaped = false;
    int depth = 0;

    for (size_t i = 0; i < buffer.size(); ++i) {
        const char ch = buffer[i];

        if (!started) {
            if (std::isspace(static_cast<unsigned char>(ch))) continue;
            if (ch != '{') return false;
            started = true;
            depth = 1;
            continue;
        }

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
        } else if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            --depth;
            if (depth == 0) {
                for (size_t j = i + 1; j < buffer.size(); ++j) {
                    if (!std::isspace(static_cast<unsigned char>(buffer[j]))) return false;
                }
                return true;
            }
            if (depth < 0) return false;
        }
    }

    return false;
}

static void updateInflightGauges() {
    MetricsExporter::recordCustomMetric("pacificdb_pipeline_accept_queue_depth", static_cast<double>(g_acceptQueueDepth.load()));
    MetricsExporter::recordCustomMetric("pacificdb_pipeline_response_pending", static_cast<double>(g_responsePending.load()));
    MetricsExporter::recordCustomMetric("pacificdb_pipeline_inflight_futures", static_cast<double>(g_inflightFutures.load()));

    if (g_connectionPool) {
        MetricsExporter::recordCustomMetric("pacificdb_pipeline_connpool_pending", static_cast<double>(g_connectionPool->getQueuedTasks()));
    }
    MetricsExporter::recordCustomMetric("pacificdb_pipeline_dbq_pending", static_cast<double>(DBTaskQueuePartitioned::instance().getTotalQueued()));

    const uint64_t commitIndex = RaftCore::instance().getCommitIndex();
    const uint64_t lastApplied = RaftCore::instance().getLastApplied();
    const uint64_t raftPending = commitIndex > lastApplied ? (commitIndex - lastApplied) : 0;
    MetricsExporter::recordCustomMetric("pacificdb_pipeline_raft_pending", static_cast<double>(raftPending));
    MetricsExporter::recordCustomMetric("pacificdb_pipeline_inflight_wal_batches", static_cast<double>(WAL::getPendingCount()));
}

static inline void recordRejectReasonCounter(const std::string& reason) {
    const std::string safeReason = reason.empty() ? "unknown" : reason;
    recordPipelineCounter("pacificdb_pipeline_rejected_" + safeReason + "_total");
}

static std::string toLowerAscii(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return input;
}

static std::string classifyResponseClass(const json& res) {
    if (!res.is_object()) return "non_object";

    if (res.contains("success") && res["success"].is_boolean() && res["success"].get<bool>()) {
        return "ok";
    }

    if (res.contains("status") && res["status"].is_string()) {
        const std::string status = toLowerAscii(res["status"].get<std::string>());
        if (status == "accepted") return "accepted";
        if (status == "ok" || status == "pong" || status == "inserted" || status == "done") return "ok";
        if (status == "error") return "error";
    }

    if (res.contains("error") && res["error"].is_string()) {
        const std::string error = toLowerAscii(res["error"].get<std::string>());
        if (error == "server_busy" || error == "server_overloaded") return "server_busy";
        if (error.find("timeout") != std::string::npos) return "timeout";
        if (error == "invalid json" || error == "invalid_json" || error == "parse_error") return "parse_error";
        return "error";
    }

    return "ok";
}

static void recordRejectReasonFromResponse(const json& res) {
    if (!res.is_object() || !res.contains("error") || !res["error"].is_string()) return;

    const std::string error = res["error"].get<std::string>();
    if (error == "server_busy") {
        if (res.contains("reason") && res["reason"].is_string()) {
            recordRejectReasonCounter(res["reason"].get<std::string>());
        } else {
            recordRejectReasonCounter("server_busy");
        }
        return;
    }

    if (error == "server_overloaded" || error == "memory_pressure" || error == "payload_too_large" || error == "connection_ttl") {
        recordRejectReasonCounter(error);
    }
}

// Debug logging toggle — disable in production for throughput
static bool g_debugLog = false;  // Set via ENGINE_DEBUG_LOG=1 env var
#define DLOG(x) do { if (g_debugLog) { std::cout << x; } } while(0)

static bool g_consistencyTrace = false; // Set via CONSISTENCY_TRACE=1
#define CTRACE(x) do { if (g_consistencyTrace) { std::cout << "[CONSISTENCY] " << x; } } while(0)

static bool g_reqTrace = false; // Set via REQ_TRACE=1
static long long g_reqTraceSlowMs = 0; // Optional threshold via REQ_TRACE_SLOW_MS
static bool g_hardReqLog = false; // Set via HARD_REQ_LOG=1
static bool g_socketZeroCopy = false; // Set via SOCKET_ZEROCOPY=1 (Linux only)
static std::atomic<unsigned long long> g_reqIdCounter{0};
static std::atomic<uint64_t> g_readBarrierAttempts{0};
static std::atomic<uint64_t> g_readBarrierFailures{0};
static std::atomic<long long> g_lastReadBarrierWaitMs{0};
static std::atomic<uint64_t> g_lastReadBarrierTerm{0};
static std::atomic<uint64_t> g_lastReadBarrierCommit{0};
static std::atomic<uint64_t> g_deterministicSeq{0};
static std::mutex g_deterministicLogMutex;

struct SessionFloorEntry {
    uint64_t version = 0;
    std::chrono::steady_clock::time_point updatedAt = std::chrono::steady_clock::now();
};

static std::unordered_map<std::string, SessionFloorEntry> g_sessionFloors;
static std::deque<std::string> g_sessionFloorOrder;
static std::mutex g_sessionFloorMutex;

static size_t sessionFloorMaxKeys() {
    static const size_t value = [] {
        const char* v = std::getenv("SESSION_TRACK_MAX_KEYS");
        if (!v) return size_t{50000};
        try { return std::max<size_t>(1024, std::stoul(v)); } catch (...) { return size_t{50000}; }
    }();
    return value;
}

static std::string sessionFloorKey(const std::string& sessionId,
                                   const std::string& userId,
                                   const std::string& dbName,
                                   const std::string& collection,
                                   const std::string& docKey) {
    return sessionId + "|" + userId + "|" + dbName + "|" + collection + "|" + docKey;
}

static uint64_t getSessionFloor(const std::string& sessionId,
                                const std::string& userId,
                                const std::string& dbName,
                                const std::string& collection,
                                const std::string& docKey,
                                bool* found) {
    if (found) *found = false;
    if (sessionId.empty() || docKey.empty()) return 0;
    const std::string key = sessionFloorKey(sessionId, userId, dbName, collection, docKey);
    auto lk = pacificdb::timing::makeTimedUniqueLock(g_sessionFloorMutex, "server_session_floor_mutex");
    auto it = g_sessionFloors.find(key);
    if (it == g_sessionFloors.end()) return 0;
    if (found) *found = true;
    return it->second.version;
}

static void updateSessionFloor(const std::string& sessionId,
                               const std::string& userId,
                               const std::string& dbName,
                               const std::string& collection,
                               const std::string& docKey,
                               uint64_t version) {
    if (sessionId.empty() || docKey.empty() || version == 0) return;
    const std::string key = sessionFloorKey(sessionId, userId, dbName, collection, docKey);
    const size_t maxKeys = sessionFloorMaxKeys();

    auto lk = pacificdb::timing::makeTimedUniqueLock(g_sessionFloorMutex, "server_session_floor_mutex");
    auto it = g_sessionFloors.find(key);
    if (it == g_sessionFloors.end()) {
        g_sessionFloors.emplace(key, SessionFloorEntry{version, std::chrono::steady_clock::now()});
        g_sessionFloorOrder.push_back(key);
    } else if (version > it->second.version) {
        it->second.version = version;
        it->second.updatedAt = std::chrono::steady_clock::now();
    }

    while (g_sessionFloors.size() > maxKeys && !g_sessionFloorOrder.empty()) {
        const std::string victim = g_sessionFloorOrder.front();
        g_sessionFloorOrder.pop_front();
        auto vit = g_sessionFloors.find(victim);
        if (vit != g_sessionFloors.end()) {
            g_sessionFloors.erase(vit);
        }
    }
}

static bool shouldTraceAction(const std::string& action) {
    return action == "insert" || action == "find";
}

static std::string toLowerCopy(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

static std::string normalizeReadConsistencyMode(const std::string& raw) {
    std::string normalized = toLowerCopy(raw);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), normalized.end());

    if (normalized == "linearizable" || normalized == "sequential") return "strong";
    if (normalized == "strong") return "strong";
    if (normalized == "bounded" || normalized == "boundedstaleness" ||
        normalized == "bounded-staleness" || normalized == "bounded_staleness") return "bounded";
    if (normalized == "eventual") return "eventual";
    if (normalized == "stale_ok" || normalized == "stale-ok" || normalized == "staleok") return "stale-ok";
    return "eventual";
}

static std::string consistencySemantics(const std::string& mode) {
    if (mode == "strong") return "linearizable";
    if (mode == "bounded") return "bounded_staleness";
    if (mode == "stale-ok") return "stale_follower_ok";
    return "eventual";
}

static std::string buildQueueKey(const std::string& userId,
                                 const std::string& dbName,
                                 const std::string& collection) {
    return userId + "|" + dbName + "|" + collection;
}

static std::string consistencyModeApiLabel(const std::string& mode) {
    if (mode == "strong") return "STRONG";
    if (mode == "bounded") return "BOUNDED";
    if (mode == "stale-ok") return "STALE_OK";
    return "EVENTUAL";
}

static std::optional<uint64_t> extractVersionField(const json& doc) {
    if (!doc.is_object()) return std::nullopt;
    if (doc.contains("_mvcc_version") && (doc["_mvcc_version"].is_number_unsigned() || doc["_mvcc_version"].is_number_integer())) {
        try { return doc["_mvcc_version"].get<uint64_t>(); } catch (...) { return std::nullopt; }
    }
    if (doc.contains("version") && (doc["version"].is_number_unsigned() || doc["version"].is_number_integer())) {
        try { return doc["version"].get<uint64_t>(); } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

static bool deterministicLogEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("DETERMINISTIC_OP_LOG");
        return v && *v;
    }();
    return enabled;
}

static std::string deterministicLogPath() {
    static const std::string path = [] {
        const char* v = std::getenv("DETERMINISTIC_OP_LOG");
        return v ? std::string(v) : std::string();
    }();
    return path;
}

static json sanitizeDeterministicPayload(const json& payload) {
    if (!payload.is_object()) return payload;
    json out = payload;
    const size_t maxArray = 4096;
    if (out.contains("binaryData") && out["binaryData"].is_array()) {
        const size_t len = out["binaryData"].size();
        if (len > maxArray) {
            out["binaryData"] = json{{"_truncated", true}, {"length", len}};
        }
    }
    return out;
}

static void appendDeterministicLog(const json& entry) {
    const std::string path = deterministicLogPath();
    if (path.empty()) return;
    std::lock_guard<std::mutex> lk(g_deterministicLogMutex);
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out << entry.dump() << "\n";
}

static bool engineAuthEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("ENGINE_AUTH_REQUIRED");
        if (!v) return false;
        const std::string s(v);
        return s == "1" || s == "true" || s == "TRUE";
    }();
    return enabled;
}

static bool isAuthExemptAction(const std::string& action) {
    return action == "ping" ||
           action == "observeLeaderTerm" || action == "observe_leader_term" ||
           action == "security_authenticate" ||
           action == "security_validate_token" ||
           action == "security_refresh_token" ||
           action == "tenantAuthenticate" ||
           action == "tenantValidateSession";
}

static std::optional<pacificdb::security::Permission> permissionForAction(const std::string& action) {
    using pacificdb::security::Permission;

    if (action == "find" || action == "count" || action == "collectionStats" || action == "listDatabases" ||
        action == "listCollections" || action == "listIndexes" ||
        action == "validateIndex" || action == "indexValidate" ||
        action == "admin_stable_payload_dump" || action == "admin_complete_payload_hash" ||
        action == "admin_raft_status" ||
        action == "admin_apply_status" || action == "admin_logical_write_status" ||
        action == "getWriteStatus" ||
        action == "admin_storage_visibility_check" || action == "admin_replication_status" ||
        action == "admin_wal_status" || action == "admin_lsm_status" ||
        action == "admin_compaction_status" || action == "admin_replay_check" ||
        action == "admin_storage_verify") {
        return Permission::READ;
    }
    if (action == "insert" || action == "updateOne" || action == "bulk") {
        return Permission::WRITE;
    }
    if (action == "deleteOne") {
        return Permission::DELETE;
    }
    if (action == "createDatabase") {
        return Permission::CREATE_DB;
    }
    if (action == "dropDatabase") {
        return Permission::DROP_DB;
    }
    if (action == "createCollection") {
        return Permission::CREATE_COLLECTION;
    }
    if (action == "dropCollection") {
        return Permission::DROP_COLLECTION;
    }
    if (action == "createIndex" || action == "rebuildIndex" || action == "indexRebuild") {
        return Permission::CREATE_COLLECTION;
    }
    if (action == "dropIndex") {
        return Permission::DROP_COLLECTION;
    }
    if (action == "create_backup" || action == "list_backups") {
        return Permission::BACKUP;
    }
    if (action == "restore_backup") {
        return Permission::RESTORE;
    }
    if (action == "rebalance_shards" || action == "migrate_shard") {
        return Permission::ADMIN;
    }

    return std::nullopt;
}

static std::string extractTraceId(const json& req, unsigned long long reqId) {
    if (req.contains("trace_id") && req["trace_id"].is_string()) {
        return req["trace_id"].get<std::string>();
    }
    if (req.contains("traceId") && req["traceId"].is_string()) {
        return req["traceId"].get<std::string>();
    }
    if (req.contains("requestId") && req["requestId"].is_string()) {
        return req["requestId"].get<std::string>();
    }
    if (req.contains("_requestId") && req["_requestId"].is_string()) {
        return req["_requestId"].get<std::string>();
    }

    return std::string("eng_") + std::to_string(reqId);
}

static json buildRaftWriteMeta(const json& req,
                               const std::string& requestId,
                               const std::string& traceId,
                               const std::string& traceParent) {
    json meta = {
        {"requestId", requestId},
        {"trace_id", traceId}
    };

    if (!traceParent.empty()) {
        meta["traceparent"] = traceParent;
    }

    if (req.contains("leader_term") && (req["leader_term"].is_number_unsigned() || req["leader_term"].is_number_integer())) {
        meta["leader_term"] = req["leader_term"];
    } else if (req.contains("leader_epoch") && (req["leader_epoch"].is_number_unsigned() || req["leader_epoch"].is_number_integer())) {
        meta["leader_term"] = req["leader_epoch"];
    }

    return meta;
}

static void stripLogicalWriteVolatileFields(json& doc) {
    if (!doc.is_object()) return;
    static const std::set<std::string> volatileFields = {
        "clientRequestId", "requestId", "_requestId", "trace_id", "traceparent",
        "_mvcc_commit_ms", "_mvcc_version", "_raft_commit_index", "_raft_term",
        "_visibility_floor", "_visibility_state", "created_at_ms", "created_txn",
        "deleted_at_ms", "deleted_txn", "version", "committed", "tenant_id",
        "_logicalWritePayloadHash"
    };
    for (const auto& key : volatileFields) {
        doc.erase(key);
    }
}

static std::string logicalWritePayloadHash(json doc) {
    stripLogicalWriteVolatileFields(doc);
    const std::string canonical = doc.dump();
    return pacificdb::durability::ChecksumCalculator::sha256(canonical.data(), canonical.size());
}

static json checkLogicalWriteReplay(const std::string& userId,
                                    const std::string& dbName,
                                    const std::string& collection,
                                    const std::string& logicalWriteId,
                                    const json& incomingData,
                                    const std::string& idempotencyKey,
                                    const std::string& requestId) {
    if (logicalWriteId.empty()) return json::object();
    try {
        json filter = {{"logicalWriteId", logicalWriteId}};
        auto existing = DatabaseEngine::find(userId, dbName, collection, filter, 2);
        if (existing.empty()) return json::object();

        const std::string incomingHash = logicalWritePayloadHash(incomingData);
        std::string existingHash;
        if (existing.front().contains("_logicalWritePayloadHash") && existing.front()["_logicalWritePayloadHash"].is_string()) {
            existingHash = existing.front()["_logicalWritePayloadHash"].get<std::string>();
        } else {
            existingHash = logicalWritePayloadHash(existing.front());
        }

        if (existingHash == incomingHash) {
            return {
                {"status", "ok"},
                {"id", existing.front().value("id", logicalWriteId)},
                {"logicalWriteId", logicalWriteId},
                {"idempotencyKey", idempotencyKey},
                {"requestId", requestId},
                {"writeLifecycleState", "APPLIED"},
                {"duplicate", true},
                {"idempotent", true},
                {"committed", true},
                {"applied", true},
                {"state", "APPLIED"}
            };
        }

        return {
            {"status", "error"},
            {"error", "idempotency_conflict"},
            {"logicalWriteId", logicalWriteId},
            {"idempotencyKey", idempotencyKey},
            {"requestId", requestId},
            {"writeLifecycleState", "REJECTED_CONFLICT"},
            {"duplicate", false},
            {"retryable", false},
            {"state", "REJECTED"},
            {"errorCode", "IDEMPOTENCY_CONFLICT"}
        };
    } catch (...) {
        return json::object();
    }
}

static bool extractExpectedLeaderTerm(const json& req, uint64_t& expectedOut) {
    if (req.contains("leader_term") && (req["leader_term"].is_number_unsigned() || req["leader_term"].is_number_integer())) {
        expectedOut = req["leader_term"].get<uint64_t>();
        return true;
    }
    if (req.contains("leader_epoch") && (req["leader_epoch"].is_number_unsigned() || req["leader_epoch"].is_number_integer())) {
        expectedOut = req["leader_epoch"].get<uint64_t>();
        return true;
    }
    return false;
}

static bool requireLeaderTermForStrong() {
    static const bool required = [] {
        const char* env = std::getenv("REQUIRE_LEADER_TERM_FOR_STRONG");
        if (!env) return false;
        const std::string v(env);
        return v == "1" || v == "true" || v == "TRUE";
    }();
    return required;
}

static bool rejectStaleLeaderTerm(const json& req, const std::string& consistency, json& res) {
    uint64_t expected = 0;
    bool hasExpected = extractExpectedLeaderTerm(req, expected);
    if (!hasExpected && consistency == "strong" && requireLeaderTermForStrong()) {
        res = {
            {"error", "leader_term_required"},
            {"observed_leader_term", RaftCore::instance().getCurrentTerm()},
            {"consistency", consistency},
            {"consistency_semantics", consistencySemantics(consistency)},
            {"retry_after_ms", 100}
        };
        return true;
    }
    if (!hasExpected) return false;

    uint64_t currentTerm = RaftCore::instance().getCurrentTerm();
    if (currentTerm == expected) return false;

    res = {
        {"error", "stale_leader_epoch"},
        {"expected_leader_term", expected},
        {"observed_leader_term", currentTerm},
        {"consistency", consistency},
        {"consistency_semantics", consistencySemantics(consistency)},
        {"retry_after_ms", 100}
    };
    return true;
}

struct StrongReadFenceState {
    uint64_t barrierTerm = 0;
    uint64_t barrierCommit = 0;
    long long barrierWaitMs = -1;
};

static void beginReadTelemetry(bool& active,
                               uint64_t& commitStart,
                               uint64_t& lastAppliedStart,
                               uint64_t& termStart) {
    active = true;
    commitStart = RaftCore::instance().getCommitIndex();
    lastAppliedStart = RaftCore::instance().getLastApplied();
    termStart = RaftCore::instance().getCurrentTerm();
}

static bool applyStrongReadFence(const json& req,
                                 const std::string& consistency,
                                 StrongReadFenceState& out,
                                 json& res) {
    if (consistency != "strong") return true;

    // V11.4-DIV-001 P0.6: a node with incomplete recovery, blocked apply, or
    // detected divergence must refuse strong reads outright — serving them
    // returned stale pre-batch state as if complete during the incident.
    // V11.4-IDX-001: index recovery is part of "recovered". A node whose snapshot-driven
    // index rebuild has not completed has base data but missing declared indexes, and an
    // empty catalog makes the validator report a false clean — so refuse strong reads.
    if (!RaftCore::instance().strongReadsAllowed() || !LSM::indexRecoverySettled()) {
        res = {
            {"error", "strong_reads_unavailable"},
            {"message", "node state incomplete or divergent; strong reads refused"},
            {"recoveryComplete", RaftCore::instance().isRecoveryComplete()},
            {"divergenceDetected", RaftCore::instance().isDivergent()},
            {"indexRecovery", LSM::indexRecoveryStatus()},
            {"consistency", "strong"},
            {"consistency_semantics", consistencySemantics(consistency)}
        };
        return false;
    }

    if (!RaftCore::instance().isLeader()) {
        res = {
            {"error", "not_leader"},
            {"message", "strong consistency requires leader read"},
            {"consistency", "strong"},
            {"consistency_semantics", consistencySemantics(consistency)}
        };
        return false;
    }

    if (rejectStaleLeaderTerm(req, consistency, res)) {
        return false;
    }

    uint64_t readStartTerm = RaftCore::instance().getCurrentTerm();
    uint64_t readStartCommit = RaftCore::instance().getCommitIndex();

    if (!RaftCore::instance().isEnabled()) {
        out.barrierTerm = readStartTerm;
        out.barrierCommit = readStartCommit;
        out.barrierWaitMs = 0;
        return true;
    }

    static const int barrierTimeoutMs = [] {
        if (const char* barrierEnv = std::getenv("RAFT_STRONG_READ_BARRIER_TIMEOUT_MS")) {
            try { return std::max(100, std::stoi(barrierEnv)); } catch (...) {}
        }
        return 1200;
    }();

    uint64_t barrierTerm = 0;
    uint64_t barrierCommit = 0;
    auto barrierStart = std::chrono::steady_clock::now();
    CTRACE("strong_read_barrier_begin term=" << RaftCore::instance().getCurrentTerm()
           << " timeout_ms=" << barrierTimeoutMs << "\n");
    g_readBarrierAttempts.fetch_add(1);
    bool barrierOk = RaftCore::instance().strongReadBarrier(barrierTimeoutMs, &barrierTerm, &barrierCommit);
    auto barrierElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - barrierStart).count();

    out.barrierWaitMs = static_cast<long long>(barrierElapsedMs);
    g_lastReadBarrierWaitMs.store(out.barrierWaitMs);
    g_lastReadBarrierTerm.store(barrierTerm);
    g_lastReadBarrierCommit.store(barrierCommit);
    MetricsExporter::recordCustomMetric("read_barrier_wait_ms", static_cast<double>(out.barrierWaitMs));
    MetricsExporter::recordCustomMetric("lease_epoch", static_cast<double>(barrierTerm));
    MetricsExporter::recordCustomMetric("read_barrier_attempts", static_cast<double>(g_readBarrierAttempts.load()));

    if (!barrierOk) {
        g_readBarrierFailures.fetch_add(1);
        MetricsExporter::recordCustomMetric("read_barrier_failures", static_cast<double>(g_readBarrierFailures.load()));
        CTRACE("strong_read_barrier_failed term=" << barrierTerm
               << " commit=" << barrierCommit
               << " elapsed_ms=" << barrierElapsedMs << "\n");
        res = {
            {"error", "strong_read_barrier_failed"},
            {"message", "unable to verify leader quorum for strong read"},
            {"consistency", "strong"},
            {"consistency_semantics", consistencySemantics(consistency)},
            {"leader_term", barrierTerm},
            {"commit_index", barrierCommit},
            {"retry_after_ms", 100}
        };
        return false;
    }

    if (readStartTerm != barrierTerm) {
        res = {
            {"error", "term_changed_during_read"},
            {"message", "leader term changed while establishing read barrier; retry"},
            {"start_term", readStartTerm},
            {"barrier_term", barrierTerm},
            {"consistency", "strong"},
            {"consistency_semantics", consistencySemantics(consistency)},
            {"retry_after_ms", 50}
        };
        return false;
    }

    uint64_t liveCommit = RaftCore::instance().getCommitIndex();
    if (liveCommit > barrierCommit) {
        barrierCommit = liveCommit;
    }
    if (readStartCommit > barrierCommit) {
        barrierCommit = readStartCommit;
    }

    out.barrierTerm = barrierTerm;
    out.barrierCommit = barrierCommit;
    CTRACE("strong_read_barrier_ok term=" << barrierTerm
           << " commit=" << barrierCommit
           << " elapsed_ms=" << barrierElapsedMs << "\n");

    uint64_t postBarrierTerm = RaftCore::instance().getCurrentTerm();
    if (postBarrierTerm != barrierTerm) {
        CTRACE("strong_read_term_changed barrier_term=" << barrierTerm
               << " current_term=" << postBarrierTerm << "\n");
        res = {
            {"error", "term_changed_during_read"},
            {"message", "leader term advanced after read barrier; retry"},
            {"barrier_term", barrierTerm},
            {"current_term", postBarrierTerm},
            {"consistency", "strong"},
            {"consistency_semantics", consistencySemantics(consistency)},
            {"retry_after_ms", 50}
        };
        return false;
    }

    uint64_t targetCommit = out.barrierCommit;
    uint64_t lastApplied = RaftCore::instance().getLastApplied();
    if (lastApplied < targetCommit) {
        CTRACE("strong_read_apply_wait begin commit=" << targetCommit
               << " last_applied=" << lastApplied << "\n");
        RaftCore::instance().applyCommittedUpTo(targetCommit);
        lastApplied = RaftCore::instance().getLastApplied();

        static const int applyWaitMs = [] {
            if (const char* waitEnv = std::getenv("RAFT_STRONG_READ_APPLY_TIMEOUT_MS")) {
                try { return std::max(0, std::stoi(waitEnv)); } catch (...) {}
            }
            return 200;
        }();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(applyWaitMs);
        while (lastApplied < targetCommit && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lastApplied = RaftCore::instance().getLastApplied();
        }
        if (lastApplied < targetCommit) {
            CTRACE("strong_read_apply_timeout commit=" << targetCommit
                   << " last_applied=" << lastApplied << "\n");
            res = {
                {"error", "not_visible_yet"},
                {"message", "commit index not yet applied locally for strong read"},
                {"consistency", "strong"},
                {"consistency_semantics", consistencySemantics(consistency)},
                {"commit_index", targetCommit},
                {"last_applied", lastApplied},
                {"retry_after_ms", 100}
            };
            return false;
        }
        CTRACE("strong_read_apply_ok commit=" << targetCommit
               << " last_applied=" << lastApplied << "\n");
    }

    if (out.barrierTerm > 0) {
        uint64_t postApplyTerm = RaftCore::instance().getCurrentTerm();
        if (postApplyTerm != out.barrierTerm) {
            res = {
                {"error", "term_changed_during_read"},
                {"message", "leader term advanced after apply; retry"},
                {"barrier_term", out.barrierTerm},
                {"current_term", postApplyTerm},
                {"consistency", "strong"},
                {"consistency_semantics", consistencySemantics(consistency)},
                {"retry_after_ms", 50}
            };
            return false;
        }
    }

    return true;
}

// Helper: track operation on the shard that owns this routing key
static void trackShardOp(const std::string& userId, const std::string& dbName,
                         const std::string& opType, double latencyMs, size_t bytes, bool success) {
    std::string routingKey = userId + "/" + dbName;
    std::string shardId = ShardManager::instance().getShardForKey(routingKey);
    if (!shardId.empty()) {
        ShardManager::instance().recordOperation(shardId, opType, latencyMs, bytes, success);
        ShardManager::instance().updateShardQueryLoad(shardId, latencyMs);
    }
}

static uint64_t steadyNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

class ShardRateLimiter {
public:
    bool allow(const std::string& shardId,
               bool isWrite,
               double cost,
               double loadFactor,
               int& retryAfterMs) {
        if (shardId.empty()) return true;
        std::lock_guard<std::mutex> lk(mu_);
        initLocked();
        if (!enabled_) return true;

        const double rate = isWrite ? writeRate_ : readRate_;
        const double burst = isWrite ? writeBurst_ : readBurst_;
        if (rate <= 0.0 || burst <= 0.0) return true;

        TokenBucket& bucket = isWrite ? writeBuckets_[shardId] : readBuckets_[shardId];
        bucket.configure(rate, burst);

        const double boundedLoad = std::max(0.0, std::min(1.0, loadFactor));
        const double penalty = 1.0 + (loadPenaltyScale_ * boundedLoad);
        const double adjustedCost = std::max(1.0, cost) * penalty;
        return bucket.consume(adjustedCost, retryAfterMs);
    }

private:
    struct TokenBucket {
        double tokens = 0.0;
        double rate = 0.0;
        double burst = 0.0;
        std::chrono::steady_clock::time_point lastRefill = std::chrono::steady_clock::now();

        void configure(double newRate, double newBurst) {
            if (newRate < 0.0) newRate = 0.0;
            if (newBurst < 0.0) newBurst = 0.0;
            if (rate != newRate || burst != newBurst) {
                rate = newRate;
                burst = newBurst;
                tokens = std::min(tokens, burst);
                if (tokens <= 0.0) tokens = burst;
                lastRefill = std::chrono::steady_clock::now();
            } else if (tokens <= 0.0 && burst > 0.0) {
                tokens = burst;
                lastRefill = std::chrono::steady_clock::now();
            }
        }

        bool consume(double cost, int& retryAfterMs) {
            if (rate <= 0.0 || burst <= 0.0) {
                retryAfterMs = 0;
                return true;
            }

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - lastRefill).count();
            if (elapsed > 0.0) {
                tokens = std::min(burst, tokens + (elapsed * rate));
                lastRefill = now;
            }

            if (tokens >= cost) {
                tokens -= cost;
                retryAfterMs = 0;
                return true;
            }

            double deficit = cost - tokens;
            retryAfterMs = static_cast<int>((deficit / rate) * 1000.0) + 1;
            if (retryAfterMs < 1) retryAfterMs = 1;
            return false;
        }
    };

    static double envDouble(const char* name, double defaultValue) {
        const char* v = std::getenv(name);
        if (!v) return defaultValue;
        try { return std::stod(v); } catch (...) { return defaultValue; }
    }

    void initLocked() {
        if (initialized_) return;
        initialized_ = true;

        const double baseRate = envDouble("SHARD_RATE_LIMIT_RPS", 0.0);
        readRate_ = envDouble("SHARD_RATE_LIMIT_READ_RPS", baseRate);
        writeRate_ = envDouble("SHARD_RATE_LIMIT_WRITE_RPS", baseRate);

        const double baseBurst = envDouble("SHARD_RATE_LIMIT_BURST", 0.0);
        readBurst_ = envDouble("SHARD_RATE_LIMIT_READ_BURST",
                               baseBurst > 0.0 ? baseBurst : (readRate_ > 0.0 ? readRate_ * 2.0 : 0.0));
        writeBurst_ = envDouble("SHARD_RATE_LIMIT_WRITE_BURST",
                                baseBurst > 0.0 ? baseBurst : (writeRate_ > 0.0 ? writeRate_ * 2.0 : 0.0));

        loadPenaltyScale_ = envDouble("SHARD_RATE_LIMIT_LOAD_PENALTY", 0.5);
        enabled_ = (readRate_ > 0.0 || writeRate_ > 0.0);
    }

    std::unordered_map<std::string, TokenBucket> readBuckets_;
    std::unordered_map<std::string, TokenBucket> writeBuckets_;
    std::mutex mu_;
    bool initialized_ = false;
    bool enabled_ = false;
    double readRate_ = 0.0;
    double writeRate_ = 0.0;
    double readBurst_ = 0.0;
    double writeBurst_ = 0.0;
    double loadPenaltyScale_ = 0.5;
};

static ShardRateLimiter g_shardRateLimiter;

static bool isRateLimitExemptAction(const std::string& action) {
    return action == "ping" || action == "opStatus" || action == "get_metrics" ||
           action == "security_authenticate" || action == "security_validate_token" ||
           action == "security_refresh_token" || action == "security_metrics" ||
           action == "tenantAuthenticate" || action == "tenantValidateSession" ||
           action == "rebalance_shards" || action == "migrate_shard";
}

static bool isWriteAction(const std::string& action) {
    return action == "insert" || action == "insertMany" ||
           action == "updateOne" || action == "updateMany" ||
           action == "deleteOne" || action == "deleteMany" ||
           action == "bulk" || action == "bulkWrite" ||
           action == "insertVector" ||
           action == "createDatabase" || action == "createCollection" ||
           action == "createIndex" || action == "dropIndex" ||
           action == "rebuildIndex" || action == "indexRebuild" ||
           action == "dropDatabase" || action == "dropCollection" ||
           action == "restore_backup" || action == "create_backup" || action == "migrate_shard";
}

static bool isBulkWriteAction(const std::string& action) {
    return action == "insertMany" || action == "bulk" || action == "bulkWrite" ||
           action == "updateMany" || action == "deleteMany";
}

// v2.9R — memory backpressure gate. Applies graduated backpressure to writes so
// the engine sheds/paces load instead of OOMing. Reads and ping are never gated
// (reads must keep flowing under write pressure).
//
// CRITICAL: replication-applied writes (marked `_repl`) are PACED but never
// REJECTED. The replica catch-up path applies WAL entries via these same insert
// actions and advances lastAppliedSeq even on error — rejecting one would skip a
// WAL entry and silently diverge the replica. So replicas only ever get delayed.
static bool applyMemoryBackpressure(const json& req, const std::string& action, json& res) {
    if (!isWriteAction(action)) return false;

    const bool isReplApply = req.is_object() &&
        (req.value("_repl", false) || req.contains("_repl_apply") || req.value("_replication", false));

    MemoryPressureState st = MemoryManager::getPressureState();

    if (st >= MemoryPressureState::READ_ONLY_EMERGENCY && !isReplApply) {
        MemoryManager::recordWriteRejection();
        res = {
            {"status", "throttled"},
            {"error", "read_only_emergency"},
            {"message", "engine under critical memory pressure; writes paused"},
            {"retry_after_ms", 200},
            {"memory_state", MemoryManager::pressureStateName(st)},
            {"memory_usage_percent", MemoryManager::getMemoryUsage() * 100.0}
        };
        return true;
    }
    if (st >= MemoryPressureState::BULK_REJECT && isBulkWriteAction(action) && !isReplApply) {
        MemoryManager::recordBulkRejection();
        res = {
            {"status", "throttled"},
            {"error", "bulk_rejected_memory_pressure"},
            {"message", "bulk writes paused under memory pressure; retry shortly"},
            {"retry_after_ms", 100},
            {"memory_state", MemoryManager::pressureStateName(st)},
            {"memory_usage_percent", MemoryManager::getMemoryUsage() * 100.0}
        };
        return true;
    }
    if (st >= MemoryPressureState::THROTTLED) {
        uint32_t d = MemoryManager::getThrottleDelayMs();
        if (d > 0) {
            MemoryManager::recordThrottleEvent();
            std::this_thread::sleep_for(std::chrono::milliseconds(d));
        }
    }
    return false;
}

static bool resolveShardId(const json& req, std::string& outShardId) {
    if (!req.is_object()) return false;
    std::string userId = req.value("userId", "");
    std::string dbName = req.value("dbName", req.value("db", std::string("")));
    if (userId.empty() || dbName.empty()) return false;
    std::string routingKey = userId + "/" + dbName;
    outShardId = ShardManager::instance().getShardForKey(routingKey);
    return !outShardId.empty();
}

static double resolveShardLoadFactor(const std::string& shardId) {
    if (shardId.empty()) return 0.0;
    ShardInfo info = ShardManager::instance().getShardInfo(shardId);
    return info.loadFactor;
}

static bool applyShardRateLimit(const json& req, const std::string& action, const std::string& shardId, json& res) {
    if (shardId.empty() || isRateLimitExemptAction(action)) return false;

    double cost = 1.0;
    if (isWriteAction(action)) {
        if (req.contains("data")) {
            try {
                cost = std::max(1.0, static_cast<double>(req["data"].dump().size()) / 1024.0);
            } catch (...) {
                cost = 1.0;
            }
        }
    }

    int retryAfterMs = 0;
    double loadFactor = resolveShardLoadFactor(shardId);
    bool allowed = g_shardRateLimiter.allow(shardId, isWriteAction(action), cost, loadFactor, retryAfterMs);
    if (allowed) return false;

    res = {
        {"error", "shard_rate_limited"},
        {"shard", shardId},
        {"retry_after_ms", std::max(1, retryAfterMs)},
        {"load_factor", loadFactor}
    };
    return true;
}

// Forward declaration — defined below at line ~522
static std::atomic<bool> g_memoryPressure;

static bool applyLoadShedding(const json& req,
                              const std::string& action,
                              const std::string& shardId,
                              json& res) {
    if (isRateLimitExemptAction(action)) return false;

    static const double threshold = [] {
        if (const char* env = std::getenv("SHARD_LOAD_SHED_THRESHOLD")) {
            try { return std::stod(env); } catch (...) {}
        }
        return 0.0;
    }();
    if (threshold <= 0.0) return false;

    double loadFactor = resolveShardLoadFactor(shardId);
    bool memoryPressure = g_memoryPressure.load();
    bool tooHot = memoryPressure || (loadFactor >= threshold);
    if (!tooHot) return false;

    static const bool shedStrong = [] {
        const char* env = std::getenv("SHARD_LOAD_SHED_STRONG");
        if (!env) return false;
        const std::string v(env);
        return v == "1" || v == "true" || v == "TRUE";
    }();

    std::string consistency = normalizeReadConsistencyMode(req.value("consistency", std::string("eventual")));
    if (!shedStrong && consistency == "strong" && !memoryPressure) return false;

    res = {
        {"error", "server_busy"},
        {"reason", "load_shed"},
        {"shard", shardId},
        {"load_factor", loadFactor},
        {"retry_after_ms", 150}
    };
    return true;
}

// Simple op id generator for async tasks
static std::atomic<unsigned long long> g_opCounter{0};
static std::mutex g_opMu;
static std::unordered_map<unsigned long long, std::string> g_opStatus; // pending|done|error

// Memory pressure: g_memoryPressure declared above applyLoadShedding
static std::mutex g_memoryLogMu;

static void updateLatencyEwma(double latencyMs) {
    constexpr double alpha = 0.15;
    double current = g_requestLatencyEwmaMs.load(std::memory_order_relaxed);
    double desired = (current <= 0.0) ? latencyMs : (alpha * latencyMs + (1.0 - alpha) * current);
    while (!g_requestLatencyEwmaMs.compare_exchange_weak(current, desired, std::memory_order_relaxed)) {
        desired = (current <= 0.0) ? latencyMs : (alpha * latencyMs + (1.0 - alpha) * current);
    }
}

#ifdef __linux__
static double sampleSystemCpuUsagePercent() {
    static std::mutex cpuMu;
    static unsigned long long prevTotal = 0;
    static unsigned long long prevBusy = 0;

    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) return 0.0;

    std::string cpu;
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    stat >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
    if (cpu != "cpu") return 0.0;

    unsigned long long busy = user + nice + sys + irq + softirq + steal;
    unsigned long long total = busy + idle + iowait;

    std::lock_guard<std::mutex> lock(cpuMu);
    if (prevTotal == 0 || total <= prevTotal) {
        prevTotal = total;
        prevBusy = busy;
        return 0.0;
    }

    unsigned long long deltaTotal = total - prevTotal;
    unsigned long long deltaBusy = (busy >= prevBusy) ? (busy - prevBusy) : 0;
    prevTotal = total;
    prevBusy = busy;

    if (deltaTotal == 0) return 0.0;
    return (static_cast<double>(deltaBusy) / static_cast<double>(deltaTotal)) * 100.0;
}
#endif

struct HotDocEntry {
    json doc;
    std::chrono::steady_clock::time_point expiresAt;
};

static std::unordered_map<std::string, HotDocEntry> g_hotDocCache;
static std::shared_mutex g_hotDocCacheMu;

static bool isHotCacheEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("FAST_FIND_HOT_CACHE");
        if (!v) return true;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE");
    }();
    return enabled;
}

static long long hotCacheTtlMs() {
    static const long long value = [] {
        const char* v = std::getenv("FAST_FIND_HOT_CACHE_TTL_MS");
        if (!v) return 10000LL;
        try { return std::max<long long>(1, std::stoll(v)); } catch (...) { return 10000LL; }
    }();
    return value;
}

static bool sampleRequestTelemetry(unsigned long long requestId) {
    static const unsigned long long every = [] {
        const char* raw = std::getenv("ENGINE_TELEMETRY_SAMPLE_RATE");
        if (!raw) return 100ULL;
        try {
            const double rate = std::stod(raw);
            if (rate <= 0.0) return 0ULL;
            if (rate >= 1.0) return 1ULL;
            return std::max(1ULL, static_cast<unsigned long long>(std::llround(1.0 / rate)));
        } catch (...) {
            return 100ULL;
        }
    }();
    return every != 0 && requestId % every == 0;
}

static std::string makeHotDocKey(const std::string& userId,
                                 const std::string& dbName,
                                 const std::string& collection,
                                 const std::string& id) {
    return userId + "|" + dbName + "|" + collection + "|" + id;
}

static std::optional<std::string> extractFilterId(const json& filter) {
    if (!filter.is_object()) return std::nullopt;
    if (!filter.contains("id") || !filter["id"].is_string()) return std::nullopt;
    return filter["id"].get<std::string>();
}

static void hotCacheUpsert(const std::string& userId,
                           const std::string& dbName,
                           const std::string& collection,
                           const json& doc) {
    if (!isHotCacheEnabled()) return;
    if (!doc.is_object() || !doc.contains("id") || !doc["id"].is_string()) return;
    std::string id = doc["id"].get<std::string>();
    if (id.empty()) return;

    HotDocEntry entry;
    entry.doc = doc;
    entry.expiresAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(hotCacheTtlMs());

    auto lk = pacificdb::timing::makeTimedUniqueLock(g_hotDocCacheMu, "server_hot_doc_cache_mutex");
    g_hotDocCache[makeHotDocKey(userId, dbName, collection, id)] = std::move(entry);
}

static std::optional<json> hotCacheGet(const std::string& userId,
                                       const std::string& dbName,
                                       const std::string& collection,
                                       const std::string& id) {
    if (!isHotCacheEnabled() || id.empty()) return std::nullopt;
    std::string key = makeHotDocKey(userId, dbName, collection, id);

    {
        auto lk = pacificdb::timing::makeTimedSharedLock(g_hotDocCacheMu, "server_hot_doc_cache_mutex");
        auto it = g_hotDocCache.find(key);
        if (it == g_hotDocCache.end()) return std::nullopt;
        if (std::chrono::steady_clock::now() > it->second.expiresAt) {
            // expired, fall-through to erase under unique lock
        } else {
            return it->second.doc;
        }
    }

    auto lk = pacificdb::timing::makeTimedUniqueLock(g_hotDocCacheMu, "server_hot_doc_cache_mutex");
    auto it = g_hotDocCache.find(key);
    if (it != g_hotDocCache.end() && std::chrono::steady_clock::now() > it->second.expiresAt) {
        g_hotDocCache.erase(it);
    }
    return std::nullopt;
}

static void hotCacheErase(const std::string& userId,
                          const std::string& dbName,
                          const std::string& collection,
                          const std::string& id) {
    if (!isHotCacheEnabled() || id.empty()) return;
    auto lk = pacificdb::timing::makeTimedUniqueLock(g_hotDocCacheMu, "server_hot_doc_cache_mutex");
    g_hotDocCache.erase(makeHotDocKey(userId, dbName, collection, id));
}

static void setOpStatus(unsigned long long id, const std::string& status) {
    auto lk = pacificdb::timing::makeTimedUniqueLock(g_opMu, "server_op_status_mutex");
    g_opStatus[id] = status;
    // Prevent unbounded growth: evict completed entries when map exceeds 10K
    if (g_opStatus.size() > 10000) {
        for (auto it = g_opStatus.begin(); it != g_opStatus.end(); ) {
            if (it->second == "done" || it->second == "error") {
                it = g_opStatus.erase(it);
            } else {
                ++it;
            }
        }
    }
}

static std::string getOpStatus(unsigned long long id) {
    auto lk = pacificdb::timing::makeTimedUniqueLock(g_opMu, "server_op_status_mutex");
    auto it = g_opStatus.find(id);
    if (it == g_opStatus.end()) return "unknown";
    return it->second;
}

void handleClient(unsigned long long clientSocket, long long enqueuedAtUs) {
    SOCKET sock = (SOCKET)clientSocket;
    struct ClientSocketGuard {
        SOCKET sock;
        bool open;
        explicit ClientSocketGuard(SOCKET s) : sock(s), open(s != INVALID_SOCKET) {
            if (open) trackClientSocket(sock);
        }
        void closeNow() {
            if (!open) return;
            untrackClientSocket(sock);
            detachClientTls(sock);
#ifdef _WIN32
            shutdown(sock, SD_BOTH);
#else
            shutdown(sock, SHUT_RDWR);
#endif
            CLOSE_SOCKET(sock);
            open = false;
        }
        ~ClientSocketGuard() { closeNow(); }
    } clientSocketGuard(sock);
    unsigned long long reqId = ++g_reqIdCounter;
    recordPipelineCounter("pacificdb_pipeline_dequeued_requests_total");
    recordLifecycleCounter("request_dequeued");
    decrementGaugeSafe(g_acceptQueueDepth);
    updateInflightGauges();

    if (!attachClientTls(sock)) {
        recordPipelineCounter("pacificdb_pipeline_tls_handshake_failed_total");
        clientSocketGuard.closeNow();
        return;
    }

    auto requestTiming = pacificdb::timing::makeRequestTimingContext();
    pacificdb::timing::setRequestTimingContext(requestTiming);
    struct RequestTimingScope {
        ~RequestTimingScope() { pacificdb::timing::clearRequestTimingContext(); }
    } requestTimingScope;

    auto workerStart = std::chrono::steady_clock::now();
    uint64_t requestReceivedUs = steadyNowUs();
    uint64_t requestQueuedUs = enqueuedAtUs > 0 ? static_cast<uint64_t>(enqueuedAtUs) : 0;
    uint64_t futureCreatedUs = 0;
    uint64_t futureResolvedUs = 0;
    uint64_t responseSerializedUs = 0;
    uint64_t responseSendStartedUs = 0;
    uint64_t responseSendCompletedUs = 0;
    uint64_t socketClosedUs = 0;
    uint64_t timeoutTriggeredUs = 0;
    bool telemetryActive = sampleRequestTelemetry(reqId);
    json lifecycle = telemetryActive ? json::object() : json();
    auto setLifecycle = [&](const char* key, const auto& value) {
        if (telemetryActive) lifecycle[key] = value;
    };
    auto activateTelemetry = [&] {
        if (telemetryActive) return;
        telemetryActive = true;
        lifecycle = json::object();
        setLifecycle("request_received_us", requestReceivedUs);
        setLifecycle("request_dequeued_us", requestReceivedUs);
        if (requestQueuedUs > 0) {
            setLifecycle("request_queued_us", requestQueuedUs);
            setLifecycle("request_queue_wait_us", requestReceivedUs > requestQueuedUs ? requestReceivedUs - requestQueuedUs : 0);
        }
    };
    setLifecycle("request_received_us", requestReceivedUs);
    setLifecycle("request_dequeued_us", requestReceivedUs);
    if (requestQueuedUs > 0) {
        setLifecycle("request_queued_us", requestQueuedUs);
        setLifecycle("request_queue_wait_us", requestReceivedUs > requestQueuedUs ? requestReceivedUs - requestQueuedUs : 0);
    }
    recordLifecycleCounter("request_received");
    double queueWaitMs = 0.0;
    uint64_t queueWaitUs = 0;
    if (enqueuedAtUs > 0) {
        auto nowUs = requestReceivedUs;
        if (nowUs > enqueuedAtUs) {
            queueWaitUs = static_cast<uint64_t>(nowUs - enqueuedAtUs);
            queueWaitMs = static_cast<double>(queueWaitUs) / 1000.0;
            pacificdb::timing::recordStage(pacificdb::timing::Stage::QueueWait, queueWaitUs);
            pacificdb::timing::recordStage(pacificdb::timing::Stage::AcceptWait, queueWaitUs);
        }
    }

    static const long long maxQueueWaitMs = [] {
        if (const char* q = std::getenv("MAX_QUEUE_WAIT_MS")) {
            try { return std::max<long long>(0, std::stoll(q)); } catch (...) {}
        }
        return 1000LL;
    }();
    if (maxQueueWaitMs > 0 && queueWaitMs > static_cast<double>(maxQueueWaitMs)) {
        recordPipelineCounter("pacificdb_pipeline_rejected_requests_total");
        recordRejectReasonCounter("queue_wait");
        recordTimeoutOriginCounter("prequeue");
        recordLifecycleCounter("timeout_triggered");
        timeoutTriggeredUs = steadyNowUs();
        recordLifecycleCounter("socket_closed");
        socketClosedUs = steadyNowUs();
        setLifecycle("timeout_triggered_us", timeoutTriggeredUs);
        setLifecycle("socket_closed_us", socketClosedUs);
        clientSocketGuard.closeNow();
        return;
    }

    // Track active connections for request-rate metrics
    g_activeConnections.fetch_add(1);
    g_totalRequestCount.fetch_add(1);

    // RAII guard to decrement on any exit path
    struct ConnGuard { ~ConnGuard() { g_activeConnections.fetch_sub(1); } } connGuard;

    // Production socket options for high throughput
    int optVal = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&optVal, sizeof(optVal));

#ifdef __linux__
    static const int keepIdleSec = [] {
        if (const char* env = std::getenv("TCP_KEEPIDLE_SEC")) {
            try { return std::max(10, std::stoi(env)); } catch (...) {}
        }
        return 60;
    }();
    static const int keepIntvlSec = [] {
        if (const char* env = std::getenv("TCP_KEEPINTVL_SEC")) {
            try { return std::max(5, std::stoi(env)); } catch (...) {}
        }
        return 10;
    }();
    static const int keepCnt = [] {
        if (const char* env = std::getenv("TCP_KEEPCNT")) {
            try { return std::max(3, std::stoi(env)); } catch (...) {}
        }
        return 6;
    }();
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdleSec, sizeof(keepIdleSec));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepIntvlSec, sizeof(keepIntvlSec));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCnt, sizeof(keepCnt));
#endif

    // TCP_NODELAY for low latency (disable Nagle's algorithm)
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&optVal, sizeof(optVal));

    // Set linger option - quick close for throughput
    struct linger lingerOpt = { 1, 1 };  // Linger on, 1 second timeout (was 5)
    setsockopt(sock, SOL_SOCKET, SO_LINGER, (char*)&lingerOpt, sizeof(lingerOpt));

#ifdef __linux__
#ifdef SO_ZEROCOPY
    if (g_socketZeroCopy) {
        setsockopt(sock, SOL_SOCKET, SO_ZEROCOPY, (char*)&optVal, sizeof(optVal));
    }
#endif
#endif

    // Adaptive timeout based on queue pressure (low=2s, medium=5s, high=15s)
    // Can be disabled via ADAPTIVE_SOCKET_TIMEOUT=0 and controlled with SOCKET_RECV_TIMEOUT_SEC.
    int recvTimeoutSec = 2;
    static const bool adaptiveTimeout = [] {
        const char* env = std::getenv("ADAPTIVE_SOCKET_TIMEOUT");
        return !(env && std::string(env) == "0");
    }();
    if (adaptiveTimeout && g_connectionPool) {
        size_t queued = g_connectionPool->getQueuedTasks();
        static const size_t maxQueue = [] {
            if (const char* q = std::getenv("CONN_MAX_QUEUE")) {
                try {
                    const size_t parsed = std::stoul(q);
                    if (parsed > 0) return parsed;
                } catch (...) {}
            }
            return size_t{16384};
        }();
        size_t pressurePct = std::min<size_t>(100, (queued * 100) / std::max<size_t>(1, maxQueue));
        if (pressurePct >= 70) recvTimeoutSec = 15;
        else if (pressurePct >= 30) recvTimeoutSec = 5;
        else recvTimeoutSec = 2;
    } else {
        static const int configuredRecvTimeoutSec = [] {
            if (const char* t = std::getenv("SOCKET_RECV_TIMEOUT_SEC")) {
                try { return std::max(1, std::stoi(t)); } catch (...) {}
            }
            return 2;
        }();
        recvTimeoutSec = configuredRecvTimeoutSec;
    }

    struct timeval recvTimeout;
    recvTimeout.tv_sec = recvTimeoutSec;
    recvTimeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&recvTimeout, sizeof(recvTimeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&recvTimeout, sizeof(recvTimeout));

    static const long long maxConnLifetimeMs = [] {
        if (const char* env = std::getenv("MAX_CONNECTION_LIFETIME_MS")) {
            try { return std::max<long long>(0, std::stoll(env)); } catch (...) {}
        }
        return 0LL;
    }();
    auto connStart = std::chrono::steady_clock::now();

    // ENGINE_KEEPALIVE_MAX_REQUESTS: serve multiple requests per TCP connection.
    // Client sends requests sequentially; server responds with JSON+\n per request.
    static const int _kaMX = [] {
        if (const char* env = std::getenv("ENGINE_KEEPALIVE_MAX_REQUESTS")) {
            try { return std::max(1, std::stoi(env)); } catch (...) {}
        }
        return 10000;
    }();
    static const long long _kaIdleMs = [] {
        if (const char* env = std::getenv("ENGINE_KEEPALIVE_IDLE_MS")) {
            try { return std::max<long long>(1000, std::stoll(env)); } catch (...) {}
        }
        return 30000LL;
    }();
    bool _kaSockClosed = false;

    std::string pendingInput;
    for (int _kaI = 0; _kaI < _kaMX && !_kaSockClosed; ++_kaI) {
        if (_kaI > 0) {
            // Reset all per-request state for subsequent keepalive requests
            reqId = ++g_reqIdCounter;
            g_totalRequestCount.fetch_add(1);
            recordPipelineCounter("pacificdb_pipeline_dequeued_requests_total");
            recordLifecycleCounter("request_dequeued");
            workerStart = std::chrono::steady_clock::now();
            requestReceivedUs = steadyNowUs();
            requestQueuedUs = 0;
            futureCreatedUs = 0;
            futureResolvedUs = 0;
            responseSerializedUs = 0;
            responseSendStartedUs = 0;
            responseSendCompletedUs = 0;
            socketClosedUs = 0;
            timeoutTriggeredUs = 0;
            telemetryActive = sampleRequestTelemetry(reqId);
            lifecycle = telemetryActive ? json::object() : json();
            setLifecycle("request_received_us", requestReceivedUs);
            setLifecycle("request_dequeued_us", requestReceivedUs);
            queueWaitMs = 0.0;
            queueWaitUs = 0;
            pacificdb::timing::clearRequestTimingContext();
            requestTiming = pacificdb::timing::makeRequestTimingContext();
            pacificdb::timing::setRequestTimingContext(requestTiming);
            // Idle recv/send timeout for subsequent requests
            struct timeval _kaIdleTv;
            _kaIdleTv.tv_sec = static_cast<long>(_kaIdleMs / 1000);
            _kaIdleTv.tv_usec = static_cast<long>((_kaIdleMs % 1000) * 1000);
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&_kaIdleTv, sizeof(_kaIdleTv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&_kaIdleTv, sizeof(_kaIdleTv));
        }

    // Increased buffer size and dynamic reading
    std::string buffer = std::move(pendingInput);
    buffer.reserve(8192);  // Pre-allocate for typical request size
    bool binaryWireV2 = false;
    bool framedPayloadTooLarge = false;
    std::size_t framedRequestBytes = 0;
    // Phase 1 Hardening: Unified payload limit — matches payloadConfig.js
    // Priority: MAX_REQUEST_SIZE_MB > ENGINE_MAX_PAYLOAD_BYTES > 16MB default
    static const int maxRequestBytes = []() -> int {
        // Primary: MAX_REQUEST_SIZE_MB (shared with Node.js payloadConfig.js)
        const char* mbEnv = std::getenv("MAX_REQUEST_SIZE_MB");
        if (mbEnv) {
            try {
                double mb = std::stod(mbEnv);
                if (mb > 0.0 && mb <= 256.0) return static_cast<int>(mb * 1024 * 1024);
            } catch (...) {}
        }
        // Fallback: ENGINE_MAX_PAYLOAD_BYTES
        if (const char* bytesEnv = std::getenv("ENGINE_MAX_PAYLOAD_BYTES")) {
            try {
                int parsed = std::stoi(bytesEnv);
                if (parsed >= 1024 && parsed <= 256 * 1024 * 1024) return parsed;
            } catch (...) {}
        }
        return 16 * 1024 * 1024; // 16 MB default (matches payloadConfig.js)
    }();

    static const int maxInMemoryPayload = [] {
        if (const char* env = std::getenv("ENGINE_MAX_IN_MEMORY_PAYLOAD")) {
            try {
                const int parsed = std::stoi(env);
                if (parsed >= 1024) return parsed;
            } catch (...) {}
        }
        return maxRequestBytes;
    }();
    char tempBuffer[65536]; // 64KB chunks
    int totalBytes = static_cast<int>(buffer.size());

    auto readStart = std::chrono::steady_clock::now();

    auto requestComplete = [&]() {
        if (buffer.size() >= sizeof(kEngineWireV2Magic) &&
            std::equal(std::begin(kEngineWireV2Magic), std::end(kEngineWireV2Magic), buffer.begin())) {
            binaryWireV2 = true;
            if (buffer.size() < sizeof(kEngineWireV2Magic) + sizeof(std::uint32_t)) return false;
            std::uint32_t networkLength = 0;
            std::memcpy(&networkLength, buffer.data() + sizeof(kEngineWireV2Magic), sizeof(networkLength));
            const std::size_t payloadBytes = ntohl(networkLength);
            if (payloadBytes > static_cast<std::size_t>(maxInMemoryPayload)) {
                framedPayloadTooLarge = true;
                return true;
            }
            framedRequestBytes = sizeof(kEngineWireV2Magic) + sizeof(networkLength) + payloadBytes;
            return buffer.size() >= framedRequestBytes;
        }
        return (!buffer.empty() && buffer.back() == '\n') || looksLikeCompleteJsonObject(buffer);
    };

    // Read data in chunks
    while (!requestComplete()) {
        if (maxConnLifetimeMs > 0) {
            auto aliveMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - connStart).count();
            if (aliveMs > maxConnLifetimeMs) {
                std::string rej = "{\"error\":\"connection_ttl\",\"retry_after_ms\":200}";
                recordLifecycleCounter("timeout_triggered");
                timeoutTriggeredUs = steadyNowUs();
                setLifecycle("timeout_triggered_us", timeoutTriggeredUs);
                recordLifecycleCounter("response_send_started");
                responseSendStartedUs = steadyNowUs();
                setLifecycle("response_send_started_us", responseSendStartedUs);
                auto ttlSend = sendTrackedPayload(sock, rej, 0);
                if (ttlSend.partial) recordPipelineCounter("pacificdb_pipeline_response_send_partial_total");
                if (ttlSend.eagain) recordPipelineCounter("pacificdb_pipeline_response_send_eagain_total");
                if (!ttlSend.ok) recordPipelineCounter("pacificdb_pipeline_response_send_failed_total");
                recordLifecycleCounter("response_send_completed");
                responseSendCompletedUs = steadyNowUs();
                setLifecycle("response_send_completed_us", responseSendCompletedUs);
                recordLifecycleCounter("socket_closed");
                socketClosedUs = steadyNowUs();
                setLifecycle("socket_closed_us", socketClosedUs);
                clientSocketGuard.closeNow();
                break;
            }
        }
        int bytes;
        do {
            bytes = clientTransportRecv(sock, tempBuffer, sizeof(tempBuffer), 0);
#ifndef _WIN32
            // A signal delivered to a worker may interrupt recv() even though the
            // keepalive connection is healthy. Treating EINTR as EOF silently closed
            // paced V10 client sockets and created false request failures.
        } while (bytes < 0 && errno == EINTR);
#else
        } while (bytes < 0 && WSAGetLastError() == WSAEINTR);
#endif

        if (bytes <= 0) {
            if (bytes == 0) {
                recordLifecycleCounter("client_disconnect");
                recordTimeoutOriginCounter("client_disconnect");
                setLifecycle("client_disconnect_us", steadyNowUs());
            } else {
#ifndef _WIN32
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    recordLifecycleCounter("timeout_triggered");
                    recordTimeoutOriginCounter("response_send");
                    setLifecycle("timeout_triggered_us", steadyNowUs());
                } else if (errno == ECONNRESET || errno == EPIPE) {
                    recordLifecycleCounter("client_disconnect");
                    recordTimeoutOriginCounter("client_disconnect");
                    setLifecycle("client_disconnect_us", steadyNowUs());
                } else {
                    recordPipelineCounter("pacificdb_pipeline_socket_recv_failed_total");
                    std::cerr << "[SERVER] recv() failed on keepalive socket, errno=" << errno << std::endl;
                }
#endif
            }
            break;
        }

        buffer.append(tempBuffer, bytes);
        totalBytes += bytes;

        // Safety limit: prevent unbounded memory growth
        if (totalBytes > maxInMemoryPayload) {
            std::cerr << "[SERVER] Payload exceeded in-memory limit (" << maxInMemoryPayload << ") - rejecting" << std::endl;
            std::string rej = "{\"error\":\"payload_too_large\",\"max_bytes\": " + std::to_string(maxInMemoryPayload) + "}";
            recordLifecycleCounter("response_send_started");
            responseSendStartedUs = steadyNowUs();
            setLifecycle("response_send_started_us", responseSendStartedUs);
            auto payloadSend = sendTrackedPayload(sock, rej, 0);
            if (payloadSend.partial) recordPipelineCounter("pacificdb_pipeline_response_send_partial_total");
            if (payloadSend.eagain) recordPipelineCounter("pacificdb_pipeline_response_send_eagain_total");
            if (!payloadSend.ok) recordPipelineCounter("pacificdb_pipeline_response_send_failed_total");
            recordLifecycleCounter("response_send_completed");
            responseSendCompletedUs = steadyNowUs();
            setLifecycle("response_send_completed_us", responseSendCompletedUs);
            recordLifecycleCounter("socket_closed");
            socketClosedUs = steadyNowUs();
            setLifecycle("socket_closed_us", socketClosedUs);
            clientSocketGuard.closeNow();
            return;
        }
    }

    if (buffer.empty()) {
        recordLifecycleCounter("socket_closed");
        socketClosedUs = steadyNowUs();
        setLifecycle("socket_closed_us", socketClosedUs);
        clientSocketGuard.closeNow();
        return;
    }

    if (framedPayloadTooLarge) {
        recordPipelineCounter("pacificdb_pipeline_rejected_requests_total");
        clientSocketGuard.closeNow();
        return;
    }

    if (binaryWireV2) {
        pendingInput = buffer.substr(framedRequestBytes);
        buffer = buffer.substr(sizeof(kEngineWireV2Magic) + sizeof(std::uint32_t),
                               framedRequestBytes - sizeof(kEngineWireV2Magic) - sizeof(std::uint32_t));
    } else if (const auto newline = buffer.find('\n'); newline != std::string::npos) {
        pendingInput = buffer.substr(newline + 1);
        buffer.resize(newline);
    }

    // Trim whitespace and newlines from buffer
    while (!buffer.empty() && (buffer.back() == '\n' || buffer.back() == '\r')) {
        buffer.pop_back();
    }

    auto readEnd = std::chrono::steady_clock::now();
    double readMs = std::chrono::duration<double, std::milli>(readEnd - readStart).count();

    DLOG("[SERVER] Received " << totalBytes << " bytes" << std::endl);

    auto parseStart = std::chrono::steady_clock::now();
    json req;
    if (binaryWireV2) {
        const std::vector<std::uint8_t> packed(buffer.begin(), buffer.end());
        req = json::from_msgpack(packed, true, false);
    } else {
        req = json::parse(buffer, nullptr, false);
    }
    auto parseEnd = std::chrono::steady_clock::now();
    double parseMs = std::chrono::duration<double, std::milli>(parseEnd - parseStart).count();
    pacificdb::timing::recordStage(pacificdb::timing::Stage::Parse,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(parseEnd - parseStart).count()));

    HashedAction action(req.is_discarded() ? "invalid_json" : req.value("action", ""));
    std::string requestId = req.is_discarded()
        ? (std::string("eng_req_") + std::to_string(reqId))
        : req.value("requestId", req.value("_requestId", std::string("eng_req_") + std::to_string(reqId)));
    std::string traceId = req.is_discarded() ? (std::string("eng_") + std::to_string(reqId)) : extractTraceId(req, reqId);
    std::string traceParent = req.is_discarded() ? "" : req.value("traceparent", std::string(""));

    std::string clientSessionId;
    bool hasClientLastSeen = false;
    uint64_t clientLastSeenVersion = 0;
    if (!req.is_discarded()) {
        if (req.contains("client_session_id") && req["client_session_id"].is_string()) {
            clientSessionId = req["client_session_id"].get<std::string>();
        } else if (req.contains("clientSessionId") && req["clientSessionId"].is_string()) {
            clientSessionId = req["clientSessionId"].get<std::string>();
        }
        if (req.contains("last_seen_version") && (req["last_seen_version"].is_number_unsigned() || req["last_seen_version"].is_number_integer())) {
            hasClientLastSeen = true;
            clientLastSeenVersion = req["last_seen_version"].get<uint64_t>();
        } else if (req.contains("lastSeenVersion") && (req["lastSeenVersion"].is_number_unsigned() || req["lastSeenVersion"].is_number_integer())) {
            hasClientLastSeen = true;
            clientLastSeenVersion = req["lastSeenVersion"].get<uint64_t>();
        }
    }

    if (!req.is_discarded()) {
        req["requestId"] = requestId;
        req["_requestId"] = requestId;
        req["trace_id"] = traceId;
        req["traceId"] = traceId;
        if (req.contains("consistency") && req["consistency"].is_string()) {
            req["consistency"] = normalizeReadConsistencyMode(req["consistency"].get<std::string>());
        }
    }

    bool hardReqLog = g_hardReqLog && shouldTraceAction(action);
    if (hardReqLog) {
        std::cout << "[REQ START] " << reqId << " action=" << action << " trace_id=" << traceId << std::endl;
    }
    json res;
    long long readBarrierWaitMs = -1;
    bool readTelemetryActive = false;
    uint64_t readCommitIndexStart = 0;
    uint64_t readCommitIndexFinish = 0;
    uint64_t readLastAppliedStart = 0;
    uint64_t readLastAppliedFinish = 0;
    uint64_t readTermStart = 0;
    uint64_t readTermFinish = 0;
    uint64_t readBarrierTerm = 0;
    uint64_t readBarrierCommit = 0;
    bool hasReturnedDocVersion = false;
    uint64_t returnedDocVersion = 0;
    bool hasMinimumVisibleVersion = false;
    uint64_t minimumVisibleVersion = 0;
    uint64_t readFloorGap = 0;
    uint64_t cacheInvalidations = 0;
    uint64_t readRetryCount = 0;
    bool readCacheHit = false;
    std::string sstVisibilitySource;
    // Refused requests must not receive cluster or tracing telemetry.
    bool authRejected = false;
    auto execStart = std::chrono::steady_clock::now();
    if (hardReqLog) {
        std::cout << "[EXEC START] " << reqId << std::endl;
    }

    try {
    auto authStart = std::chrono::steady_clock::now();
    if (req.is_discarded()) {
        res = { {"error", "Invalid JSON"} };
    }
    else {
        DLOG("[SERVER] Action = " << action << std::endl);

        // Optional engine-level auth/RBAC enforcement for core actions.
        if (engineAuthEnabled() && !isAuthExemptAction(action)) {
            std::string token = req.value("token", "");
            if (token.empty() || !pacificdb::security::SecurityManager::instance().validateToken(token)) {
                res = {
                    {"error", "unauthorized"},
                    {"message", "valid engine token required"}
                };
                authRejected = true;
            } else {
                auto perm = permissionForAction(action);
                if (perm.has_value()) {
                    std::string dbForAuth = req.value("dbName", req.value("db", std::string("")));
                    if (!pacificdb::security::SecurityManager::instance().hasPermission(token, *perm, dbForAuth)) {
                        res = {
                            {"error", "permission_denied"},
                            {"action", action}
                        };
                        authRejected = true;
                    }
                }
            }
        }
        auto authEnd = std::chrono::steady_clock::now();
        pacificdb::timing::recordStage(pacificdb::timing::Stage::Auth,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(authEnd - authStart).count()));

        // ---------------- PING ----------------
        if (!res.is_null()) {
            // Auth/RBAC rejected the request.
        }
        else if (applyMemoryBackpressure(req, action, res)) {
            // v2.9R: memory backpressure rejected/paused this write (reads unaffected).
        }
        else if (action == "ping") {
            // Include dynamic leader state so health checks can detect leadership changes
            res = { {"status", "pong"}, {"isLeader", RaftCore::instance().isLeader()},
                    {"leader_term", RaftCore::instance().getCurrentTerm()},
                    {"commit_index", RaftCore::instance().getCommitIndex()},
                    {"last_applied", RaftCore::instance().getLastApplied()} };
        }

        // v3.8 Phase 2 — STALE-LEADER FENCING (in-core): the control plane pushes the
        // newly-elected leader term into the engine. observeHigherTerm() is monotonic and
        // steps this node down, so afterwards rejectStaleLeaderTerm() rejects any write that
        // still carries the OLD leader_epoch — fencing enforced in the engine, not in Node.
        else if (action == "observeLeaderTerm" || action == "observe_leader_term") {
            uint64_t term = 0;
            if (req.contains("term") && (req["term"].is_number_unsigned() || req["term"].is_number_integer()))
                term = req["term"].get<uint64_t>();
            else if (req.contains("leader_epoch") && (req["leader_epoch"].is_number_unsigned() || req["leader_epoch"].is_number_integer()))
                term = req["leader_epoch"].get<uint64_t>();
            uint64_t before = RaftCore::instance().getCurrentTerm();
            if (term > before) RaftCore::instance().observeHigherTerm(term);
            res = { {"status", "ok"}, {"action", "observeLeaderTerm"},
                    {"previous_term", before}, {"current_term", RaftCore::instance().getCurrentTerm()},
                    {"is_leader", RaftCore::instance().isLeader()} };
        }

        // ---------------- ENGINE SECURITY: AUTHENTICATE ----------------
        else if (action == "security_authenticate") {
            std::string username = req.value("username", "");
            std::string password = req.value("password", "");
            std::string clientIP = req.value("clientIP", "unknown");

            if (username.empty() || password.empty()) {
                res = {{"error", "username and password required"}};
                authRejected = true;
            } else {
                auto token = pacificdb::security::SecurityManager::instance().authenticate(username, password, clientIP);
                if (!token.has_value()) {
                    res = {{"success", false}, {"error", "authentication_failed"}};
                    authRejected = true;
                } else {
                    res = {
                        {"success", true},
                        {"token", token->token},
                        {"username", token->username},
                        {"role", pacificdb::security::roleToString(token->role)},
                        {"expires_at", std::chrono::duration_cast<std::chrono::seconds>(token->expiresAt.time_since_epoch()).count()}
                    };
                }
            }
        }

        // ---------------- ENGINE SECURITY: VALIDATE TOKEN ----------------
        else if (action == "security_validate_token") {
            std::string token = req.value("token", "");
            bool valid = !token.empty() && pacificdb::security::SecurityManager::instance().validateToken(token);
            res = {
                {"success", true},
                {"valid", valid}
            };
            if (valid) {
                res["username"] = pacificdb::security::SecurityManager::instance().getTokenUsername(token);
                res["role"] = pacificdb::security::roleToString(
                    pacificdb::security::SecurityManager::instance().getTokenRole(token));
            }
        }

        // ---------------- ENGINE SECURITY: REFRESH TOKEN ----------------
        else if (action == "security_refresh_token") {
            std::string token = req.value("token", "");
            if (token.empty()) {
                res = {{"error", "token required"}};
            } else {
                auto refreshed = pacificdb::security::SecurityManager::instance().refreshToken(token);
                if (!refreshed.has_value()) {
                    res = {{"success", false}, {"error", "refresh_failed"}};
                } else {
                    res = {
                        {"success", true},
                        {"token", refreshed->token},
                        {"username", refreshed->username},
                        {"role", pacificdb::security::roleToString(refreshed->role)}
                    };
                }
            }
        }

        // ---------------- ENGINE SECURITY: METRICS ----------------
        else if (action == "security_metrics") {
            res = pacificdb::security::SecurityManager::instance().getSecurityMetrics();
            res["success"] = true;
        }

        // ---------------- BACKUP: CREATE ----------------
        else if (action == "create_backup") {
            if (!RaftCore::instance().isLeader()) {
                res = {{"error", "not_leader"}};
            } else {
                const std::string description = req.value("description", "api request");
                const auto documentCount = countAllRegisteredDocuments();
                const std::string backupId = BackupManager::instance().createFullBackup(
                    description, documentCount);
                if (backupId.empty()) {
                    res = {{"success", false}, {"error", "backup_failed"}};
                } else {
                    res = {{"success", true}, {"backup_id", backupId}, {"type", "full"}};
                }
            }
        }

        // ---------------- BACKUP: LIST ----------------
        else if (action == "list_backups") {
            json backups = json::array();
            for (const auto& b : BackupManager::instance().listBackups()) {
                backups.push_back(b.toJson());
            }
            res = {{"success", true}, {"backups", backups}};
        }

        // ---------------- BACKUP: VERIFY ----------------
        else if (action == "verify_backup") {
            std::string backupId = req.value("backup_id", "");
            if (backupId.empty()) {
                res = {{"error", "backup_id required"}};
            } else {
                bool ok = BackupManager::instance().verifyBackup(backupId);
                res = {{"success", ok}, {"backup_id", backupId}};
            }
        }

        // ---------------- BACKUP: RESTORE ----------------
        else if (action == "restore_backup") {
            if (!RaftCore::instance().isLeader()) {
                res = {{"error", "not_leader"}};
            } else {
                std::string backupId = req.value("backup_id", "");
                std::string targetDir = req.value("target_dir", "");
                if (backupId.empty() || targetDir.empty()) {
                    res = {{"error", "backup_id and target_dir required"}};
                } else {
                    const std::string targetClusterId =
                        req.value("target_cluster_id", "");
                    const std::string targetNodeId =
                        req.value("target_node_id", "");
                    bool ok = BackupManager::instance().restoreFromBackup(
                        backupId, targetDir, targetClusterId, targetNodeId);
                    res = {
                        {"success", ok},
                        {"backup_id", backupId},
                        {"target_dir", targetDir},
                        {"target_cluster_id", targetClusterId},
                        {"target_node_id", targetNodeId},
                    };
                }
            }
        }

        // ---------------- INIT USER SPACE ----------------
        else if (action == "initUserSpace") {
            // Only leader should perform workspace initialization to avoid split-state
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string userId = req.value("userId", "system");
                DatabaseEngine::ensureUserRoot(userId);
                res = { {"status", "ok"}, {"message", "user workspace initialized"} };
            }
        }

        // ---------------- CREATE DATABASE ----------------
        else if (action == "createDatabase") {
            // Only leader should accept database creation requests
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string userId = req.value("userId", "");
                std::string dbName = req.value("dbName", "");
                std::string dbType = req.value("dbType", "binary"); // Default to binary
                validateStorageIdentifier(userId, "userId");
                validateStorageIdentifier(dbName, "databaseName");
                validateDatabaseStorageBeforeReplication(userId, dbName);

                if (!dbName.empty()) {
                    std::string writeConsistency = normalizeReadConsistencyMode(req.value("consistency", std::string("eventual")));
                    if (rejectStaleLeaderTerm(req, writeConsistency, res)) {
                        // Response already set for stale leader term.
                    } else {
                    static const int raftTimeoutMs = [] {
                        if (const char* t = std::getenv("RAFT_SCHEMA_OP_TIMEOUT_MS")) {
                            try { return std::max(1000, std::stoi(t)); } catch (...) {}
                        }
                        return 15000;
                    }();

                    json entry = {
                        // IMPORTANT:
                        // RaftCore recognizes the logical action name, not only legacy op.
                        // Keep both fields for backward compatibility.
                        {"action", "createDatabase"},
                        {"op", "createDatabase"},
                        {"legacyOp", "CREATE_DB"},
                        {"userId", userId},
                        {"user", userId},
                        {"dbName", dbName},
                        {"db", dbName},
                        {"database", dbName},
                        {"dbType", dbType},
                        {"type", dbType},
                        {"requestId", requestId},
                        {"trace_id", traceId},
                        {"traceparent", traceParent}
                    };

                    std::cerr << "[DLOG][CREATE_DB][SUBMIT] user=" << userId
                              << " db=" << dbName
                              << " type=" << dbType
                              << " raftEnabled=" << RaftCore::instance().isEnabled()
                              << " leader=" << RaftCore::instance().isLeader()
                              << std::endl;

                    bool committed = false;
                    if (RaftCore::instance().isEnabled()) {
                        auto replicationStart = std::chrono::steady_clock::now();
                        committed = RaftCore::instance().replicateAndApply(entry, OperationPriority::HIGH, raftTimeoutMs);
                        auto replicationEnd = std::chrono::steady_clock::now();
                        pacificdb::timing::recordStage(pacificdb::timing::Stage::Replication,
                            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(replicationEnd - replicationStart).count()));
                    } else {
                        committed = DatabaseEngine::createDatabase(userId, dbName, dbType);
                    }

                    if (!committed) {
                        res = {
                            {"error", "write_not_committed"},
                            {"message", "createDatabase did not reach quorum commit"},
                            {"retry_after_ms", 100}
                        };
                    } else {
                        std::string dbId = "";
                        try {
                            json meta = DatabaseEngine::getDatabaseMetadata(userId, dbName);
                            if (!meta.is_discarded() && meta.contains("id")) dbId = meta["id"].get<std::string>();
                        } catch (...) {}

                        res = {
                            {"status", "ok"},
                            {"message", "database created"},
                            {"dbName", dbName},
                            {"dbType", dbType},
                            {"dbId", dbId}
                        };
                    }
                    std::cerr << "[DLOG][SCHEMA][RESPONSE] requestId=" << requestId
                              << " action=createDatabase"
                              << " committed=" << (committed ? "true" : "false")
                              << " user=" << userId
                              << " db=" << dbName << std::endl;
                    }
                } else {
                    res = { {"error", "dbName required"} };
                }
            }
        }

        // ---------------- CREATE COLLECTION ----------------
        else if (action == "createCollection") {
            // Only leader should accept collection creation requests
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string userId = req.value("userId", "");
                std::string dbName = req.value("dbName", "");
                std::string coll  = req.value("collection", "");
                validateStorageIdentifier(userId, "userId");
                validateStorageIdentifier(dbName, "databaseName");
                validateStorageIdentifier(coll, "collectionName");
                validateCollectionStorageBeforeReplication(userId, dbName, coll);

                if (!dbName.empty() && !coll.empty()) {
                    std::string writeConsistency = normalizeReadConsistencyMode(req.value("consistency", std::string("eventual")));
                    if (rejectStaleLeaderTerm(req, writeConsistency, res)) {
                        // Response already set for stale leader term.
                    } else {
                    static const int raftTimeoutMs = [] {
                        if (const char* t = std::getenv("RAFT_SCHEMA_OP_TIMEOUT_MS")) {
                            try { return std::max(1000, std::stoi(t)); } catch (...) {}
                        }
                        return 15000;
                    }();

                    json entry = {
                        {"action", "createCollection"},
                        {"op", "createCollection"},
                        {"legacyOp", "CREATE_COLLECTION"},
                        {"userId", userId},
                        {"user", userId},
                        {"dbName", dbName},
                        {"db", dbName},
                        {"database", dbName},
                        {"collection", coll},
                        {"collectionName", coll},
                        {"requestId", requestId},
                        {"trace_id", traceId},
                        {"traceparent", traceParent}
                    };

                    bool committed = false;
                    if (RaftCore::instance().isEnabled()) {
                        auto replicationStart = std::chrono::steady_clock::now();
                        committed = RaftCore::instance().replicateAndApply(entry, OperationPriority::HIGH, raftTimeoutMs);
                        auto replicationEnd = std::chrono::steady_clock::now();
                        pacificdb::timing::recordStage(pacificdb::timing::Stage::Replication,
                            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(replicationEnd - replicationStart).count()));
                    } else {
                        committed = !DatabaseEngine::createCollection(userId, dbName, coll).empty();
                    }

                    if (!committed) {
                        res = {
                            {"error", "write_not_committed"},
                            {"message", "createCollection did not reach quorum commit"},
                            {"retry_after_ms", 100}
                        };
                    } else {
                        std::string collId;
                        try {
                            json meta = DatabaseEngine::getDatabaseMetadata(userId, dbName);
                            if (meta.contains("collections") && meta["collections"].is_array()) {
                                for (const auto& c : meta["collections"]) {
                                    if (!c.is_object()) continue;
                                    if (c.value("name", std::string()) == coll && c.contains("id") && c["id"].is_string()) {
                                        collId = c["id"].get<std::string>();
                                        break;
                                    }
                                }
                            }
                        } catch (...) {}

                        res = { {"status", "ok"}, {"message", "collection created"}, {"collectionId", collId} };
                    }
                    std::cerr << "[DLOG][SCHEMA][RESPONSE] requestId=" << requestId
                              << " action=createCollection"
                              << " committed=" << (committed ? "true" : "false")
                              << " user=" << userId
                              << " db=" << dbName
                              << " collection=" << coll << std::endl;
                    }
                } else {
                    res = { {"error", "dbName and collection required"} };
                }
            }
        }

        // ---------------- DROP DATABASE / COLLECTION ----------------
        else if (action == "dropDatabase" || action == "dropCollection") {
            if (!RaftCore::instance().isLeader()) {
                res = {{"error", "not_leader"}};
            } else {
                const std::string userId = req.value("userId", "");
                const std::string dbName = req.value("dbName", "");
                const std::string collection = req.value("collection", "");
                const bool dropDb = action == "dropDatabase";
                validateStorageIdentifier(userId, "userId");
                validateStorageIdentifier(dbName, "databaseName");
                if (!dropDb) {
                    validateStorageIdentifier(collection, "collectionName");
                }
                if (dropDb) {
                    validateDatabaseStorageBeforeReplication(userId, dbName);
                } else {
                    validateCollectionStorageBeforeReplication(
                        userId, dbName, collection);
                }
                {
                    const std::string writeConsistency = normalizeReadConsistencyMode(
                        req.value("consistency", std::string("strong")));
                    if (!rejectStaleLeaderTerm(req, writeConsistency, res)) {
                        static const int raftTimeoutMs = [] {
                            if (const char* timeout = std::getenv("RAFT_SCHEMA_OP_TIMEOUT_MS")) {
                                try { return std::max(1000, std::stoi(timeout)); } catch (...) {}
                            }
                            return 30000;
                        }();
                        json entry = {
                            {"action", action},
                            {"op", action},
                            {"legacyOp", dropDb ? "DROP_DB" : "DROP_COLLECTION"},
                            {"userId", userId},
                            {"user", userId},
                            {"dbName", dbName},
                            {"db", dbName},
                            {"database", dbName},
                            {"requestId", requestId},
                            {"trace_id", traceId},
                            {"traceparent", traceParent}
                        };
                        if (!dropDb) entry["collection"] = collection;

                        bool committed = RaftCore::instance().isEnabled()
                            ? RaftCore::instance().replicateAndApply(entry, OperationPriority::HIGH, raftTimeoutMs)
                            : (dropDb
                                ? DatabaseEngine::dropDatabase(userId, dbName)
                                : DatabaseEngine::dropCollection(userId, dbName, collection));
                        if (committed && RaftCore::instance().isEnabled()) {
                            const uint64_t commitIndex = RaftCore::instance().getCommitIndex();
                            const uint64_t lastApplied = RaftCore::instance().getLastApplied();
                            if (commitIndex < lastApplied) {
                                std::cerr << "[ENGINE][DROP] refusing success with apply beyond commit"
                                          << " action=" << action
                                          << " commitIndex=" << commitIndex
                                          << " lastApplied=" << lastApplied << std::endl;
                                committed = false;
                            }
                        }
                        if (!committed) {
                            res = {
                                {"error", "write_not_committed"},
                                {"message", action + " did not reach quorum commit and apply"},
                                {"retry_after_ms", 100}
                            };
                        } else {
                            res = {
                                {"status", "ok"},
                                {"success", true},
                                {"message", dropDb ? "database deleted" : "collection deleted"},
                                {"database", dbName},
                                {"collection", dropDb ? "" : collection},
                                {"committed", true},
                                {"term", RaftCore::instance().getCurrentTerm()},
                                {"commitIndex", RaftCore::instance().getCommitIndex()},
                                {"lastApplied", RaftCore::instance().getLastApplied()}
                            };
                        }
                    }
                }
            }
        }

        // ---------------- LIST COLLECTIONS ----------------
        else if (action == "listCollections") {
            std::string userId = req.value("userId", "system");
            std::string dbName = req.value("dbName", "");

            if (dbName.empty()) {
                res = { {"error", "dbName required"} };
            } else {
                json meta = DatabaseEngine::getDatabaseMetadata(userId, dbName);
                if (meta.contains("collections") && meta["collections"].is_array()) {
                    res = meta["collections"];
                } else {
                    res = json::array();
                }
            }
        }

        // ---------------- CREATE INDEX ----------------
        else if (action == "createIndex") {
            std::string userId = req.value("userId", "system");
            std::string dbName = req.value("dbName", "");
            std::string coll = req.value("collection", "");
            std::string name = req.value("name", "");
            const bool nameProvided = req.contains("name");
            bool unique = req.value("unique", false);
            json fieldsJson = (req.contains("fields") && req["fields"].is_object()) ? req["fields"] : json::object();
            validateStorageIdentifier(userId, "userId");
            validateStorageIdentifier(dbName, "databaseName");
            validateStorageIdentifier(coll, "collectionName");
            validateCollectionStorageBeforeReplication(userId, dbName, coll);
            if (nameProvided) validateStorageIdentifier(name, "indexName");

            if (!RaftCore::instance().isLeader()) {
                res = {{"error", "not_leader"}};
            } else if (dbName.empty() || coll.empty() || fieldsJson.empty()) {
                res = { {"error", "dbName, collection, and fields required"} };
            } else if (fieldsJson.size() != 1 || unique) {
                res = {{"error", "index_type_not_supported"}, {"message", "Only non-unique single-field B-tree indexes are currently certified"}};
            } else {
                auto fieldIt = fieldsJson.begin();
                const std::string field = fieldIt.key();
                int order = (fieldIt.value().is_number_integer() && fieldIt.value().get<int>() < 0) ? -1 : 1;
                if (name.empty()) name = field + (order < 0 ? "_-1" : "_1");
                validateStorageIdentifier(name, "indexName");
                validateStorageIdentifier(field, "indexFieldName");
                json definition = {{"name", name}, {"type", "btree"}, {"field", field}, {"order", order}, {"unique", false}, {"sparse", req.value("sparse", false)}};
                json entry = {{"action", "createIndex"}, {"op", "createIndex"}, {"legacyOp", "CREATE_INDEX"}, {"userId", userId}, {"db", dbName}, {"collection", coll}, {"index", definition}, {"requestId", requestId}, {"trace_id", traceId}};
                int timeoutMs = 15000;
                bool committed = RaftCore::instance().isEnabled()
                    ? RaftCore::instance().replicateAndApply(entry, OperationPriority::HIGH, timeoutMs)
                    : LSM::createSecondaryIndex(userId, dbName, coll, definition).value("status", std::string()) != "error";
                res = committed ? LSM::listSecondaryIndexes(userId, dbName, coll)
                                : json{{"error", "write_not_committed"}, {"message", "createIndex did not reach quorum commit"}, {"retry_after_ms", 100}};
            }
        }

        // ---------------- LIST INDEXES ----------------
        else if (action == "listIndexes") {
            std::string userId = req.value("userId", "system");
            std::string dbName = req.value("dbName", "");
            std::string coll = req.value("collection", "");
            validateStorageIdentifier(userId, "userId");
            validateStorageIdentifier(dbName, "databaseName");
            validateStorageIdentifier(coll, "collectionName");

            if (dbName.empty() || coll.empty()) {
                res = { {"error", "dbName and collection required"} };
            } else {
                res = LSM::listSecondaryIndexes(userId, dbName, coll);
            }
        }

        // ---------------- VALIDATE INDEXES ----------------
        else if (action == "validateIndex" || action == "indexValidate") {
            std::string userId = req.value("userId", "system");
            std::string dbName = req.value("dbName", "");
            std::string coll = req.value("collection", "");
            validateStorageIdentifier(userId, "userId");
            validateStorageIdentifier(dbName, "databaseName");
            validateStorageIdentifier(coll, "collectionName");

            if (dbName.empty() || coll.empty()) {
                res = { {"error", "dbName and collection required"} };
            } else {
                res = LSM::validateColumnIndexes(userId, dbName, coll);
            }
        }

        // ---------------- REBUILD INDEXES ----------------
        else if (action == "rebuildIndex" || action == "indexRebuild") {
            std::string userId = req.value("userId", "system");
            std::string dbName = req.value("dbName", "");
            std::string coll = req.value("collection", "");

            std::string name = req.value("name", "");
            validateStorageIdentifier(userId, "userId");
            validateStorageIdentifier(dbName, "databaseName");
            validateStorageIdentifier(coll, "collectionName");
            validateStorageIdentifier(name, "indexName");
            validateCollectionStorageBeforeReplication(userId, dbName, coll);
            if (!RaftCore::instance().isLeader()) {
                res = {{"error", "not_leader"}};
            } else if (dbName.empty() || coll.empty() || name.empty()) {
                res = { {"error", "dbName, collection, and name required"} };
            } else {
                json entry = {{"action", "rebuildIndex"}, {"op", "rebuildIndex"}, {"legacyOp", "REBUILD_INDEX"}, {"userId", userId}, {"db", dbName}, {"collection", coll}, {"indexName", name}, {"requestId", requestId}};
                bool committed = RaftCore::instance().isEnabled()
                    ? RaftCore::instance().replicateAndApply(entry, OperationPriority::HIGH, 15000)
                    : LSM::rebuildSecondaryIndex(userId, dbName, coll, name).value("status", std::string()) != "error";
                res = committed ? json{{"status", "ok"}, {"name", name}, {"message", "index rebuilt"}}
                                : json{{"error", "write_not_committed"}, {"message", "rebuildIndex did not reach quorum commit"}};
            }
        }

        // ---------------- DROP INDEX ----------------
        else if (action == "dropIndex") {
            std::string userId = req.value("userId", "system");
            std::string dbName = req.value("dbName", "");
            std::string coll = req.value("collection", "");
            std::string name = req.value("name", "");
            validateStorageIdentifier(userId, "userId");
            validateStorageIdentifier(dbName, "databaseName");
            validateStorageIdentifier(coll, "collectionName");
            validateStorageIdentifier(name, "indexName");
            validateCollectionStorageBeforeReplication(userId, dbName, coll);

            if (!RaftCore::instance().isLeader()) {
                res = {{"error", "not_leader"}};
            } else if (dbName.empty() || coll.empty() || name.empty()) {
                res = { {"error", "dbName, collection, and name required"} };
            } else {
                json entry = {{"action", "dropIndex"}, {"op", "dropIndex"}, {"legacyOp", "DROP_INDEX"}, {"userId", userId}, {"db", dbName}, {"collection", coll}, {"indexName", name}, {"requestId", requestId}};
                bool committed = RaftCore::instance().isEnabled()
                    ? RaftCore::instance().replicateAndApply(entry, OperationPriority::HIGH, 15000)
                    : LSM::dropSecondaryIndex(userId, dbName, coll, name).value("status", std::string()) != "error";
                res = committed ? json{{"status", "ok"}, {"name", name}}
                                : json{{"error", "write_not_committed"}, {"message", "dropIndex did not reach quorum commit"}};
            }
        }

        // ---------------- LIST DATABASES ----------------
        else if (action == "listDatabases") {
            std::string userId = req.value("userId", "system");
            std::string readConsistency = normalizeReadConsistencyMode(req.value("consistency", std::string("eventual")));
            req["consistency"] = readConsistency;
            bool strongRead = (readConsistency == "strong");
            StrongReadFenceState fenceState;
            beginReadTelemetry(readTelemetryActive, readCommitIndexStart, readLastAppliedStart, readTermStart);
            if (res.is_null() && strongRead) {
                applyStrongReadFence(req, readConsistency, fenceState, res);
                readBarrierWaitMs = fenceState.barrierWaitMs;
                readBarrierTerm = fenceState.barrierTerm;
                readBarrierCommit = fenceState.barrierCommit;
            }
            if (res.is_null() && RaftCore::instance().isEnabled() && !RaftCore::instance().isRecoveryComplete()) {
                res = {
                    {"error", "not_visible_yet"},
                    {"message", "node recovery in progress; reads are blocked until replay completes"},
                    {"consistency", readConsistency},
                    {"consistency_semantics", consistencySemantics(readConsistency)},
                    {"retry_after_ms", 100}
                };
            }
            if (res.is_null()) {
                auto dbs = DatabaseEngine::listDatabases(userId);
                res = dbs; // respond as array
            }
        }

        // ---------------- INSERT ----------------
        else if (action == "insert") {
            // Followers should not accept writes; redirect to leader
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                // For small payloads, perform synchronous insert to avoid pending opStatus in tests
                auto userId = req.value("userId", "system");
                auto dbName = req["dbName"];
                auto coll = req["collection"];
                // v5.4R-3 Phase 0/1 fix: accept `document` as an alias for `data`.
                // RF3 harness (and other clients) send the body under `document`; the
                // engine previously only read `data`, so the application payload (and its
                // deterministic id) was silently dropped and an empty row was stored.
                auto data = req.contains("data") && !req["data"].is_null()
                    ? req["data"]
                    : (req.contains("document") && !req["document"].is_null() ? req["document"] : req["data"]);
                std::string dbNameStr = dbName.get<std::string>();
                std::string collStr = coll.get<std::string>();
                std::string queueKey = buildQueueKey(userId, dbNameStr, collStr);
                std::string writeConsistency = normalizeReadConsistencyMode(req.value("consistency", std::string("eventual")));
                bool strongWrite = (writeConsistency == "strong");
                json raftMeta = buildRaftWriteMeta(req, requestId, traceId, traceParent);
                std::string logicalWriteId = req.value("logicalWriteId", req.value("logical_write_id", std::string("")));
                std::string idempotencyKey = req.value("idempotencyKey", req.value("idempotency_key", std::string("")));
                if (logicalWriteId.empty() && !idempotencyKey.empty()) logicalWriteId = idempotencyKey;
                if (idempotencyKey.empty() && !logicalWriteId.empty()) idempotencyKey = logicalWriteId;
                if (!logicalWriteId.empty()) {
                    if (!data.is_object()) data = json::object();
                    data["logicalWriteId"] = logicalWriteId;
                    data["_logicalWriteId"] = logicalWriteId;
                    data["clientRequestId"] = requestId;
                    if (!idempotencyKey.empty()) {
                        data["idempotencyKey"] = idempotencyKey;
                        data["_idempotencyKey"] = idempotencyKey;
                    }
                    if (!data.contains("id") || data["id"].is_null() ||
                        (data["id"].is_string() && data["id"].get<std::string>().empty())) {
                        data["id"] = logicalWriteId;
                    }
                    data["_logicalWritePayloadHash"] = logicalWritePayloadHash(data);
                    raftMeta["logicalWriteId"] = logicalWriteId;
                    raftMeta["idempotencyKey"] = idempotencyKey;
                    raftMeta["clientRequestId"] = requestId;
                    raftMeta["writeLifecycleState"] = "ADMITTED";
                }

                if (rejectStaleLeaderTerm(req, writeConsistency, res)) {
                    // Response already set for stale leader term.
                } else {
                    if (!logicalWriteId.empty()) {
                        json replay = checkLogicalWriteReplay(userId, dbNameStr, collStr, logicalWriteId, data, idempotencyKey, requestId);
                        if (!replay.empty()) {
                            res = replay;
                        }
                    }
                    if (res.is_null()) {

                // Rough payload size heuristic
                size_t payloadSize = data.dump().size();
                static const size_t syncThreshold = [] {
                    if (const char* th = std::getenv("INSERT_SYNC_THRESHOLD_BYTES")) {
                        try { return static_cast<size_t>(std::stoul(th)); } catch (...) {}
                    }
                    return size_t{256 * 1024};
                }();

                // v2.1 Trace isolation: if this is a trace/observability write, always route
                // asynchronously at LOW priority so trace writes never contend with user writes.
                bool isTraceWrite = req.value("_trace_write", false);
                if (isTraceWrite) {
                    // Check if trace writes should be dropped under engine pressure
                    static const bool traceDropOnPressure = [] {
                        const char* env = std::getenv("TRACE_DROP_ON_PRESSURE");
                        return !(env && (std::string(env) == "0" || std::string(env) == "false"));
                    }();
                    if (traceDropOnPressure && DBTaskQueuePartitioned::instance().getTotalQueued() > 500) {
                        // Engine queue is deep — drop trace write silently
                        res = { {"status", "dropped"}, {"reason", "engine_pressure"}, {"_trace_write", true} };
                    } else {
                        auto opId = ++g_opCounter;
                        if (!data.contains("id") || data["id"].is_null() || (data["id"].is_string() && data["id"].get<std::string>().empty())) {
                            data["id"] = IDGenerator::generateObjectId();
                        }
                        bool enq = DBTaskQueuePartitioned::instance().enqueue([userId, dbName, coll, data, raftMeta, opId, payloadSize, requestTiming]() mutable {
                            pacificdb::timing::setRequestTimingContext(requestTiming);
                            struct TimingScope { ~TimingScope() { pacificdb::timing::clearRequestTimingContext(); } } timingScope;
                            try {
                                DatabaseEngine::insert(userId, dbName, coll, data, raftMeta);
                                setOpStatus(opId, "done");
                            } catch (...) {
                                setOpStatus(opId, "error");
                            }
                        }, DBTaskQueuePartitioned::Priority::LOW, queueKey, 1);
                        if (enq) {
                            setOpStatus(opId, "pending");
                            res = { {"status", "accepted"}, {"opId", opId}, {"id", data.value("id", std::string(""))}, {"_trace_write", true} };
                        } else {
                            res = { {"status", "dropped"}, {"reason", "queue_full"}, {"_trace_write", true} };
                        }
                    }
                } else {

                static const long long execTimeoutMs = [] {
                    if (const char* t = std::getenv("DB_EXEC_TIMEOUT_MS")) {
                        try { return std::max<long long>(1, std::stoll(t)); } catch (...) {}
                    }
                    return 10000LL;
                }();

                if (payloadSize <= syncThreshold || strongWrite) {
                    // Ensure canonical id exists and return it immediately
                    if (!data.contains("id") || data["id"].is_null() || (data["id"].is_string() && data["id"].get<std::string>().empty())) {
                        std::string generatedId = IDGenerator::generateObjectId();
                        data["id"] = generatedId;
                        DLOG("[SERVER] Generated id for insert (sync): " << generatedId << std::endl);
                    }

                    try {
                        pacificdb::timing::setRequestTimingContext(requestTiming);
                        struct TimingScope { ~TimingScope() { pacificdb::timing::clearRequestTimingContext(); } } timingScope;
                        DatabaseEngine::insert(userId, dbName, coll, data, raftMeta);
                        const std::string collName = coll.get<std::string>();
                        // json::value<std::string>() throws when "id" exists but is not a
                        // string, so a committed insert of {"id": 42} was reported back to
                        // the client as a failure. Report the string form the storage layer
                        // actually keys the document under.
                        const std::string id = (data.contains("id") && !data["id"].is_null())
                            ? (data["id"].is_string() ? data["id"].get<std::string>() : data["id"].dump())
                            : std::string("");
                        if (!id.empty()) {
                            g_readCache.invalidate(userId + ":" + dbNameStr + ":" + collName + ":" + id);
                        }
                        hotCacheUpsert(userId, dbName, collName, data);
                        EngineMetrics::recordInsert();
                        trackShardOp(userId, dbName, "write", 0, payloadSize, true);
                        res = json{{"status", "ok"}, {"id", id}};
                        if (!logicalWriteId.empty()) {
                            res["logicalWriteId"] = logicalWriteId;
                            res["idempotencyKey"] = idempotencyKey;
                            res["writeLifecycleState"] = "RESPONDED";
                            res["idempotent"] = true;
                        }
                        futureResolvedUs = steadyNowUs();
                        setLifecycle("future_resolved_us", futureResolvedUs);
                        recordLifecycleCounter("future_resolved");
                    } catch (const std::exception& e) {
                        trackShardOp(userId, dbName, "write", 0, 0, false);
                        res = json{{"status", "error"}, {"error", e.what()}};
                        if (!logicalWriteId.empty()) {
                            res["logicalWriteId"] = logicalWriteId;
                            res["idempotencyKey"] = idempotencyKey;
                            res["writeLifecycleState"] = std::string(e.what()) == "apply_overloaded" ? "REJECTED_BUSY" : "FAILED";
                            res["retryable"] = std::string(e.what()) == "apply_overloaded";
                        }
                    } catch (...) {
                        trackShardOp(userId, dbName, "write", 0, 0, false);
                        res = json{{"status", "error"}, {"error", "unknown insert failure"}};
                        if (!logicalWriteId.empty()) {
                            res["logicalWriteId"] = logicalWriteId;
                            res["idempotencyKey"] = idempotencyKey;
                            res["writeLifecycleState"] = "FAILED";
                        }
                    }

                    if (res.is_object()) {
                        std::string err = res.value("error", "");
                        if (err == "write_not_committed" || err == "not_leader") {
                            res["retry_after_ms"] = 100;
                            res["retryable"] = true;
                            if (!logicalWriteId.empty()) res["writeLifecycleState"] = err == "not_leader" ? "NOT_LEADER" : "DEADLINE_EXPIRED";
                        } else if (err == "apply_overloaded") {
                            static const int retryAfterMs = [] {
                                if (const char* v = std::getenv("APPLY_OVERLOAD_RETRY_AFTER_MS")) {
                                    try { return std::max(1, std::stoi(v)); } catch (...) {}
                                }
                                return 25;
                            }();
                            res["retryable"] = true;
                            res["retry_after_ms"] = retryAfterMs;
                            if (!logicalWriteId.empty()) res["writeLifecycleState"] = "REJECTED_BUSY";
                        }
                    }
                    if (!clientSessionId.empty() && res.is_object() && res.value("status", "") == "ok") {
                        std::string id = data.value("id", std::string(""));
                        bool fenceFound = false;
                        long long fenceVersion = DatabaseEngine::getReadFenceVersion(userId, dbNameStr, collStr, id, &fenceFound);
                        if (fenceFound && fenceVersion > 0) {
                            updateSessionFloor(clientSessionId, userId, dbNameStr, collStr, id, static_cast<uint64_t>(fenceVersion));
                        } else {
                            auto ver = extractVersionField(data);
                            if (!id.empty() && ver.has_value()) {
                                updateSessionFloor(clientSessionId, userId, dbNameStr, collStr, id, *ver);
                            }
                        }
                    }
                } else {
                    auto opId = ++g_opCounter;
                    // Ensure id exists before enqueue so we can return canonical id immediately
                    if (!data.contains("id") || data["id"].is_null() || (data["id"].is_string() && data["id"].get<std::string>().empty())) {
                        std::string gen = IDGenerator::generateObjectId();
                        data["id"] = gen;
                        DLOG("[SERVER] Generated id for insert (async): " << gen << std::endl);
                    }

                    bool enq = DBTaskQueuePartitioned::instance().enqueue([userId, dbName, coll, data, raftMeta, opId, payloadSize, requestTiming]() mutable {
                        pacificdb::timing::setRequestTimingContext(requestTiming);
                        struct TimingScope { ~TimingScope() { pacificdb::timing::clearRequestTimingContext(); } } timingScope;
                        try {
                            DatabaseEngine::insert(userId, dbName, coll, data, raftMeta);
                            const std::string collName = coll.get<std::string>();
                            const std::string id = data.value("id", std::string(""));
                            if (!id.empty()) {
                                g_readCache.invalidate(userId + ":" + dbName.get<std::string>() + ":" + collName + ":" + id);
                            }
                            hotCacheUpsert(userId, dbName, collName, data);
                            EngineMetrics::recordInsert();
                            trackShardOp(userId, dbName, "write", 0, payloadSize, true);
                            setOpStatus(opId, "done");
                        } catch (...) {
                            trackShardOp(userId, dbName, "write", 0, 0, false);
                            setOpStatus(opId, "error");
                        }
                    }, DBTaskQueuePartitioned::Priority::MEDIUM, queueKey, std::max<size_t>(1, payloadSize / 1024));

                    if (enq) {
                        setOpStatus(opId, "pending");
                        res = { {"status", "accepted"}, {"opId", opId},
                                {"id", data["id"].is_string() ? data["id"].get<std::string>() : data["id"].dump()} };
                    } else {
                        res = { {"error", "server_busy"}, {"retry_after_ms", 100} };
                    }
                }
                } // end else (non-trace write)
                    } // end logical write replay gate
            }
        }
        }

        // ---------------- INSERT VECTOR ----------------
        else if (action == "insertVector") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
            auto userId = req.value("userId", "system");
            auto dbName = req["dbName"];
            auto coll = req["collection"];
            auto data = req["data"];
            auto opId = ++g_opCounter;
            std::string dbNameStr = dbName.get<std::string>();
            std::string collStr = coll.get<std::string>();
            std::string queueKey = buildQueueKey(userId, dbNameStr, collStr);

            size_t vectorCost = std::max<size_t>(1, data.dump().size() / 1024);
            bool enq = DBTaskQueuePartitioned::instance().enqueue([userId, dbName, coll, data, opId]() mutable {
                try {
                    DatabaseEngine::insertVector(userId, dbName, coll, data);
                    EngineMetrics::recordInsert();
                    setOpStatus(opId, "done");
                } catch (...) {
                    setOpStatus(opId, "error");
                }
            }, DBTaskQueuePartitioned::Priority::MEDIUM, queueKey, vectorCost);

            if (enq) {
                setOpStatus(opId, "pending");
                res = { {"status", "accepted"}, {"opId", opId} };
            } else {
                res = { {"error", "server_busy"}, {"retry_after_ms", 100} };
            }
            }
        }

        // ---------------- FIND ----------------
        else if (action == "find") {
            DLOG("[SERVER] Dispatching FIND\n");

            std::string userId = req.value("userId", "system");
            std::string dbName = req.value("dbName", "");
            std::string collection = req.value("collection", "");
            std::string readConsistency = normalizeReadConsistencyMode(req.value("consistency", std::string("eventual")));
            req["consistency"] = readConsistency;
            bool strongRead = (readConsistency == "strong");
            StrongReadFenceState fenceState;
            readTelemetryActive = true;
            beginReadTelemetry(readTelemetryActive, readCommitIndexStart, readLastAppliedStart, readTermStart);
            if (res.is_null() && strongRead) {
                applyStrongReadFence(req, readConsistency, fenceState, res);
                readBarrierWaitMs = fenceState.barrierWaitMs;
                readBarrierTerm = fenceState.barrierTerm;
                readBarrierCommit = fenceState.barrierCommit;
            }

            if (res.is_null() && RaftCore::instance().isEnabled() && !RaftCore::instance().isRecoveryComplete()) {
                res = {
                    {"error", "not_visible_yet"},
                    {"message", "node recovery in progress; reads are blocked until replay completes"},
                    {"consistency", readConsistency},
                    {"consistency_semantics", consistencySemantics(readConsistency)},
                    {"retry_after_ms", 100}
                };
            }

            bool localUserExists = DatabaseEngine::userExists(userId);
            bool localDbExists = localUserExists && DatabaseEngine::databaseExists(userId, dbName);
            bool localCollectionExists = localDbExists && DatabaseEngine::collectionExists(userId, dbName, collection);

            // Validate resources exist before querying
            if (res.is_null() && !localUserExists) {
                // A completed strong-read barrier on the leader makes absence
                // authoritative. Returning not_visible_yet here caused an
                // empty console collection to surface as a gateway/network
                // failure and retry forever. Followers still fail as stale.
                if (RaftCore::instance().isEnabled() && !RaftCore::instance().isLeader()) {
                    res = {
                        {"error", "replica_stale"},
                        {"message", "follower metadata not yet applied"},
                        {"consistency", readConsistency},
                        {"consistency_semantics", consistencySemantics(readConsistency)}
                    };
                } else {
                    res = { {"error", "user_not_found"}, {"message", "User workspace does not exist"} };
                }
            } else if (res.is_null() && !localDbExists) {
                if (RaftCore::instance().isEnabled() && !RaftCore::instance().isLeader()) {
                    res = {
                        {"error", "replica_stale"},
                        {"message", "follower database metadata not yet applied"},
                        {"consistency", readConsistency},
                        {"consistency_semantics", consistencySemantics(readConsistency)}
                    };
                } else {
                    res = { {"error", "database_not_found"}, {"message", "Database does not exist"} };
                }
            } else if (res.is_null() && !localCollectionExists) {
                if (RaftCore::instance().isEnabled() && !RaftCore::instance().isLeader()) {
                    res = {
                        {"error", "replica_stale"},
                        {"message", "follower collection metadata not yet applied"},
                        {"consistency", readConsistency},
                        {"consistency_semantics", consistencySemantics(readConsistency)}
                    };
                } else {
                    res = { {"error", "collection_not_found"}, {"message", "Collection does not exist"} };
                }
            } else if (res.is_null()) {
                static const long long execTimeoutMs = [] {
                    if (const char* t = std::getenv("DB_EXEC_TIMEOUT_MS")) {
                        try { return std::max<long long>(1, std::stoll(t)); } catch (...) {}
                    }
                    return 10000LL;
                }();

                json filter = req["filter"];
                long long limit = -1;
                if (req.contains("limit") && req["limit"].is_number_integer()) {
                    limit = req["limit"].get<long long>();
                }
                long long offset = 0;
                if (req.contains("offset") && req["offset"].is_number_integer()) {
                    offset = req["offset"].get<long long>();
                }
                long long readFloorVersion = -1;
                long long maxStalenessMs = 0;
                if (req.contains("maxStalenessMs") && (req["maxStalenessMs"].is_number_integer() || req["maxStalenessMs"].is_number_unsigned())) {
                    maxStalenessMs = std::max<long long>(0, req["maxStalenessMs"].get<long long>());
                } else if (req.contains("max_staleness_ms") && (req["max_staleness_ms"].is_number_integer() || req["max_staleness_ms"].is_number_unsigned())) {
                    maxStalenessMs = std::max<long long>(0, req["max_staleness_ms"].get<long long>());
                }

                if (req.contains("minVersion") && (req["minVersion"].is_number_integer() || req["minVersion"].is_number_unsigned())) {
                    readFloorVersion = std::max<long long>(0, req["minVersion"].get<long long>());
                } else if (req.contains("read_your_writes_floor") && (req["read_your_writes_floor"].is_number_integer() || req["read_your_writes_floor"].is_number_unsigned())) {
                    readFloorVersion = std::max<long long>(0, req["read_your_writes_floor"].get<long long>());
                } else if (req.contains("readFloorVersion") && (req["readFloorVersion"].is_number_integer() || req["readFloorVersion"].is_number_unsigned())) {
                    readFloorVersion = std::max<long long>(0, req["readFloorVersion"].get<long long>());
                }

                static const bool directFindEnabled = [] {
                    const char* d = std::getenv("FAST_FIND_DIRECT");
                    if (!d) return true;
                    const std::string v(d);
                    return !(v == "0" || v == "false" || v == "FALSE");
                }();

                bool isSimpleIdFind = filter.is_object() && filter.size() == 1 &&
                                      filter.contains("id") && filter["id"].is_string();
                std::string readKey = isSimpleIdFind ? filter["id"].get<std::string>() : std::string();
                bool readContextSet = false;

                uint64_t sessionFloor = 0;
                bool sessionFloorFound = false;
                if (!clientSessionId.empty() && !readKey.empty()) {
                    sessionFloor = getSessionFloor(clientSessionId, userId, dbName, collection, readKey, &sessionFloorFound);
                    if (hasClientLastSeen && clientLastSeenVersion > sessionFloor) {
                        sessionFloor = clientLastSeenVersion;
                        updateSessionFloor(clientSessionId, userId, dbName, collection, readKey, clientLastSeenVersion);
                    }
                }
                if (sessionFloor > 0) {
                    readFloorVersion = std::max<long long>(readFloorVersion, static_cast<long long>(sessionFloor));
                }
                if (readFloorVersion >= 0) {
                    minimumVisibleVersion = static_cast<uint64_t>(readFloorVersion);
                    hasMinimumVisibleVersion = true;
                }

                bool forceAuthoritativeRead = strongRead || hasMinimumVisibleVersion || hasClientLastSeen || sessionFloorFound;

                bool handledDirectFind = false;

                if (strongRead && isSimpleIdFind) {
                    std::string wantedId = filter["id"].get<std::string>();
                    // The Raft barrier makes a plain strong read linearizable.
                    // A per-key version floor is required only when the client
                    // supplied one explicitly or through its session. Applying
                    // the process-local write fence to unrelated sessions can
                    // strand reads after recovery when that volatile fence is
                    // ahead of the latest materialized LSM version.

                    // ══════════════════════════════════════════════════════════════════════════════════════
                    // PHASE-1 VISIBILITY CLOSURE: SNAPSHOT-BOUND STRONG READ
                    //
                    // Enforces linearizability by:
                    // 1. Invalidating ALL cache layers (ID cache, read cache, lease cache)
                    // 2. Using memtable-first visibility (barrier-bound snapshot)
                    // 3. Enforcing strict per-key floor comparison
                    // 4. Capturing deterministic telemetry per attempt
                    //
                    // Expected results after patch:
                    // - strong_read_stale: 0
                    // - read_your_writes_violation: 0
                    // ══════════════════════════════════════════════════════════════════════════════════════
                    if (!wantedId.empty()) {
                        std::string cacheKey = userId + ":" + dbName + ":" + collection + ":" + wantedId;
                        g_readCache.invalidate(cacheKey);
                        hotCacheErase(userId, dbName, collection, wantedId);
                        cacheInvalidations += 1;
                    }

                    // Prepare floor version (use barrier commit as baseline for strong reads)
                    uint64_t effectiveFloor = (readFloorVersion >= 0) ? static_cast<uint64_t>(readFloorVersion) : 0;

                    // Use new snapshot-bound strong read path with mandatory memtable-first visibility
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(execTimeoutMs);
                    bool floorSatisfied = false;
                    long long observedVersion = -1;

                    while (std::chrono::steady_clock::now() < deadline && !floorSatisfied) {
                        readRetryCount += 1;

                        // ────────────────────────────────────────────────────────────────────────────────
                        // SNAPSHOT-BOUND READ: Query LSM with memtable-first precedence
                        // ────────────────────────────────────────────────────────────────────────────────
                        try {
                            json snapshotResult = LSM::findStrongSnapshotBound(userId, dbName, collection, wantedId, effectiveFloor);

                            bool found = snapshotResult.value("found", false);
                            bool satisfied = snapshotResult.value("floor_satisfied", false);
                            std::string visSource = snapshotResult.value("visibility_source", "UNKNOWN");

                            if (snapshotResult.contains("returned_version") &&
                                snapshotResult["returned_version"].is_number_integer()) {
                                observedVersion = snapshotResult["returned_version"].get<long long>();
                            }

                            // Extra forensic SST/iterator telemetry (if provided by LSM)
                            std::string sstFile = snapshotResult.value("sst_file", "");
                            uint64_t sstGeneration = 0;
                            if (snapshotResult.contains("sst_generation") && snapshotResult["sst_generation"].is_number_unsigned()) {
                                sstGeneration = snapshotResult["sst_generation"].get<uint64_t>();
                            }
                            uint64_t iteratorId = 0;
                            if (snapshotResult.contains("iterator_id") && snapshotResult["iterator_id"].is_number_unsigned()) {
                                iteratorId = snapshotResult["iterator_id"].get<uint64_t>();
                            }
                            uint64_t snapshotId = 0;
                            if (snapshotResult.contains("snapshot_id") && snapshotResult["snapshot_id"].is_number_unsigned()) {
                                snapshotId = snapshotResult["snapshot_id"].get<uint64_t>();
                            }

                            // Fix 4: Deterministic assert mode - Enhanced with comprehensive checks
                            static const bool detAssert = [] {
                                const char* value = std::getenv("DETERMINISTIC_STRONG_ASSERT");
                                return value && (*value == '1' || std::string(value) == "true");
                            }();
                            if (detAssert) {
                                // Assert 1: Floor constraint must be satisfied
                                if (found && hasMinimumVisibleVersion && observedVersion >= 0 && static_cast<uint64_t>(observedVersion) < minimumVisibleVersion) {
                                    std::cerr << "[DETERMINISTIC_ASSERT_FLOOR_VIOLATION] key=" << wantedId
                                              << " returned=" << observedVersion
                                              << " required_floor=" << minimumVisibleVersion
                                              << " gap=" << (minimumVisibleVersion - static_cast<uint64_t>(observedVersion))
                                              << " sst_file=" << sstFile
                                              << " sst_generation=" << sstGeneration
                                              << " iterator_id=" << iteratorId
                                              << " snapshot_id=" << snapshotId
                                              << " visibility_source=" << visSource
                                              << " found=" << (found ? "true" : "false")
                                              << " satisfied=" << (satisfied ? "true" : "false")
                                              << std::endl;
                                    std::abort();
                                }

                                // Assert 2: SST source must have valid version
                                if (found && visSource == "SST_L0" && observedVersion < 0) {
                                    std::cerr << "[DETERMINISTIC_ASSERT_INVALID_SST_VERSION] key=" << wantedId
                                              << " sst_file=" << sstFile
                                              << " sst_generation=" << sstGeneration
                                              << " returned_version=" << observedVersion
                                              << " iterator_id=" << iteratorId
                                              << std::endl;
                                    std::abort();
                                }

                                // Assert 3: If satisfying a floor, ensure it's actually satisfied
                                if (found && satisfied) {
                                    if (hasMinimumVisibleVersion && static_cast<uint64_t>(observedVersion) < minimumVisibleVersion) {
                                        std::cerr << "[DETERMINISTIC_ASSERT_SATISFIED_CONTRADICTION] key=" << wantedId
                                                  << " satisfied=true but returned=" << observedVersion
                                                  << " < floor=" << minimumVisibleVersion
                                                  << std::endl;
                                        std::abort();
                                    }
                                }
                            }

                            // ────────────────────────────────────────────────────────────────────────────────
                            // Mandatory per-attempt trace for Phase-1 closure validation
                            // ────────────────────────────────────────────────────────────────────────────────
                            CTRACE("strong_read_attempt"
                                   << " key=" << wantedId
                                   << " attempt=" << readRetryCount
                                   << " required_floor=" << effectiveFloor
                                   << " returned_version=" << observedVersion
                                   << " source=" << visSource
                                   << " cache_hit=false"
                                   << " commit_index=" << RaftCore::instance().getCommitIndex()
                                   << " last_applied=" << RaftCore::instance().getLastApplied()
                                   << " term=" << RaftCore::instance().getCurrentTerm()
                                   << " barrier_commit=" << fenceState.barrierCommit
                                   << " found=" << (found ? "true" : "false")
                                   << " satisfied=" << (satisfied ? "true" : "false") << "\n");

                            if (satisfied) {
                                // ────────────────────────────────────────────────────────────────────────────────
                                // SUCCESS: Floor constraint satisfied, return result
                                // ────────────────────────────────────────────────────────────────────────────────
                                auto docs = snapshotResult.value("documents", json::array());
                                res["status"] = "ok";
                                res["count"] = docs.size();
                                res["data"] = docs;
                                sstVisibilitySource = visSource; // Capture source for telemetry
                                if (found && observedVersion >= 0) {
                                    returnedDocVersion = static_cast<uint64_t>(observedVersion);
                                    hasReturnedDocVersion = true;
                                }
                                EngineMetrics::recordQuery(0, false, false);
                                trackShardOp(userId, dbName, "read", 0, 0, true);
                                handledDirectFind = true;
                                floorSatisfied = true;
                                break;
                            } else if (found && !satisfied) {
                                // ────────────────────────────────────────────────────────────────────────────────
                                // RETRY: Document exists but version < floor, retry with backoff
                                // ────────────────────────────────────────────────────────────────────────────────
                                CTRACE("strong_read_retry_version_gap"
                                       << " key=" << wantedId
                                       << " attempt=" << readRetryCount
                                       << " required=" << effectiveFloor
                                       << " observed=" << observedVersion
                                       << " gap=" << (effectiveFloor - static_cast<uint64_t>(std::max(0LL, observedVersion))) << "\n");

                                // Exponential backoff: 10ms base, capped at 100ms per retry
                                long long backoffMs = std::min(10LL * static_cast<long long>(readRetryCount), 100LL);
                                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                                continue;
                            } else {
                                // ────────────────────────────────────────────────────────────────────────────────
                                // RETRY: Document not found yet, retry
                                // ────────────────────────────────────────────────────────────────────────────────
                                CTRACE("strong_read_retry_not_found"
                                       << " key=" << wantedId
                                       << " attempt=" << readRetryCount
                                       << " required=" << effectiveFloor << "\n");

                                long long backoffMs = std::min(10LL * static_cast<long long>(readRetryCount), 100LL);
                                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                                continue;
                            }
                        } catch (const std::exception& e) {
                            CTRACE("strong_read_exception key=" << wantedId << " attempt=" << readRetryCount << " error=" << e.what() << "\n");
                            res = { {"error", "find_exception"}, {"message", e.what()} };
                            handledDirectFind = true;
                            break;
                        } catch (...) {
                            CTRACE("strong_read_exception_unknown key=" << wantedId << " attempt=" << readRetryCount << "\n");
                            res = { {"error", "find_exception"}, {"message", "unknown exception"} };
                            handledDirectFind = true;
                            break;
                        }
                    }

                    if (!handledDirectFind) {
                        // ────────────────────────────────────────────────────────────────────────────────
                        // TIMEOUT: Floor not met within timeout
                        // ────────────────────────────────────────────────────────────────────────────────
                        res = {
                            {"error", "read_floor_not_met"},
                            {"message", "strong read floor version not yet visible (snapshot-bound deadline exceeded)"},
                            {"expected_version", static_cast<long long>(effectiveFloor)},
                            {"observed_version", observedVersion},
                            {"retry_count", readRetryCount},
                            {"consistency", "strong"},
                            {"consistency_semantics", consistencySemantics(readConsistency)},
                            {"retry_after_ms", 100}
                        };
                        handledDirectFind = true;
                        CTRACE("strong_read_timeout key=" << wantedId << " expected=" << effectiveFloor << " observed=" << observedVersion << "\n");
                    }
                }

                if (!forceAuthoritativeRead && isSimpleIdFind) {
                    std::string wantedId = filter["id"].get<std::string>();

                    // Phase C: Core read cache — check before hot cache
                    std::string cacheKey = userId + ":" + dbName + ":" + collection + ":" + wantedId;
                    json cachedVal;
                    uint64_t cachedVersion = 0;
                    if (g_readCache.get(cacheKey, cachedVal, cachedVersion)) {
                        if (readFloorVersion < 0 || static_cast<long long>(cachedVersion) >= readFloorVersion) {
                            res["status"] = "ok";
                            res["count"] = 1;
                            res["data"] = json::array({cachedVal});
                            res["cache_hit"] = true;
                            readCacheHit = true;
                            sstVisibilitySource = "core_read_cache";
                            EngineMetrics::recordQuery(0, false, false);
                            trackShardOp(userId, dbName, "read", 0, 0, true);
                            handledDirectFind = true;
                        } else {
                            g_readCache.invalidate(cacheKey);
                            DatabaseEngine::invalidateReadCache(userId, dbName, collection, wantedId);
                            cacheInvalidations += 1;
                        }
                    }

                    if (!handledDirectFind) {
                        auto cached = hotCacheGet(userId, dbName, collection, wantedId);
                        if (cached.has_value()) {
                            auto hotVersion = extractVersionField(*cached);
                            if (readFloorVersion < 0 || (hotVersion.has_value() && static_cast<long long>(*hotVersion) >= readFloorVersion)) {
                                res["status"] = "ok";
                                res["count"] = 1;
                                res["data"] = json::array({*cached});
                                readCacheHit = true;
                                sstVisibilitySource = "hot_doc_cache";
                                EngineMetrics::recordQuery(0, false, false);
                                trackShardOp(userId, dbName, "read", 0, 0, true);
                                handledDirectFind = true;
                            } else {
                                hotCacheErase(userId, dbName, collection, wantedId);
                                DatabaseEngine::invalidateReadCache(userId, dbName, collection, wantedId);
                                cacheInvalidations += 1;
                            }
                        }
                    }
                }

                if (!handledDirectFind && directFindEnabled && isSimpleIdFind) {
                    try {
                        if (!readContextSet) {
                            DatabaseEngine::setReadContext(readConsistency, maxStalenessMs, readFloorVersion);
                            readContextSet = true;
                        }
                        auto results = DatabaseEngine::find(userId, dbName, collection, filter, limit, offset);
                        res["status"] = "ok";
                        res["count"] = results.size();
                        res["data"] = results;
                        sstVisibilitySource = "lsm_direct_find";
                        if (!results.empty()) {
                            auto ver = extractVersionField(results[0]);
                            if (ver.has_value()) {
                                returnedDocVersion = *ver;
                                hasReturnedDocVersion = true;
                            }
                        }
                        EngineMetrics::recordQuery(0, false, false);
                        trackShardOp(userId, dbName, "read", 0, 0, true);

                        // Phase C: Populate read cache on successful find
                        if (!results.empty() && !strongRead) {
                            std::string wantedId = filter["id"].get<std::string>();
                            std::string cacheKey = userId + ":" + dbName + ":" + collection + ":" + wantedId;
                            uint64_t version = 0;
                            if (results[0].contains("version") && results[0]["version"].is_number()) {
                                version = results[0]["version"].get<uint64_t>();
                            }
                            g_readCache.put(cacheKey, results[0], version);
                        }

                        std::string truncReason;
                        if (QueryLimiter::consumeTruncatedFlag(truncReason)) {
                            res["partial"] = true;
                            res["partial_reason"] = truncReason;
                        }
                    } catch (const std::exception& e) {
                        res = { {"error", "find_exception"}, {"message", e.what()} };
                    } catch (...) {
                        res = { {"error", "find_exception"}, {"message", "unknown exception"} };
                    }
                    handledDirectFind = true;
                }
                if (!handledDirectFind) {
                    auto promise = std::make_shared<std::promise<json>>();
                    auto future = promise->get_future();
                    futureCreatedUs = steadyNowUs();
                    setLifecycle("future_created_us", futureCreatedUs);
                    recordLifecycleCounter("future_created");
                    g_inflightFutures.fetch_add(1);
                    updateInflightGauges();

                    size_t findCost = std::max<size_t>(1, filter.dump().size() / 128);
                    std::string queueKey = buildQueueKey(userId, dbName, collection);
                    bool enqFind = DBTaskQueuePartitioned::instance().enqueue([userId, dbName, collection, filter, promise, readConsistency, maxStalenessMs, readFloorVersion, requestTiming, limit, offset]() mutable {
                        pacificdb::timing::setRequestTimingContext(requestTiming);
                        struct TimingScope { ~TimingScope() { pacificdb::timing::clearRequestTimingContext(); } } timingScope;
                        try {
                            DatabaseEngine::setReadContext(readConsistency, maxStalenessMs, readFloorVersion);
                            auto results = DatabaseEngine::find(userId, dbName, collection, filter, limit, offset);
                            DatabaseEngine::clearReadContext();
                            json localRes;
                            localRes["status"] = "ok";
                            localRes["count"] = results.size();
                            localRes["data"] = results;
                            localRes["sst_visibility_source"] = "lsm_queued_find";
                            if (!results.empty()) {
                                auto ver = extractVersionField(results[0]);
                                if (ver.has_value()) {
                                    localRes["returned_doc_version"] = *ver;
                                }
                            }
                            EngineMetrics::recordQuery(0, false, false);
                            trackShardOp(userId, dbName, "read", 0, 0, true);

                            std::string truncReason;
                            if (QueryLimiter::consumeTruncatedFlag(truncReason)) {
                                localRes["partial"] = true;
                                localRes["partial_reason"] = truncReason;
                            }

                            promise->set_value(localRes);
                        } catch (const std::exception& e) {
                            DatabaseEngine::clearReadContext();
                            promise->set_value(json{{"error", "find_exception"}, {"message", e.what()}});
                        } catch (...) {
                            DatabaseEngine::clearReadContext();
                            promise->set_value(json{{"error", "find_exception"}, {"message", "unknown exception"}});
                        }
                    }, DBTaskQueuePartitioned::Priority::HIGH, queueKey, findCost);

                    if (!enqFind) {
                        g_inflightFutures.fetch_sub(1);
                        updateInflightGauges();
                        recordTimeoutOriginCounter("dbq");
                        recordLifecycleCounter("future_timeout");
                        recordLifecycleCounter("timeout_triggered");
                        timeoutTriggeredUs = steadyNowUs();
                        setLifecycle("timeout_triggered_us", timeoutTriggeredUs);
                        res = { {"error", "server_busy"}, {"retry_after_ms", 100} };
                    } else if (future.wait_for(std::chrono::milliseconds(execTimeoutMs)) == std::future_status::timeout) {
                        g_inflightFutures.fetch_sub(1);
                        updateInflightGauges();
                        recordTimeoutOriginCounter("dbq");
                        recordLifecycleCounter("future_timeout");
                        recordLifecycleCounter("timeout_triggered");
                        futureResolvedUs = steadyNowUs();
                        timeoutTriggeredUs = futureResolvedUs;
                        setLifecycle("future_resolved_us", futureResolvedUs);
                        setLifecycle("timeout_triggered_us", timeoutTriggeredUs);
                        res = {
                            {"error", "execution_timeout"},
                            {"action", "find"},
                            {"timeout_ms", execTimeoutMs},
                            {"retry_after_ms", 200}
                        };
                    } else {
                        res = future.get();
                        futureResolvedUs = steadyNowUs();
                        setLifecycle("future_resolved_us", futureResolvedUs);
                        recordLifecycleCounter("future_resolved");
                        g_inflightFutures.fetch_sub(1);
                        updateInflightGauges();
                    }
                }

                if (readContextSet) {
                    DatabaseEngine::clearReadContext();
                }

                if (res.is_object() && res.contains("returned_doc_version") && res["returned_doc_version"].is_number()) {
                    try {
                        returnedDocVersion = res["returned_doc_version"].get<uint64_t>();
                        hasReturnedDocVersion = true;
                    } catch (...) {}
                }

                if (res.is_object() && res.contains("sst_visibility_source") && res["sst_visibility_source"].is_string()) {
                    sstVisibilitySource = res["sst_visibility_source"].get<std::string>();
                }

                if (!hasReturnedDocVersion && res.is_object() && res.contains("data") && res["data"].is_array()) {
                    if (!readKey.empty()) {
                        for (const auto& doc : res["data"]) {
                            if (!doc.is_object()) continue;
                            if (!doc.contains("id") || !doc["id"].is_string()) continue;
                            if (doc["id"].get<std::string>() != readKey) continue;
                            auto ver = extractVersionField(doc);
                            if (ver.has_value()) {
                                returnedDocVersion = *ver;
                                hasReturnedDocVersion = true;
                                break;
                            }
                        }
                    } else if (!res["data"].empty()) {
                        auto ver = extractVersionField(res["data"].front());
                        if (ver.has_value()) {
                            returnedDocVersion = *ver;
                            hasReturnedDocVersion = true;
                        }
                    }
                }

                if (hasReturnedDocVersion && !clientSessionId.empty() && !readKey.empty()) {
                    updateSessionFloor(clientSessionId, userId, dbName, collection, readKey, returnedDocVersion);
                }

                if (hasReturnedDocVersion) {
                    res["returned_doc_version"] = returnedDocVersion;
                }

                if (hasMinimumVisibleVersion) {
                    res["minimum_visible_version"] = minimumVisibleVersion;
                }

                if (res.is_object() && !res.contains("error") && hasMinimumVisibleVersion && isSimpleIdFind) {
                    bool floorSatisfied = hasReturnedDocVersion && returnedDocVersion >= minimumVisibleVersion;
                    long long observedVersion = hasReturnedDocVersion ? static_cast<long long>(returnedDocVersion) : -1;
                    long long expectedVersion = static_cast<long long>(minimumVisibleVersion);

                    if (!floorSatisfied) {
                        static const int retryDelayMs = [] {
                            if (const char* d = std::getenv("STRONG_READ_RETRY_DELAY_MS")) {
                                try { return std::max(1, std::stoi(d)); } catch (...) {}
                            }
                            return 12;
                        }();

                        static const int maxRetries = [] {
                            if (const char* m = std::getenv("STRONG_READ_RETRY_MAX")) {
                                try { return std::max(1, std::stoi(m)); } catch (...) {}
                            }
                            return 12;
                        }();

                        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(execTimeoutMs);
                        std::string wantedId = readKey;
                        while (!floorSatisfied && readRetryCount < static_cast<uint64_t>(maxRetries)
                               && std::chrono::steady_clock::now() < deadline) {
                            readRetryCount += 1;
                            if (!wantedId.empty()) {
                                std::string cacheKey = userId + ":" + dbName + ":" + collection + ":" + wantedId;
                                g_readCache.invalidate(cacheKey);
                                hotCacheErase(userId, dbName, collection, wantedId);
                                DatabaseEngine::invalidateReadCache(userId, dbName, collection, wantedId);
                                cacheInvalidations += 1;
                            }

                            try {
                                DatabaseEngine::setReadContext("strong", 0, expectedVersion);
                                auto retryResults = DatabaseEngine::find(userId, dbName, collection, filter, limit, offset);
                                DatabaseEngine::clearReadContext();

                                observedVersion = -1;
                                for (const auto& doc : retryResults) {
                                    if (!doc.is_object()) continue;
                                    if (!doc.contains("id") || !doc["id"].is_string()) continue;
                                    if (doc["id"].get<std::string>() != wantedId) continue;
                                    auto ver = extractVersionField(doc);
                                    if (ver.has_value()) {
                                        observedVersion = static_cast<long long>(*ver);
                                        returnedDocVersion = *ver;
                                        hasReturnedDocVersion = true;
                                    }
                                    break;
                                }

                                floorSatisfied = observedVersion >= expectedVersion;
                                if (floorSatisfied) {
                                    res["status"] = "ok";
                                    res["count"] = retryResults.size();
                                    res["data"] = retryResults;
                                    sstVisibilitySource = "lsm_authoritative_retry";
                                    break;
                                }
                            } catch (...) {
                                DatabaseEngine::clearReadContext();
                            }

                            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                        }

                        if (!floorSatisfied) {
                            res = {
                                {"error", "read_floor_not_met"},
                                {"message", "read visibility floor not met after authoritative retries"},
                                {"expected_version", expectedVersion},
                                {"observed_version", observedVersion},
                                {"retry_count", readRetryCount},
                                {"consistency", readConsistency},
                                {"consistency_semantics", consistencySemantics(readConsistency)},
                                {"retry_after_ms", 100}
                            };
                        }
                    }
                }

                if (hasMinimumVisibleVersion && hasReturnedDocVersion && returnedDocVersion < minimumVisibleVersion) {
                    readFloorGap = minimumVisibleVersion - returnedDocVersion;
                }
                if (!sstVisibilitySource.empty()) {
                    res["sst_visibility_source"] = sstVisibilitySource;
                }
            }
        }

        // ---------------- VECTOR QUERY ----------------
        else if (action == "queryVector") {
            DLOG("[SERVER] Dispatching VECTOR QUERY\n");
            std::string readConsistency = normalizeReadConsistencyMode(req.value("consistency", std::string("eventual")));
            req["consistency"] = readConsistency;
            bool strongRead = (readConsistency == "strong");
            StrongReadFenceState fenceState;
            beginReadTelemetry(readTelemetryActive, readCommitIndexStart, readLastAppliedStart, readTermStart);
            if (res.is_null() && strongRead) {
                applyStrongReadFence(req, readConsistency, fenceState, res);
                readBarrierWaitMs = fenceState.barrierWaitMs;
                readBarrierTerm = fenceState.barrierTerm;
                readBarrierCommit = fenceState.barrierCommit;
            }
            if (res.is_null() && RaftCore::instance().isEnabled() && !RaftCore::instance().isRecoveryComplete()) {
                res = {
                    {"error", "not_visible_yet"},
                    {"message", "node recovery in progress; reads are blocked until replay completes"},
                    {"consistency", readConsistency},
                    {"consistency_semantics", consistencySemantics(readConsistency)},
                    {"retry_after_ms", 100}
                };
            }
            if (!res.is_null()) {
                // Read fence failed; return error.
            } else {
            auto results = DatabaseEngine::queryVector(
                req.value("userId", "system"),
                req["dbName"],
                req["collection"],
                req
            );
            res["status"] = "ok";
            res["count"] = results.size();
            res["data"] = results;
            EngineMetrics::recordQuery(0, false, false);
            trackShardOp(req.value("userId", "system"), req.value("dbName", ""), "read", 0, 0, true);
            std::string truncReasonV;
            if (QueryLimiter::consumeTruncatedFlag(truncReasonV)) {
                res["partial"] = true;
                res["partial_reason"] = truncReasonV;
            }
            }
        }

        // ---------------- UPDATE ONE ----------------
        else if (action == "updateOne" || action == "update") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
            DLOG("[SERVER] Dispatching UPDATE_ONE\n");
            json raftMeta = buildRaftWriteMeta(req, requestId, traceId, traceParent);

            try {
                bool ok = DatabaseEngine::updateOne(
                    req.value("userId", "system"),
                    req["dbName"],
                    req["collection"],
                    req["filter"],
                    req["update"],
                    raftMeta
                );

                try {
                    std::string uid = req.value("userId", "system");
                    std::string db = req.value("dbName", "");
                    std::string coll = req.value("collection", "");
                    auto idOpt = extractFilterId(req["filter"]);
                    if (idOpt.has_value()) {
                        g_readCache.invalidate(uid + ":" + db + ":" + coll + ":" + *idOpt);
                        hotCacheErase(uid, db, coll, *idOpt);
                    }
                } catch (...) {}

                trackShardOp(req.value("userId", "system"), req.value("dbName", ""), "write", 0, 0, ok);
                res["status"] = ok ? "updated" : "not_found";
            } catch (const std::exception& e) {
                std::string err = e.what();
	                if (err == "write_not_committed") {
	                    res = {
	                        {"error", "write_not_committed"},
	                        {"message", "update did not reach quorum commit"},
	                        {"retry_after_ms", 100}
	                    };
	                } else if (err == "apply_overloaded") {
	                    static const int retryAfterMs = [] {
	                        if (const char* v = std::getenv("APPLY_OVERLOAD_RETRY_AFTER_MS")) {
	                            try { return std::max(1, std::stoi(v)); } catch (...) {}
	                        }
	                        return 25;
	                    }();
	                    res = {
	                        {"error", "apply_overloaded"},
	                        {"retryable", true},
	                        {"retry_after_ms", retryAfterMs}
	                    };
	                } else if (err == "not_leader") {
	                    res = { {"error", "not_leader"} };
	                } else {
                    res = { {"error", "update_exception"}, {"message", err} };
                }
                trackShardOp(req.value("userId", "system"), req.value("dbName", ""), "write", 0, 0, false);
            }
            }
        }

        // ---------------- DELETE ONE ----------------
        else if (action == "deleteOne") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
            DLOG("[SERVER] Dispatching DELETE_ONE\n");
            json raftMeta = buildRaftWriteMeta(req, requestId, traceId, traceParent);

            try {
                bool ok = DatabaseEngine::deleteOne(
                    req.value("userId", "system"),
                    req["dbName"],
                    req["collection"],
                    req["filter"],
                    raftMeta
                );

                try {
                    std::string uid = req.value("userId", "system");
                    std::string db = req.value("dbName", "");
                    std::string coll = req.value("collection", "");
                    auto idOpt = extractFilterId(req["filter"]);
                    if (idOpt.has_value()) {
                        g_readCache.invalidate(uid + ":" + db + ":" + coll + ":" + *idOpt);
                        hotCacheErase(uid, db, coll, *idOpt);
                    }
                } catch (...) {}

                if (ok) EngineMetrics::recordDelete();
                trackShardOp(req.value("userId", "system"), req.value("dbName", ""), "write", 0, 0, ok);
                res["status"] = ok ? "deleted" : "not_found";
            } catch (const std::exception& e) {
                std::string err = e.what();
	                if (err == "write_not_committed") {
	                    res = {
	                        {"error", "write_not_committed"},
	                        {"message", "delete did not reach quorum commit"},
	                        {"retry_after_ms", 100}
	                    };
	                } else if (err == "apply_overloaded") {
	                    static const int retryAfterMs = [] {
	                        if (const char* v = std::getenv("APPLY_OVERLOAD_RETRY_AFTER_MS")) {
	                            try { return std::max(1, std::stoi(v)); } catch (...) {}
	                        }
	                        return 25;
	                    }();
	                    res = {
	                        {"error", "apply_overloaded"},
	                        {"retryable", true},
	                        {"retry_after_ms", retryAfterMs}
	                    };
	                } else if (err == "not_leader") {
	                    res = { {"error", "not_leader"} };
	                } else {
                    res = { {"error", "delete_exception"}, {"message", err} };
                }
                trackShardOp(req.value("userId", "system"), req.value("dbName", ""), "write", 0, 0, false);
            }
            }
        }

        // ---------------- BULK / MANY WRITES ----------------
        else if (action == "insertMany" || action == "updateMany" ||
                 action == "deleteMany" || action == "bulk" ||
                 action == "bulkWrite") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
            json bulkRaftMeta = buildRaftWriteMeta(req, requestId, traceId, traceParent);

            size_t inserted = 0;
            size_t updated = 0;
            size_t deleted = 0;
            size_t failed = 0;
            json errors = json::array();

            auto recordFailure = [&](size_t index, const std::string& message) {
                ++failed;
                errors.push_back({{"index", index}, {"error", message}});
            };

            auto runInsert = [&](const json& op, size_t index) {
                try {
                    std::string uid = op.value("userId", req.value("userId", "system"));
                    std::string db = op.value("dbName", req.value("dbName", ""));
                    std::string coll = op.value("collection", req.value("collection", ""));
                    json data = op.contains("data") ? op["data"] : (op.contains("document") ? op["document"] : json::object());
                    if (db.empty() || coll.empty() || !data.is_object()) {
                        recordFailure(index, "insert requires dbName, collection, and object data");
                        return;
                    }
                    DatabaseEngine::insert(uid, db, coll, data, bulkRaftMeta);
                    EngineMetrics::recordInsert();
                    ++inserted;
                } catch (const std::exception& e) {
                    recordFailure(index, e.what());
                } catch (...) {
                    recordFailure(index, "insert failed");
                }
            };

            auto runUpdateOne = [&](const json& op, size_t index) {
                try {
                    std::string uid = op.value("userId", req.value("userId", "system"));
                    std::string db = op.value("dbName", req.value("dbName", ""));
                    std::string coll = op.value("collection", req.value("collection", ""));
                    json filter = op.value("filter", json::object());
                    json upd = op.value("update", json::object());
                    if (db.empty() || coll.empty()) {
                        recordFailure(index, "updateOne requires dbName and collection");
                        return;
                    }
                    if (DatabaseEngine::updateOne(uid, db, coll, filter, upd, bulkRaftMeta)) ++updated;
                    else recordFailure(index, "not_found");
                } catch (const std::exception& e) {
                    recordFailure(index, e.what());
                } catch (...) {
                    recordFailure(index, "updateOne failed");
                }
            };

            auto runDeleteOne = [&](const json& op, size_t index) {
                try {
                    std::string uid = op.value("userId", req.value("userId", "system"));
                    std::string db = op.value("dbName", req.value("dbName", ""));
                    std::string coll = op.value("collection", req.value("collection", ""));
                    json filter = op.value("filter", json::object());
                    if (db.empty() || coll.empty()) {
                        recordFailure(index, "deleteOne requires dbName and collection");
                        return;
                    }
                    bool delOk = DatabaseEngine::deleteOne(uid, db, coll, filter, bulkRaftMeta);
                    if (delOk) {
                        EngineMetrics::recordDelete();
                        ++deleted;
                    } else {
                        recordFailure(index, "not_found");
                    }
                } catch (const std::exception& e) {
                    recordFailure(index, e.what());
                } catch (...) {
                    recordFailure(index, "deleteOne failed");
                }
            };

            auto runUpdateMany = [&](const json& op, size_t index) {
                try {
                    std::string uid = op.value("userId", req.value("userId", "system"));
                    std::string db = op.value("dbName", req.value("dbName", ""));
                    std::string coll = op.value("collection", req.value("collection", ""));
                    json filter = op.value("filter", json::object());
                    json upd = op.value("update", json::object());
                    if (db.empty() || coll.empty()) {
                        recordFailure(index, "updateMany requires dbName and collection");
                        return;
                    }
                    auto docs = DatabaseEngine::find(uid, db, coll, filter);
                    size_t matched = 0;
                    for (const auto& doc : docs) {
                        json targetFilter = filter;
                        if (doc.contains("id")) targetFilter = json{{"id", doc["id"]}};
                        if (DatabaseEngine::updateOne(uid, db, coll, targetFilter, upd, bulkRaftMeta)) {
                            ++matched;
                        }
                    }
                    updated += matched;
                } catch (const std::exception& e) {
                    recordFailure(index, e.what());
                } catch (...) {
                    recordFailure(index, "updateMany failed");
                }
            };

            auto runDeleteMany = [&](const json& op, size_t index) {
                try {
                    std::string uid = op.value("userId", req.value("userId", "system"));
                    std::string db = op.value("dbName", req.value("dbName", ""));
                    std::string coll = op.value("collection", req.value("collection", ""));
                    json filter = op.value("filter", json::object());
                    if (db.empty() || coll.empty()) {
                        recordFailure(index, "deleteMany requires dbName and collection");
                        return;
                    }
                    auto docs = DatabaseEngine::find(uid, db, coll, filter);
                    size_t matched = 0;
                    for (const auto& doc : docs) {
                        json targetFilter = filter;
                        if (doc.contains("id")) targetFilter = json{{"id", doc["id"]}};
                        if (DatabaseEngine::deleteOne(uid, db, coll, targetFilter, bulkRaftMeta)) {
                            EngineMetrics::recordDelete();
                            ++matched;
                        }
                    }
                    deleted += matched;
                } catch (const std::exception& e) {
                    recordFailure(index, e.what());
                } catch (...) {
                    recordFailure(index, "deleteMany failed");
                }
            };

            auto runOp = [&](const json& op, size_t index) {
                std::string a = op.value("action", "");
                if (a == "insert" || a == "insertOne") {
                    runInsert(op, index);
                } else if (a == "updateOne") {
                    runUpdateOne(op, index);
                } else if (a == "updateMany") {
                    runUpdateMany(op, index);
                } else if (a == "deleteOne") {
                    runDeleteOne(op, index);
                } else if (a == "deleteMany") {
                    runDeleteMany(op, index);
                } else {
                    recordFailure(index, "unsupported bulk operation: " + a);
                }
            };

            if (action == "insertMany") {
                json docs = req.value("documents", json::array());
                if (!docs.is_array()) {
                    res = { {"error", "documents array required"} };
                } else {
                    std::string uid = req.value("userId", "system");
                    std::string db = req.value("dbName", "");
                    std::string coll = req.value("collection", "");
                    if (db.empty() || coll.empty()) {
                        res = { {"error", "insertMany requires dbName and collection"} };
                    } else {
                        std::vector<json> batch;
                        batch.reserve(docs.size());
                        for (const auto& doc : docs) batch.push_back(doc);
                        json batchResult = DatabaseEngine::insertMany(uid, db, coll, std::move(batch), bulkRaftMeta);
                        inserted = static_cast<size_t>(batchResult.value("inserted", 0));
                        failed = static_cast<size_t>(batchResult.value("failed", 0));
                        for (size_t metricIndex = 0; metricIndex < inserted; ++metricIndex) {
                            EngineMetrics::recordInsert();
                        }
                        trackShardOp(uid, db, "write", 0, 0, failed == 0);
                        res = {
                            {"status", failed == 0 ? "ok" : "partial"},
                            {"inserted", inserted},
                            {"updated", updated},
                            {"deleted", deleted},
                            {"failed", failed},
                            {"total", docs.size()},
                            {"errors", errors},
                            {"_bulkMetrics", batchResult}
                        };
                    }
                }
            } else if (action == "updateMany") {
                json op = {
                    {"action", "updateMany"},
                    {"userId", req.value("userId", "system")},
                    {"dbName", req.value("dbName", "")},
                    {"collection", req.value("collection", "")},
                    {"filter", req.value("filter", json::object())},
                    {"update", req.value("update", json::object())}
                };
                runUpdateMany(op, 0);
                res = {
                    {"status", failed == 0 ? "ok" : "partial"},
                    {"inserted", inserted},
                    {"updated", updated},
                    {"deleted", deleted},
                    {"failed", failed},
                    {"total", updated + failed},
                    {"errors", errors}
                };
            } else if (action == "deleteMany") {
                json op = {
                    {"action", "deleteMany"},
                    {"userId", req.value("userId", "system")},
                    {"dbName", req.value("dbName", "")},
                    {"collection", req.value("collection", "")},
                    {"filter", req.value("filter", json::object())}
                };
                runDeleteMany(op, 0);
                res = {
                    {"status", failed == 0 ? "ok" : "partial"},
                    {"inserted", inserted},
                    {"updated", updated},
                    {"deleted", deleted},
                    {"failed", failed},
                    {"total", deleted + failed},
                    {"errors", errors}
                };
            } else {
            // expect: { action: 'bulk'|'bulkWrite', ops: [ { action: 'insertOne', ... }, ... ] }
            auto ops = req.value("ops", json::array());

            if (!ops.is_array()) {
                res = { {"error", "ops array required"} };
            } else {
                for (size_t i = 0; i < ops.size(); ++i) {
                    runOp(ops[i], i);
                }
                res = {
                    {"status", failed == 0 ? "ok" : "partial"},
                    {"inserted", inserted},
                    {"updated", updated},
                    {"deleted", deleted},
                    {"failed", failed},
                    {"total", ops.size()},
                    {"errors", errors}
                };
            }
            }
            }
        }

        // ---------------- OP STATUS ----------------
        else if (action == "opStatus") {
            unsigned long long opId = req.value("opId", 0ULL);
            auto st = getOpStatus(opId);
            res = { {"opId", opId}, {"status", st} };
        }

        else if (action == "get_metrics") {
            // Return Prometheus-format metrics
            std::string metricsText = MetricsExporter::getMetrics();
            res = {
                {"success", true},
                {"metrics", metricsText}
            };
        }

        else if (action == "health_check") {
            // Return JSON health status
            std::string healthJson = MetricsExporter::getHealth();
            try {
                json healthData = json::parse(healthJson);
                // Add role information for leader discovery
                healthData["is_leader"] = RaftCore::instance().isLeader();
                healthData["role"] = RaftCore::instance().isLeader() ? "leader" : "follower";
                res = healthData;
            } catch (...) {
                res = {
                    {"status", "error"},
                    {"message", "Failed to parse health data"}
                };
            }
        }

        else if (action == "get_cluster_status") {
            // Return cluster role and status for client discovery
            uint64_t commitIndex = RaftCore::instance().getCommitIndex();
            uint64_t lastApplied = RaftCore::instance().getLastApplied();
            auto& walStats = WAL::getStats();
            res = {
                {"success", true},
                {"is_leader", RaftCore::instance().isLeader()},
                {"role", RaftCore::instance().isLeader() ? "leader" : "follower"},
                {"raft_enabled", RaftCore::instance().isEnabled()},
                {"raft_commit_index", commitIndex},
                {"raft_last_applied", lastApplied},
                {"raft_current_term", RaftCore::instance().getCurrentTerm()},
                {"raft_apply_queue_depth", commitIndex > lastApplied ? commitIndex - lastApplied : 0},
                {"wal_flush_latency", walStats.avgFlushLatencyMs.load()},
                {"read_barrier_wait_ms", g_lastReadBarrierWaitMs.load()},
                {"read_barrier_attempts", g_readBarrierAttempts.load()},
                {"read_barrier_failures", g_readBarrierFailures.load()},
                {"lease_epoch", g_lastReadBarrierTerm.load()}
            };
        }

        // ---------------- SPLIT SHARD ----------------
        else if (action == "split_shard") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string shardId = req.value("shardId", "");
                std::string splitKey = req.value("splitKey", "");

                if (shardId.empty()) {
                    res = { {"error", "shardId required"} };
                } else {
                    if (splitKey.empty()) {
                        splitKey = ShardManager::instance().suggestSplitKey(shardId);
                    }

                    if (splitKey.empty()) {
                        res = { {"status", "error"}, {"error", "split_key_unavailable"} };
                    } else {
                        bool ok = ShardManager::instance().splitShard(shardId, splitKey);
                        res = {
                            {"status", ok ? "ok" : "error"},
                            {"shardId", shardId},
                            {"splitKey", splitKey}
                        };
                    }
                }
            }
        }

        // ---------------- GET SHARD DETAILS ----------------
        else if (action == "get_shard_details") {
            json details;
            details["total_shards"] = ShardManager::instance().getTotalShards();
            details["shards"] = json::array();

            auto allShards = ShardManager::instance().getAllShardsInfo();
            for (const auto& shard : allShards) {
                details["shards"].push_back({
                    {"id", shard.shardId},
                    {"start_key", shard.startKey},
                    {"end_key", shard.endKey},
                    {"node_id", shard.primaryHost},
                    {"document_count", shard.documentCount},
                    {"size_bytes", shard.sizeBytes},
                    {"load_factor", shard.loadFactor},
                    {"status", shard.status == ShardInfo::Status::ACTIVE ? "active" : "inactive"}
                });
            }

            details["thresholds"] = {
                {"size_bytes", ShardManager::instance().getShardSizeThreshold()},
                {"doc_count", ShardManager::instance().getShardDocCountThreshold()},
                {"load_factor", ShardManager::instance().getShardLoadSplitThreshold()},
                {"query_latency_ms", ShardManager::instance().getShardQueryLatencyThresholdMs()},
                {"query_rps", ShardManager::instance().getShardQueryRpsThreshold()}
            };

            res = details;
        }

        // ---------------- LIST SHARDS ----------------
        else if (action == "list_shards") {
            res = ShardManager::instance().getClusterStatus();
            res["success"] = true;
            res["thresholds"] = {
                {"size_bytes", ShardManager::instance().getShardSizeThreshold()},
                {"doc_count", ShardManager::instance().getShardDocCountThreshold()},
                {"load_factor", ShardManager::instance().getShardLoadSplitThreshold()},
                {"query_latency_ms", ShardManager::instance().getShardQueryLatencyThresholdMs()},
                {"query_rps", ShardManager::instance().getShardQueryRpsThreshold()}
            };
        }

        // ============== CONFIG MANAGEMENT (RUNTIME) ==============
        else if (action == "config_get") {
            // Get a single config value or all values
            std::string key = req.value("key", "");
            if (key.empty()) {
                // Return all config (sensitive values masked)
                json allConfig;
                for (const auto& [k, v] : EnvConfig::getAll()) {
                    // Mask sensitive values
                    bool sensitive = (k.find("SECRET") != std::string::npos ||
                                       k.find("PASSWORD") != std::string::npos ||
                                       k.find("KEY") != std::string::npos ||
                                       k.find("TOKEN") != std::string::npos);
                    if (sensitive && !v.empty()) {
                        allConfig[k] = v.size() > 4 ? v.substr(0, 2) + std::string(v.size() - 4, '*') + v.substr(v.size() - 2) : "****";
                    } else {
                        allConfig[k] = v;
                    }
                }
                res = { {"success", true}, {"config", allConfig} };
            } else {
                std::string value = EnvConfig::getString(key, "");
                res = { {"success", true}, {"key", key}, {"value", value}, {"exists", EnvConfig::has(key)} };
            }
        }

        else if (action == "config_set") {
            // Set a config value at runtime
            // Note: some values require restart to take full effect
            std::string key = req.value("key", "");
            std::string value = req.value("value", "");
            if (key.empty()) {
                res = { {"success", false}, {"error", "key_required"} };
            } else {
                EnvConfig::set(key, value);
                res = {
                    {"success", true},
                    {"key", key},
                    {"value", value},
                    {"note", "Some changes require restart to take effect"}
                };
            }
        }

        else if (action == "config_reload") {
            // Reload config from .env files
            EnvConfig::reload();
            res = {
                {"success", true},
                {"message", "Configuration reloaded"},
                {"entries", static_cast<int>(EnvConfig::getAll().size())}
            };
        }

        else if (action == "config_dump") {
            // Return structured config groups
            auto storageCfg = EnvConfig::getStorageConfig();
            auto netCfg = EnvConfig::getNetworkConfig();
            auto raftCfg = EnvConfig::getRaftConfig();
            auto walCfg = EnvConfig::getWalConfig();
            auto secCfg = EnvConfig::getSecurityConfig();

            json structured;
            structured["storage"] = {
                {"data_root", storageCfg.dataRoot},
                {"wal_dir", storageCfg.walDir},
                {"sst_dir", storageCfg.sstDir},
                {"snapshot_dir", storageCfg.snapshotDir},
                {"backup_dir", storageCfg.backupDir},
                {"tmp_dir", storageCfg.tmpDir},
                {"log_dir", storageCfg.logDir},
            };
            structured["network"] = {
                {"engine_host", netCfg.engineHost},
                {"engine_port", netCfg.enginePort},
                {"raft_host", netCfg.raftHost},
                {"raft_port", netCfg.raftPort},
                {"metrics_port", netCfg.metricsPort},
            };
            structured["raft"] = {
                {"node_id", raftCfg.nodeId},
                {"peers", raftCfg.peers},
                {"is_leader", raftCfg.isLeader},
                {"election_timeout_ms", raftCfg.electionTimeoutMs},
                {"heartbeat_interval_ms", raftCfg.heartbeatIntervalMs},
                {"min_quorum_size", raftCfg.minQuorumSize},
                {"split_brain_fencer", raftCfg.splitBrainFencerEnabled},
                {"visibility_barrier", raftCfg.visibilityBarrierEnabled},
            };
            structured["wal"] = {
                {"enabled", walCfg.enabled},
                {"fsync_enabled", walCfg.fsyncEnabled},
                {"segment_size_mb", walCfg.segmentSizeMb},
                {"compression_enabled", walCfg.compressionEnabled},
                {"checksum_enabled", walCfg.checksumEnabled},
                {"group_commit_enabled", walCfg.groupCommitEnabled},
            };
            structured["security"] = {
                {"tls_enabled", secCfg.tlsEnabled},
                {"rbac_enabled", secCfg.rbacEnabled},
                {"api_keys_enabled", secCfg.apiKeysEnabled},
                {"audit_logging", secCfg.auditLoggingEnabled},
                {"encryption_enabled", secCfg.encryptionEnabled},
                {"encryption_algorithm", secCfg.encryptionAlgorithm},
                {"auth_required", secCfg.authRequired},
                {"read_only_mode", secCfg.readOnlyMode},
            };

            res = { {"success", true}, {"config", structured} };
        }

        // ============== ADMIN STATUS ==============
        else if (action == "admin_raft_status" || action == "admin_replication_status") {
            // v5.5P-R1: production Raft status for election/replication observability.
            bool isLeader = RaftCore::instance().isLeader();
            res = json::object();
            res["ok"] = true;
            res["action"] = action;
            res["nodeId"] = RaftCore::instance().getNodeId();
            res["role"] = RaftCore::instance().getRole();
            res["engineBindHost"] = std::getenv("ENGINE_BIND_HOST") ? std::getenv("ENGINE_BIND_HOST") : "0.0.0.0";
            res["raftBindHost"] = std::getenv("RAFT_BIND_HOST") ? std::getenv("RAFT_BIND_HOST") : "0.0.0.0";
            res["engineAdvertiseHost"] = std::getenv("ENGINE_ADVERTISE_HOST") ? std::getenv("ENGINE_ADVERTISE_HOST") : "";
            res["raftAdvertiseHost"] = std::getenv("RAFT_ADVERTISE_HOST") ? std::getenv("RAFT_ADVERTISE_HOST") : "";
            res["isLeader"] = isLeader;
            res["leaderEligible"] = RaftCore::instance().isLeaderEligible();
            res["currentTerm"] = RaftCore::instance().getCurrentTerm();
            res["votedFor"] = RaftCore::instance().getVotedFor();
            res["leaderId"] = RaftCore::instance().getLeaderId();
            res["commitIndex"] = RaftCore::instance().getCommitIndex();
            res["lastLogIndex"] = RaftCore::instance().getLastIndex();
            res["lastLogTerm"] = RaftCore::instance().getLastLogTerm();
            res["lastApplied"] = RaftCore::instance().getLastApplied();
            res["hasQuorum"] = RaftCore::instance().hasQuorum();
            res["raftListenerRunning"] = RaftCore::instance().isRaftListenerRunning();
            res["lastHeartbeatMs"] = RaftCore::instance().getLastHeartbeatMs();
            res["lastHeartbeatAgeMs"] = RaftCore::instance().millisSinceLastHeartbeat();
            res["peers"] = RaftCore::instance().peers();
            res["peerCount"] = RaftCore::instance().peerCount();
            res["quorumHealth"] = RaftCore::instance().getQuorumHealth();
            res["raftEnabled"] = RaftCore::instance().isEnabled();
            res["testFailpointsCompiled"] = pacificdb::test::failpointsCompiled();
            // V11.4-DIV-001: progress markers alone cannot prove state agreement. A node
            // that stepped over a failed committed entry reports healthy markers, so the
            // apply-blocked state must be visible to any operator or gate reading status.
            res["applyBlockedState"] = RaftCore::instance().applyBlockedStatus();
            res["recoveryBlocked"] = RaftCore::instance().isApplyBlocked();
            res["recoveryComplete"] = RaftCore::instance().isRecoveryComplete();
            res["strongReadsAllowed"] = RaftCore::instance().strongReadsAllowed();
            // V11.4-DIV-001 P0.6: strong-read availability also requires completed
            // recovery and no detected divergence, not just an unblocked apply.
            res["strongReadsServable"] =
                RaftCore::instance().strongReadsAllowed() && LSM::indexRecoverySettled();
            res["indexRecovery"] = LSM::indexRecoveryStatus();
            res["divergenceDetected"] = RaftCore::instance().isDivergent();
            res["repairSourceEligible"] =
                RaftCore::instance().strongReadsAllowed() && LSM::indexRecoverySettled();
            // v5.5P-R2B FD/connection leak observability
            res["raftOpenConnections"] = RaftCore::instance().getRaftOpenConnections();
            res["raftTotalAccepted"] = RaftCore::instance().getRaftTotalAccepted();
            res["raftTotalClosed"] = RaftCore::instance().getRaftTotalClosed();
            res["raftAcceptErrors"] = RaftCore::instance().getRaftAcceptErrors();
            res["raftEmfileErrors"] = RaftCore::instance().getRaftEmfileErrors();
            {
                json writeMetrics = RaftCore::instance().getWriteReplicationMetrics();
                for (auto it = writeMetrics.begin(); it != writeMetrics.end(); ++it) {
                    res[it.key()] = it.value();
                }
            }
            {
                long fdCount = -1;
                try { fdCount = (long)std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator()); } catch (...) {}
                res["processFdCount"] = fdCount;
#ifndef _WIN32
                struct rlimit rl; long fdLimit = -1;
                if (getrlimit(RLIMIT_NOFILE, &rl) == 0) fdLimit = (long)rl.rlim_cur;
                res["processFdLimit"] = fdLimit;
#endif
            }
        }
        else if (action == "admin_apply_status") {
            res = json::object();
            res["ok"] = true;
            res["action"] = "admin_apply_status";
            res["nodeId"] = RaftCore::instance().getNodeId();
            res["role"] = RaftCore::instance().getRole();
            res["commitIndex"] = RaftCore::instance().getCommitIndex();
            res["lastApplied"] = RaftCore::instance().getLastApplied();
            res["lastLogIndex"] = RaftCore::instance().getLastIndex();
            res["testFailpointsCompiled"] = pacificdb::test::failpointsCompiled();
            json writeMetrics = RaftCore::instance().getWriteReplicationMetrics();
            for (auto it = writeMetrics.begin(); it != writeMetrics.end(); ++it) {
                res[it.key()] = it.value();
            }
        }
        else if (action == "admin_logical_write_status" || action == "getWriteStatus") {
            const std::string userId = req.value("userId", "system");
            const std::string dbName = req.value("dbName", req.value("db", std::string("")));
            const std::string collection = req.value("collection", std::string(""));
            const std::string logicalWriteId = req.value("logicalWriteId", std::string(""));
            res = json::object();
            res["ok"] = false;
            res["action"] = action;
            res["nodeId"] = RaftCore::instance().getNodeId();
            res["logicalWriteId"] = logicalWriteId;
            if (dbName.empty() || collection.empty() || logicalWriteId.empty()) {
                res["error"] = "missing_db_collection_or_logicalWriteId";
            } else {
                json filter = {{"logicalWriteId", logicalWriteId}};
                size_t visibleCount = DatabaseEngine::count(userId, dbName, collection, filter);
                res["ok"] = true;
                res["visible"] = visibleCount > 0;
                res["count"] = visibleCount;
                res["state"] = visibleCount > 0 ? "APPLIED" : "NOT_FOUND";
                res["committed"] = visibleCount > 0;
                res["applied"] = visibleCount > 0;
                res["commitIndex"] = RaftCore::instance().getCommitIndex();
                res["lastApplied"] = RaftCore::instance().getLastApplied();
                if (visibleCount > 0) {
                    auto docs = DatabaseEngine::find(userId, dbName, collection, filter, 1);
                    if (!docs.empty()) {
                        const auto& doc = docs.front();
                        res["id"] = doc.value("id", logicalWriteId);
                        if (doc.contains("_logicalWritePayloadHash") && doc["_logicalWritePayloadHash"].is_string()) {
                            res["payloadHash"] = doc["_logicalWritePayloadHash"];
                        }
                        if (doc.contains("_raft_commit_index")) {
                            res["writeCommitIndex"] = doc["_raft_commit_index"];
                        }
                    }
                }
            }
        }
        else if (action == "admin_storage_visibility_check") {
            const std::string userId = req.value("userId", "system");
            const std::string dbName = req.value("dbName", req.value("db", std::string("")));
            const std::string collection = req.value("collection", std::string(""));
            const std::string runId = req.value("runId", req.value("RUN_ID", std::string("")));
            json filter = req.contains("filter") ? req["filter"] : json::object();
            if (!runId.empty() && filter.empty()) {
                filter = {{"runId", runId}};
            }
            res = json::object();
            res["ok"] = false;
            res["action"] = "admin_storage_visibility_check";
            res["nodeId"] = RaftCore::instance().getNodeId();
            res["runId"] = runId;
            if (dbName.empty() || collection.empty()) {
                res["error"] = "missing_db_or_collection";
            } else {
                // Defect V11.4-VERIFY-002: normalQueryCount was previously assigned from
                // the same DatabaseEngine::count call as visibleCount, so the divergence
                // check was x == x and could never fail. The two values must come from
                // independent paths for the comparison to mean anything.
                size_t visibleCount = DatabaseEngine::count(userId, dbName, collection, filter);
                size_t normalQueryCount = DatabaseEngine::find(userId, dbName, collection, filter, -1).size();
                const bool divergent = (visibleCount != normalQueryCount);
                res["ok"] = true;
                res["visibleCount"] = visibleCount;
                res["normalQueryCount"] = normalQueryCount;
                res["countPathsAgree"] = !divergent;
                if (divergent) res["error"] = "visibility_query_path_divergence";
                res["filter"] = filter;
                res["commitIndex"] = RaftCore::instance().getCommitIndex();
                res["lastApplied"] = RaftCore::instance().getLastApplied();
            }
        }
        else if (action == "admin_dashboard") {
            json dashboard;
            dashboard["success"] = true;
            dashboard["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();

            // System info
            json system_info;
#ifdef _WIN32
            system_info["process_id"] = GetCurrentProcessId();
#else
            system_info["process_id"] = getpid();
#endif
            system_info["uptime_seconds"] = EngineMetrics::startTime() > 0
                ? (std::time(nullptr) - EngineMetrics::startTime()) : 0;

#ifdef _WIN32
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                system_info["memory_used_mb"] = pmc.WorkingSetSize / 1024 / 1024;
                system_info["peak_memory_mb"] = pmc.PeakWorkingSetSize / 1024 / 1024;
            }
            SYSTEM_INFO sysInfo;
            GetSystemInfo(&sysInfo);
            system_info["cpu_cores"] = sysInfo.dwNumberOfProcessors;
#else
            // Linux memory info
            struct rusage usage;
            if (getrusage(RUSAGE_SELF, &usage) == 0) {
                system_info["memory_used_mb"] = usage.ru_maxrss / 1024; // Linux returns KB
                system_info["peak_memory_mb"] = usage.ru_maxrss / 1024;
            }
            system_info["cpu_cores"] = std::thread::hardware_concurrency();
#endif
            system_info["cpu_usage_percent"] = sampleSystemCpuUsagePercent();
            dashboard["system"] = system_info;

            // Cluster info
            json cluster_info;
            cluster_info["role"] = RaftCore::instance().isLeader() ? "LEADER" : "FOLLOWER";
            cluster_info["is_leader"] = RaftCore::instance().isLeader();
            cluster_info["raft_enabled"] = RaftCore::instance().isEnabled();
            const char* nodeIdEnv = std::getenv("RAFT_NODE_ID");
            cluster_info["node_id"] = nodeIdEnv ? std::string(nodeIdEnv) : "node-0";
            const char* enginePortEnv = std::getenv("ENGINE_PORT");
            cluster_info["engine_port"] = enginePortEnv ? std::stoi(enginePortEnv) : 9000;
            dashboard["cluster"] = cluster_info;

            // Connection pool
            json pool_info;
            if (g_connectionPool) {
                pool_info["active_threads"] = g_connectionPool->getActiveThreads();
                pool_info["total_threads"] = g_connectionPool->getTotalThreads();
                pool_info["queued_tasks"] = g_connectionPool->getQueuedTasks();
                pool_info["processed_total"] = g_connectionPool->getProcessedTasks();
                pool_info["queue_depth_max"] = g_connectionPool->getQueueDepthMax();
                pool_info["queue_depth_avg"] = g_connectionPool->getQueueDepthAvg();
            }
            dashboard["connection_pool"] = pool_info;

            json admission_info;
            admission_info["active_connections"] = g_activeConnections.load();
            admission_info["total_requests"] = g_totalRequestCount.load();
            admission_info["accept_queue_depth"] = g_acceptQueueDepth.load();
            admission_info["response_pending"] = g_responsePending.load();
            admission_info["inflight_futures"] = g_inflightFutures.load();
            admission_info["configured_max_connections"] = g_configuredMaxConnections.load();
            admission_info["dynamic_max_connections"] = g_dynamicMaxConnectionsGauge.load();
            admission_info["configured_queue_high_watermark"] = g_configuredQueueHighWatermark.load();
            admission_info["dynamic_queue_high_watermark"] = g_dynamicQueueHighWatermarkGauge.load();
            admission_info["connection_limit_rejects"] = g_connectionLimitRejects.load();
            admission_info["queue_high_rejects"] = g_queueHighRejects.load();
            admission_info["server_overloaded_rejects"] = g_serverOverloadedRejects.load();
            admission_info["connection_limit_waits"] = g_connectionLimitWaits.load();
            admission_info["connection_limit_wait_successes"] = g_connectionLimitWaitSuccesses.load();
            admission_info["connection_limit_wait_total_ms"] = g_connectionLimitWaitTotalMs.load();
            admission_info["connection_limit_wait_max_ms"] = g_connectionLimitWaitMaxMs.load();
            dashboard["connection_admission"] = admission_info;

            // Query metrics
            json query_info;
            auto& qm = EngineMetrics::queryMetrics();
            auto& sm = EngineMetrics::storageMetrics();
            auto& cm = EngineMetrics::cacheMetrics();
            query_info["total_queries"] = qm.totalQueries;
            query_info["total_inserts"] = sm.totalDocsInserted;
            query_info["total_deletes"] = sm.totalDocsDeleted;
            query_info["cache_hits"] = cm.hits;
            query_info["cache_misses"] = cm.misses;
            query_info["cache_hit_rate"] = cm.hitRate();
            query_info["avg_query_time_ms"] = qm.avgLatencyMs();
            query_info["slow_queries"] = qm.slowQueries;
            dashboard["queries"] = query_info;

            // Memory manager
            json memory_info;
            memory_info["max_memory_gb"] = MemoryManager::getMaxMemoryBytes() / 1024.0 / 1024.0 / 1024.0;
            memory_info["memory_limit_mb"] = MemoryManager::getMaxMemoryMB();
            memory_info["current_usage_mb"] = MemoryManager::getProcessMemory() / 1024.0 / 1024.0;
            memory_info["memory_usage_mb"] = MemoryManager::getUsedMemoryMB();
            memory_info["usage_percent"] = MemoryManager::getMemoryUsage() * 100.0;
            memory_info["backpressure_active"] = MemoryManager::shouldSlowDownWrites();
            memory_info["memory_pressure_state"] = MemoryManager::pressureStateName(MemoryManager::getPressureState());
            memory_info["bulk_rejections_total"] = MemoryManager::getBulkRejections();
            memory_info["write_rejections_total"] = MemoryManager::getWriteRejections();
            memory_info["throttle_events_total"] = MemoryManager::getThrottleEvents();
            memory_info["warning_threshold"] = MemoryManager::getWarningThreshold() * 100.0;
            memory_info["critical_threshold"] = MemoryManager::getCriticalThreshold() * 100.0;
            dashboard["memory"] = memory_info;

            // Query limits
            json limiter_info;
            limiter_info["max_query_time_sec"] = QueryLimiter::getMaxQueryTime().count();
            limiter_info["max_result_size_mb"] = QueryLimiter::getMaxResultSize() / 1024.0 / 1024.0;
            limiter_info["max_scan_rows"] = QueryLimiter::getMaxScanRows();
            dashboard["query_limits"] = limiter_info;

            // User storage
            json storage_info;
            storage_info["data_root"] = DatabaseEngine::getDataRoot();
            json users_storage = json::array();
            std::string dataRoot = DatabaseEngine::getDataRoot();
            try {
                for (const auto& entry : std::filesystem::directory_iterator(dataRoot)) {
                    if (entry.is_directory()) {
                        std::string userName = entry.path().filename().string();
                        if (userName == "system" || userName == "." || userName == "..") continue;
                        size_t userBytes = 0;
                        int dbCount = 0;
                        for (const auto& dbEntry : std::filesystem::recursive_directory_iterator(entry.path())) {
                            if (dbEntry.is_regular_file()) userBytes += dbEntry.file_size();
                            if (dbEntry.is_directory() && dbEntry.path().parent_path() == entry.path()) dbCount++;
                        }
                        json userInfo;
                        userInfo["user_id"] = userName;
                        userInfo["storage_mb"] = userBytes / 1024.0 / 1024.0;
                        userInfo["databases"] = dbCount;
                        users_storage.push_back(userInfo);
                    }
                }
            } catch (...) {}
            storage_info["users"] = users_storage;
            dashboard["storage"] = storage_info;

            res = dashboard;
        }

        // ============================================================================
        // TENANT ISOLATION MANAGEMENT (MongoDB-style multi-tenancy)
        // ============================================================================

        // ---------------- CREATE TENANT ----------------
        else if (action == "createTenant") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                std::string name = req.value("name", tenantId);
                std::string ownerUsername = req.value("ownerUsername", "admin");
                std::string ownerPassword = req.value("ownerPassword", "");
                json options = req.value("options", json::object());

                if (tenantId.empty() || ownerPassword.empty()) {
                    res = { {"error", "tenantId and ownerPassword required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().createTenant(
                        tenantId, name, ownerUsername, ownerPassword, options);
                }
            }
        }

        // ---------------- DELETE TENANT ----------------
        else if (action == "deleteTenant") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                std::string confirmedBy = req.value("confirmedBy", "");

                if (tenantId.empty() || confirmedBy.empty()) {
                    res = { {"error", "tenantId and confirmedBy required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().deleteTenant(tenantId, confirmedBy);
                }
            }
        }

        // ---------------- LIST TENANTS ----------------
        else if (action == "listTenants") {
            res = pacificdb::tenant::TenantManager::instance().listTenants();
        }

        // ---------------- GET TENANT INFO ----------------
        else if (action == "getTenant") {
            std::string tenantId = req.value("tenantId", "");
            if (tenantId.empty()) {
                res = { {"error", "tenantId required"} };
            } else {
                auto tenant = pacificdb::tenant::TenantManager::instance().getTenant(tenantId);
                if (tenant) {
                    res = tenant->toJson();
                    res["success"] = true;
                } else {
                    res = { {"success", false}, {"error", "Tenant not found"} };
                }
            }
        }

        // ---------------- UPDATE TENANT ----------------
        else if (action == "updateTenant") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                json updates = req.value("updates", json::object());

                if (tenantId.empty()) {
                    res = { {"error", "tenantId required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().updateTenant(tenantId, updates);
                }
            }
        }

        // ---------------- TENANT USER: CREATE ----------------
        else if (action == "createTenantUser") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                std::string username = req.value("username", "");
                std::string password = req.value("password", "");
                std::string roleStr = req.value("role", "tenant_readonly");
                std::string createdBy = req.value("createdBy", "");

                auto role = pacificdb::tenant::stringToTenantRole(roleStr);

                if (tenantId.empty() || username.empty() || password.empty() || createdBy.empty()) {
                    res = { {"error", "tenantId, username, password, and createdBy required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().createTenantUser(
                        tenantId, username, password, role, createdBy);
                }
            }
        }

        // ---------------- TENANT USER: DELETE ----------------
        else if (action == "deleteTenantUser") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                std::string username = req.value("username", "");
                std::string deletedBy = req.value("deletedBy", "");

                if (tenantId.empty() || username.empty() || deletedBy.empty()) {
                    res = { {"error", "tenantId, username, and deletedBy required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().deleteTenantUser(
                        tenantId, username, deletedBy);
                }
            }
        }

        // ---------------- TENANT USER: UPDATE ROLE ----------------
        else if (action == "updateTenantUserRole") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                std::string username = req.value("username", "");
                std::string roleStr = req.value("role", "");
                std::string updatedBy = req.value("updatedBy", "");

                auto role = pacificdb::tenant::stringToTenantRole(roleStr);

                if (tenantId.empty() || username.empty() || roleStr.empty() || updatedBy.empty()) {
                    res = { {"error", "tenantId, username, role, and updatedBy required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().updateTenantUserRole(
                        tenantId, username, role, updatedBy);
                }
            }
        }

        // ---------------- TENANT USER: UPDATE PASSWORD ----------------
        else if (action == "updateTenantUserPassword") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                std::string username = req.value("username", "");
                std::string newPassword = req.value("newPassword", "");
                std::string updatedBy = req.value("updatedBy", "");

                if (tenantId.empty() || username.empty() || newPassword.empty() || updatedBy.empty()) {
                    res = { {"error", "tenantId, username, newPassword, and updatedBy required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().updateTenantUserPassword(
                        tenantId, username, newPassword, updatedBy);
                }
            }
        }

        // ---------------- TENANT USER: SET DATABASE ACCESS ----------------
        else if (action == "setTenantUserDatabaseAccess") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string tenantId = req.value("tenantId", "");
                std::string username = req.value("username", "");
                std::vector<std::string> databases = req.value("databases", std::vector<std::string>{});
                std::string updatedBy = req.value("updatedBy", "");

                if (tenantId.empty() || username.empty() || updatedBy.empty()) {
                    res = { {"error", "tenantId, username, and updatedBy required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().setTenantUserDatabaseAccess(
                        tenantId, username, databases, updatedBy);
                }
            }
        }

        // ---------------- TENANT USER: LIST ----------------
        else if (action == "listTenantUsers") {
            std::string tenantId = req.value("tenantId", "");
            if (tenantId.empty()) {
                res = { {"error", "tenantId required"} };
            } else {
                res = pacificdb::tenant::TenantManager::instance().listTenantUsers(tenantId);
            }
        }

        // ---------------- TENANT: AUTHENTICATE ----------------
        else if (action == "tenantAuthenticate") {
            std::string tenantId = req.value("tenantId", "");
            std::string username = req.value("username", "");
            std::string password = req.value("password", "");
            std::string clientIP = req.value("clientIP", "unknown");

            if (tenantId.empty() || username.empty() || password.empty()) {
                res = { {"error", "tenantId, username, and password required"} };
            } else {
                res = pacificdb::tenant::TenantManager::instance().authenticateTenantUser(
                    tenantId, username, password, clientIP);
            }
        }

        // ---------------- TENANT: VALIDATE SESSION ----------------
        else if (action == "tenantValidateSession") {
            std::string token = req.value("token", "");
            if (token.empty()) {
                res = { {"error", "token required"} };
            } else {
                bool valid = pacificdb::tenant::TenantManager::instance().validateSession(token);
                if (valid) {
                    auto session = pacificdb::tenant::TenantManager::instance().getSession(token);
                    res = {
                        {"valid", true},
                        {"tenantId", session->tenantId},
                        {"username", session->username},
                        {"role", pacificdb::tenant::tenantRoleToString(session->role)}
                    };
                } else {
                    res = { {"valid", false}, {"error", "Invalid or expired session"} };
                }
            }
        }

        // ---------------- TENANT: REVOKE SESSION ----------------
        else if (action == "tenantRevokeSession") {
            std::string token = req.value("token", "");
            if (token.empty()) {
                res = { {"error", "token required"} };
            } else {
                pacificdb::tenant::TenantManager::instance().revokeSession(token);
                res = { {"success", true}, {"message", "Session revoked"} };
            }
        }

        // ---------------- TENANT: CHECK ACCESS ----------------
        else if (action == "tenantCheckAccess") {
            std::string token = req.value("token", "");
            std::string targetAction = req.value("targetAction", "");
            std::string database = req.value("database", "");
            std::string collection = req.value("collection", "");

            if (token.empty() || targetAction.empty()) {
                res = { {"error", "token and targetAction required"} };
            } else {
                res = pacificdb::tenant::TenantManager::instance().checkAccess(
                    token, targetAction, database, collection);
            }
        }

        // ---------------- TENANT: GET STATS ----------------
        else if (action == "tenantGetStats") {
            std::string token = req.value("token", "");
            if (token.empty()) {
                res = { {"error", "token required"} };
            } else {
                res = pacificdb::tenant::TenantManager::instance().getTenantStats(token);
            }
        }

        // ---------------- TENANT: GET AUDIT LOG ----------------
        else if (action == "tenantGetAuditLog") {
            std::string tenantId = req.value("tenantId", "");
            int limit = req.value("limit", 100);

            if (tenantId.empty()) {
                res = { {"error", "tenantId required"} };
            } else {
                res = pacificdb::tenant::TenantManager::instance().getTenantAuditLog(tenantId, limit);
            }
        }

        // ---------------- TENANT: CREATE DATABASE (isolated) ----------------
        else if (action == "tenantCreateDatabase") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string token = req.value("token", "");
                std::string dbName = req.value("dbName", "");
                std::string dbType = req.value("dbType", "document");

                if (token.empty() || dbName.empty()) {
                    res = { {"error", "token and dbName required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().createDatabaseInTenant(
                        token, dbName, dbType);
                }
            }
        }

        // ---------------- TENANT: DROP DATABASE (isolated) ----------------
        else if (action == "tenantDropDatabase") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string token = req.value("token", "");
                std::string dbName = req.value("dbName", "");

                if (token.empty() || dbName.empty()) {
                    res = { {"error", "token and dbName required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().dropDatabaseInTenant(token, dbName);
                }
            }
        }

        // ---------------- TENANT: LIST DATABASES (isolated) ----------------
        else if (action == "tenantListDatabases") {
            std::string token = req.value("token", "");
            if (token.empty()) {
                res = { {"error", "token required"} };
            } else {
                res = pacificdb::tenant::TenantManager::instance().listDatabasesInTenant(token);
            }
        }

        // ---------------- TENANT: CREATE COLLECTION (isolated) ----------------
        else if (action == "tenantCreateCollection") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string token = req.value("token", "");
                std::string dbName = req.value("dbName", "");
                std::string collName = req.value("collName", "");

                if (token.empty() || dbName.empty() || collName.empty()) {
                    res = { {"error", "token, dbName, and collName required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().createCollectionInTenant(
                        token, dbName, collName);
                }
            }
        }

        // ---------------- TENANT: DROP COLLECTION (isolated) ----------------
        else if (action == "tenantDropCollection") {
            if (!RaftCore::instance().isLeader()) {
                res = { {"error", "not_leader"} };
            } else {
                std::string token = req.value("token", "");
                std::string dbName = req.value("dbName", "");
                std::string collName = req.value("collName", "");

                if (token.empty() || dbName.empty() || collName.empty()) {
                    res = { {"error", "token, dbName, and collName required"} };
                } else {
                    res = pacificdb::tenant::TenantManager::instance().dropCollectionInTenant(
                        token, dbName, collName);
                }
            }
        }

        // ============== DYNAMIC NODE & SHARD MANAGEMENT ==============
        // All cluster topology changes require leader status

        // ---------------- REGISTER NODE ----------------
        else if (action == "register_node" || action == "cluster_register") {
            if (!RaftCore::instance().isLeader()) { res = {{"error","not_leader"}}; }
            else {
            std::string nodeId = req.value("nodeId", "");
            std::string host = req.value("host", "127.0.0.1");
            int port = req.value("port", 9000);

            if (nodeId.empty()) {
                res = { {"error", "nodeId required"} };
            } else {
                ClusterNode node;
                node.nodeId = nodeId;
                node.host = host;
                node.port = port;
                node.isAlive = true;
                node.lastHeartbeat = std::chrono::steady_clock::now();

                bool ok = true;
                try {
                    ShardManager::instance().registerNode(node);
                } catch (...) {
                    ok = false;
                }
                res = {
                    {"success", ok},
                    {"nodeId", nodeId},
                    {"host", host},
                    {"port", port}
                };
            }
            } // leader check
        }

        // ---------------- DEREGISTER NODE ----------------
        else if (action == "deregister_node" || action == "cluster_deregister") {
            if (!RaftCore::instance().isLeader()) { res = {{"error","not_leader"}}; }
            else {
            std::string nodeId = req.value("nodeId", "");

            if (nodeId.empty()) {
                res = { {"error", "nodeId required"} };
            } else {
                bool ok = true;
                try {
                    ShardManager::instance().unregisterNode(nodeId);
                } catch (...) {
                    ok = false;
                }
                res = { {"success", ok}, {"nodeId", nodeId} };
            }
            } // leader check
        }

        // ---------------- CREATE SHARD ----------------
        else if (action == "create_shard") {
            if (!RaftCore::instance().isLeader()) { res = {{"error","not_leader"}}; }
            else {
            std::string shardId = req.value("shardId", "");
            std::string startKey = req.value("startKey", "");
            std::string endKey = req.value("endKey", "");
            std::string primaryHost = req.value("primaryHost", "127.0.0.1:9000");

            ShardInfo info;
            info.shardId = shardId.empty() ? ("shard-manual-" + std::to_string(std::time(nullptr))) : shardId;
            info.startKey = startKey;
            info.endKey = endKey;
            info.primaryHost = primaryHost;
            info.status = ShardInfo::Status::ACTIVE;
            info.weight = 1.0;
            info.createdAt = std::chrono::steady_clock::now();
            info.lastActivity = info.createdAt;

            bool ok = ShardManager::instance().createShard(info);
            res = {
                {"success", ok},
                {"shardId", info.shardId},
                {"startKey", startKey},
                {"endKey", endKey}
            };
            } // leader check
        }

        // ---------------- MIGRATE SHARD (MANUAL) ----------------
        else if (action == "migrate_shard") {
            if (!RaftCore::instance().isLeader()) res = {{"error", "not_leader"}};
            else {
                const std::string shardId = req.value("shardId", "");
                const std::string targetNode = req.value("targetNode", "");
                const bool ok = !shardId.empty() && !targetNode.empty() &&
                    ShardManager::instance().migrateShard(shardId, targetNode);
                res = {{"success", ok}, {"shardId", shardId}, {"targetNode", targetNode}};
            }
        }

        // ---------------- REBALANCE SHARDS ----------------
        else if (action == "rebalance_shards") {
            if (!RaftCore::instance().isLeader()) { res = {{"error","not_leader"}}; }
            else {
            bool ok = ShardManager::instance().rebalanceShards();
            auto status = ShardManager::instance().getClusterStatus();
            res = {
                {"success", ok},
                {"status", "rebalanced"},
                {"total_shards", status.value("total_shards", 0)},
                {"active_nodes", status.value("active_nodes", 0)}
            };
            } // leader check
        }

        // ---------------- ROUTE INSPECTION ----------------
        else if (action == "cluster_route" || action == "route_key") {
            std::string key = req.value("key", "");
            std::string operation = req.value("operation", "read");
            if (key.empty()) {
                res = {{"error", "key required"}};
            } else {
                std::string shardId = ShardManager::instance().getShardForKey(key);
                ShardInfo info = ShardManager::instance().getShardInfo(shardId);
                std::string selected = info.primaryHost;

                res = {
                    {"success", !shardId.empty()},
                    {"key", key},
                    {"operation", operation},
                    {"shard_id", shardId},
                    {"selected_host", selected},
                    {"primary_host", info.primaryHost},
                    {"replicas", info.replicas},
                    {"load_factor", info.loadFactor},
                    {"avg_latency_ms", info.avgLatencyMs},
                    {"error_ratio", info.totalOperations > 0 ? (double)info.totalErrors / (double)info.totalOperations : 0.0}
                };
            }
        }

        // ---------------- STORAGE MAINTENANCE ----------------
        else if (action == "storage_stats" || action == "wal_status" ||
                 action == "admin_wal_status" || action == "admin_lsm_status" ||
                 action == "admin_compaction_status" || action == "admin_replay_check" ||
                 action == "admin_storage_verify" ||
                 action == "memtable_status" || action == "sst_status" ||
                 action == "storage_flush" || action == "flush" ||
                 action == "storage_compact" || action == "compact" ||
                 action == "storage_repair" || action == "repair" ||
                 action == "verify_integrity" ||
                 action == "lsm_metrics" || action == "lsmProfile" || action == "lsm_profile") {
            auto collectStorageFiles = []() {
                json stats;
                stats["data_root"] = DatabaseEngine::getDataRoot();
                stats["total_bytes"] = 0ULL;
                stats["sst_files"] = 0;
                stats["sst_bytes"] = 0ULL;
                stats["wal_files"] = 0;
                stats["wal_bytes"] = 0ULL;
                stats["other_files"] = 0;

                try {
                    std::filesystem::path root(DatabaseEngine::getDataRoot());
                    if (std::filesystem::exists(root)) {
                        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                            if (!entry.is_regular_file()) continue;
                            uintmax_t size = 0;
                            try { size = entry.file_size(); } catch (...) { size = 0; }
                            stats["total_bytes"] = stats.value("total_bytes", 0ULL) + size;
                            const std::string ext = entry.path().extension().string();
                            if (ext == ".sst") {
                                stats["sst_files"] = stats.value("sst_files", 0) + 1;
                                stats["sst_bytes"] = stats.value("sst_bytes", 0ULL) + size;
                            } else if (ext == ".wal") {
                                stats["wal_files"] = stats.value("wal_files", 0) + 1;
                                stats["wal_bytes"] = stats.value("wal_bytes", 0ULL) + size;
                            } else {
                                stats["other_files"] = stats.value("other_files", 0) + 1;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    stats["scan_error"] = e.what();
                } catch (...) {
                    stats["scan_error"] = "unknown";
                }
                return stats;
            };

            auto walJson = []() {
                auto& walStats = WAL::getStats();
                return json{
                    {"success", true},
                    {"entries_written", walStats.entriesWritten.load()},
                    {"entries_fsynced", walStats.entriesFsynced.load()},
                    {"batches_committed", walStats.batchesCommitted.load()},
                    {"bytes_written", walStats.bytesWritten.load()},
                    {"pending_entries", WAL::getPendingCount()},
                    {"avg_flush_latency_ms", walStats.avgFlushLatencyMs.load()},
                    {"bytes_before_compression", walStats.bytesBeforeCompression.load()},
                    {"bytes_after_compression", walStats.bytesAfterCompression.load()},
                    {"compressed_batches", walStats.compressedBatches.load()},
                    {"uncompressed_batches", walStats.uncompressedBatches.load()},
                    {"segments_created", walStats.segmentsCreated.load()},
                    {"segments_compacted", walStats.segmentsCompacted.load()},
                    {"active_segments", walStats.activeSegments.load()},
                    {"current_lsn", WAL::getCurrentLSN()},
                    {"segment_count", WAL::getSegmentCount()}
                };
            };

            if (action == "wal_status" || action == "admin_wal_status") {
                res = walJson();
                res["action"] = action;
                res["checksum_validation_expected"] = true;
                res["replay_rejects_corrupt_records"] = true;
            } else if (action == "memtable_status") {
                res = LSM::getRuntimeStats();
            } else if (action == "lsmProfile" || action == "lsm_profile" || action == "lsm_metrics" || action == "admin_lsm_status") {
                res = LSM::getLsmMetrics();
                res["action"] = action;
                res["runtime"] = LSM::getRuntimeStats();
            } else if (action == "admin_compaction_status") {
                res = LSM::getRuntimeStats();
                res["success"] = true;
                res["action"] = action;
                res["lsm_metrics"] = LSM::getLsmMetrics();
            } else if (action == "sst_status") {
                json files = collectStorageFiles();
                res = {
                    {"success", true},
                    {"data_root", files.value("data_root", "")},
                    {"sst_files", files.value("sst_files", 0)},
                    {"sst_bytes", files.value("sst_bytes", 0ULL)},
                    {"sst_mb", files.value("sst_bytes", 0ULL) / 1024.0 / 1024.0}
                };
            } else if (action == "storage_stats") {
                json files = collectStorageFiles();
                res = {
                    {"success", true},
                    {"data_root", files.value("data_root", "")},
                    {"total_bytes", files.value("total_bytes", 0ULL)},
                    {"total_mb", files.value("total_bytes", 0ULL) / 1024.0 / 1024.0},
                    {"sst_files", files.value("sst_files", 0)},
                    {"sst_mb", files.value("sst_bytes", 0ULL) / 1024.0 / 1024.0},
                    {"wal_files", files.value("wal_files", 0)},
                    {"wal_mb", files.value("wal_bytes", 0ULL) / 1024.0 / 1024.0},
                    {"memtables", LSM::getRuntimeStats()},
                    {"wal", walJson()},
                    {"memory", {
                        {"current_usage_mb", MemoryManager::getProcessMemory() / 1024.0 / 1024.0},
                        {"usage_percent", MemoryManager::getMemoryUsage() * 100.0},
                        {"backpressure_active", MemoryManager::shouldSlowDownWrites()}
                    }}
                };
            } else if (action == "storage_flush" || action == "flush") {
                json before = LSM::getRuntimeStats();
                LSM::forceFlush();
                res = {
                    {"success", true},
                    {"message", "Memtables flushed to SSTables."},
                    {"before", before},
                    {"after", LSM::getRuntimeStats()},
                    {"wal", walJson()}
                };
            } else if (action == "storage_compact" || action == "compact") {
                int compactedCollections = 0;
                try {
                    std::filesystem::path root(DatabaseEngine::getDataRoot());
                    if (std::filesystem::exists(root)) {
                        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                            if (!entry.is_directory()) continue;
                            if (entry.path().extension() != ".lsm") continue;
                            const std::string collection = entry.path().stem().string();
                            const std::string dbName = entry.path().parent_path().filename().string();
                            const std::string userId = entry.path().parent_path().parent_path().filename().string();
                            if (!userId.empty() && !dbName.empty() && !collection.empty()) {
                                LSM::compact(userId, dbName, collection);
                                compactedCollections++;
                            }
                        }
                    }
                    res = {
                        {"success", true},
                        {"message", "Compaction checked all SSTable collections."},
                        {"collections_checked", compactedCollections},
                        {"storage", collectStorageFiles()}
                    };
                } catch (const std::exception& e) {
                    res = { {"success", false}, {"error", "compact_failed"}, {"message", e.what()} };
                }
            } else if (action == "admin_replay_check") {
                res = {
                    {"success", true},
                    {"action", action},
                    {"wal", walJson()},
                    {"storage", collectStorageFiles()},
                    {"message", "Replay status is exposed for audit; destructive crash replay tests remain external harness gates."}
                };
            } else if (action == "verify_integrity" || action == "admin_storage_verify") {
                // Defect V11.4-VERIFY-001: this action previously shared the repair
                // handler. It rebuilt a bloom filter for every SST and then reported
                // "Storage integrity verified", so it compared nothing, could never
                // fail, and mutated storage under a READ permission.
                //
                // It now performs a real base/index comparison. LSM::validateColumnIndexes
                // rebuilds the expected index from the base documents and diffs it against
                // the persisted index, reporting missing and stale entries per field. This
                // handler is read-only and FAILS CLOSED: success is true only when every
                // collection verified cleanly.
                try {
                    const std::string vUser = req.value("userId", std::string(""));
                    const std::string vDb = req.value("dbName", req.value("db", std::string("")));
                    const std::string vColl = req.value("collection", std::string(""));

                    // Each verified collection is identified by its <collection>.idx
                    // directory under the LSM root, which LSM::init sets to the data root.
                    std::vector<std::array<std::string, 3>> targets;
                    if (!vUser.empty() && !vDb.empty() && !vColl.empty()) {
                        targets.push_back({vUser, vDb, vColl});
                    } else {
                        std::filesystem::path root(DatabaseEngine::getDataRoot());
                        if (std::filesystem::exists(root)) {
                            for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                                if (!entry.is_directory()) continue;
                                const auto& p = entry.path();
                                if (p.extension() != ".idx") continue;
                                const auto dbDir = p.parent_path();
                                const auto userDir = dbDir.parent_path();
                                if (dbDir.empty() || userDir.empty()) continue;
                                targets.push_back({userDir.filename().string(),
                                                   dbDir.filename().string(),
                                                   p.stem().string()});
                            }
                        }
                    }

                    json collections = json::array();
                    size_t missingTotal = 0;
                    size_t staleTotal = 0;
                    size_t errorTotal = 0;
                    for (const auto& t : targets) {
                        json v = LSM::validateColumnIndexes(t[0], t[1], t[2]);
                        const std::string vStatus = v.value("status", std::string("error"));
                        const size_t missing = v.value("missingEntries", static_cast<size_t>(0));
                        const size_t stale = v.value("staleEntries", static_cast<size_t>(0));
                        missingTotal += missing;
                        staleTotal += stale;
                        if (vStatus == "error") errorTotal++;
                        collections.push_back({
                            {"userId", t[0]}, {"database", t[1]}, {"collection", t[2]},
                            {"status", vStatus},
                            {"missingEntries", missing},
                            {"staleEntries", stale},
                            {"fields", v.value("fields", json::array())}
                        });
                    }

                    const bool consistent = (missingTotal == 0 && staleTotal == 0 && errorTotal == 0);
                    res = {
                        {"success", consistent},
                        {"action", action},
                        {"verification", "base_index_comparison"},
                        {"readOnly", true},
                        {"collectionsVerified", collections.size()},
                        {"missingIndexEntries", missingTotal},
                        {"staleIndexEntries", staleTotal},
                        {"collectionsInError", errorTotal},
                        {"baseIndexConsistent", consistent},
                        {"collections", collections},
                        {"message", consistent
                            ? "Base data and indexes are consistent."
                            : "Base/index divergence detected; storage is NOT verified."}
                    };
                    if (!consistent) res["error"] = "base_index_divergence";
                } catch (const std::exception& e) {
                    // Fail closed: an exception is never a clean verification.
                    res = { {"success", false}, {"error", "verification_failed"}, {"message", e.what()} };
                }
            } else if (action == "storage_repair" || action == "repair") {
                int sstChecked = 0;
                int sstReindexed = 0;
                try {
                    std::filesystem::path root(DatabaseEngine::getDataRoot());
                    if (std::filesystem::exists(root)) {
                        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                            if (!entry.is_regular_file()) continue;
                            if (entry.path().extension() != ".sst") continue;
                            sstChecked++;
                            LSM::buildBloomForSST(entry.path().string());
                            sstReindexed++;
                        }
                    }
                    res = {
                        {"success", true},
                        {"message", "Storage repair completed."},
                        {"repairPerformed", true},
                        {"sst_checked", sstChecked},
                        {"sst_reindexed", sstReindexed},
                        {"storage", collectStorageFiles()}
                    };
                } catch (const std::exception& e) {
                    res = { {"success", false}, {"error", "repair_failed"}, {"message", e.what()} };
                }
            }
        }

        else {
            res = {
                {"error", "Unknown action"},
                {"action", action}
            };
        }
    }
    } catch (const std::exception& e) {
        std::cerr << "[SERVER] Request execution exception (action=" << action << "): " << e.what() << std::endl;
        res = {
            {"error", "execution_exception"},
            {"action", action},
            {"message", e.what()}
        };
    } catch (...) {
        std::cerr << "[SERVER] Request execution unknown exception (action=" << action << ")" << std::endl;
        res = {
            {"error", "execution_exception"},
            {"action", action},
            {"message", "unknown exception"}
        };
    }

    auto execEnd = std::chrono::steady_clock::now();
    double execMs = std::chrono::duration<double, std::milli>(execEnd - execStart).count();
    if (hardReqLog) {
        std::cout << "[EXEC END] " << reqId << " status="
                  << (res.contains("error") ? "error" : "ok")
                  << " exec_ms=" << execMs << std::endl;
    }

    if (!authRejected && res.is_object()) {
        const bool failed = res.contains("error") ||
            (res.contains("status") && res["status"].is_string() && res["status"] == "error");
        const bool explicitlyRequested = req.is_object() && req.contains("includeTelemetry") &&
            req["includeTelemetry"].is_boolean() && req["includeTelemetry"].get<bool>();
        if (failed || explicitlyRequested) activateTelemetry();
    }

    if (res.is_object()) {
        res["requestId"] = requestId;
        res["trace_id"] = traceId;
        if (!traceParent.empty()) {
            res["traceparent"] = traceParent;
        }

        std::string mode = "eventual";
        if (!req.is_discarded() && req.contains("consistency") && req["consistency"].is_string()) {
            mode = normalizeReadConsistencyMode(req["consistency"].get<std::string>());
        }
        res["consistency_mode"] = consistencyModeApiLabel(mode);
        res["consistency_semantics"] = consistencySemantics(mode);
        res["isLeader"] = RaftCore::instance().isLeader();
        res["term"] = RaftCore::instance().getCurrentTerm();
        if (!authRejected && res.contains("error")) activateTelemetry();

        if (readTelemetryActive) {
            readCommitIndexFinish = RaftCore::instance().getCommitIndex();
            readLastAppliedFinish = RaftCore::instance().getLastApplied();
            readTermFinish = RaftCore::instance().getCurrentTerm();
            if (readBarrierTerm == 0) readBarrierTerm = readTermFinish;
            if (readBarrierCommit == 0) readBarrierCommit = readCommitIndexFinish;
            if (mode == "strong" && readBarrierTerm > 0 && readTermFinish != readBarrierTerm && !res.contains("error")) {
                res = {
                    {"error", "term_changed_during_read"},
                    {"message", "leader term changed before the strong read response was finalized; retry"},
                    {"barrier_term", readBarrierTerm},
                    {"current_term", readTermFinish},
                    {"consistency", "strong"},
                    {"consistency_semantics", consistencySemantics(mode)},
                    {"retry_after_ms", 50}
                };
            }
        }

        res["requestId"] = requestId;
        res["trace_id"] = traceId;
        if (!traceParent.empty()) {
            res["traceparent"] = traceParent;
        }
        res["consistency_mode"] = consistencyModeApiLabel(mode);
        res["consistency_semantics"] = consistencySemantics(mode);
        res["isLeader"] = RaftCore::instance().isLeader();
        res["term"] = RaftCore::instance().getCurrentTerm();
        if (!clientSessionId.empty()) {
            res["client_session_id"] = clientSessionId;
            res["_session"]["client_session_id"] = clientSessionId;
            if (hasClientLastSeen) {
                res["last_seen_version"] = clientLastSeenVersion;
                res["_session"]["last_seen_version"] = clientLastSeenVersion;
            }
            if (hasMinimumVisibleVersion) {
                res["minimum_visible_version"] = minimumVisibleVersion;
                res["_session"]["minimum_visible_version"] = minimumVisibleVersion;
            }
        }

        if (telemetryActive) {
            const uint64_t responseCommitIndex = RaftCore::instance().getCommitIndex();
            const uint64_t responseLastApplied = RaftCore::instance().getLastApplied();
            auto& responseWalStats = WAL::getStats();
            res["_raft"] = {
            {"is_leader", RaftCore::instance().isLeader()},
            {"term", RaftCore::instance().getCurrentTerm()},
            {"commit_index", responseCommitIndex},
            {"last_index", RaftCore::instance().getLastIndex()},
            {"last_applied", responseLastApplied},
            {"apply_queue_depth", responseCommitIndex > responseLastApplied ? responseCommitIndex - responseLastApplied : 0}
        };
            res["_debug_metrics"] = {
            {"raft_commit_index", responseCommitIndex},
            {"raft_last_applied", responseLastApplied},
            {"raft_current_term", RaftCore::instance().getCurrentTerm()},
            {"raft_apply_queue_depth", responseCommitIndex > responseLastApplied ? responseCommitIndex - responseLastApplied : 0},
            {"wal_flush_latency", responseWalStats.avgFlushLatencyMs.load()},
            {"read_barrier_wait_ms", readBarrierWaitMs >= 0 ? readBarrierWaitMs : g_lastReadBarrierWaitMs.load()},
            {"lease_epoch", readBarrierTerm ? readBarrierTerm : g_lastReadBarrierTerm.load()}
        };

            if (readTelemetryActive) {
                res["_debug_metrics"]["commit_index_at_read_start"] = readCommitIndexStart;
            res["_debug_metrics"]["commit_index_at_read_finish"] = readCommitIndexFinish;
            res["_debug_metrics"]["last_applied_at_read"] = readLastAppliedFinish;
            res["_debug_metrics"]["raft_term"] = readTermFinish;
            res["_debug_metrics"]["term_at_read_start"] = readTermStart;
            res["_debug_metrics"]["term_at_read_end"] = readTermFinish;
            res["_debug_metrics"]["leader_epoch"] = readBarrierTerm;
            res["_debug_metrics"]["barrier_wait_ms"] = readBarrierWaitMs >= 0 ? readBarrierWaitMs : g_lastReadBarrierWaitMs.load();
            res["_debug_metrics"]["apply_queue_depth"] = readCommitIndexFinish > readLastAppliedFinish ? readCommitIndexFinish - readLastAppliedFinish : 0;
            res["_debug_metrics"]["wal_flush_latency"] = responseWalStats.avgFlushLatencyMs.load();
            res["_debug_metrics"]["returned_doc_version"] = hasReturnedDocVersion ? json(returnedDocVersion) : json(nullptr);
            res["_debug_metrics"]["minimum_visible_version"] = hasMinimumVisibleVersion ? json(minimumVisibleVersion) : json(nullptr);
            res["_debug_metrics"]["required_floor_version"] = hasMinimumVisibleVersion ? json(minimumVisibleVersion) : json(nullptr);
            res["_debug_metrics"]["read_floor_gap"] = readFloorGap;
            res["_debug_metrics"]["cache_invalidations"] = cacheInvalidations;
            res["_debug_metrics"]["cache_hit"] = readCacheHit;
            res["_debug_metrics"]["retry_count"] = readRetryCount;
            res["_debug_metrics"]["SST_visibility_source"] = sstVisibilitySource.empty() ? json(nullptr) : json(sstVisibilitySource);
                if (!clientSessionId.empty()) {
                    res["_debug_metrics"]["client_session_id"] = clientSessionId;
                }
                if (hasClientLastSeen) {
                    res["_debug_metrics"]["last_seen_version"] = clientLastSeenVersion;
                }
            }
            if (futureCreatedUs > 0) lifecycle["future_created_us"] = futureCreatedUs;
            if (futureResolvedUs > 0) lifecycle["future_resolved_us"] = futureResolvedUs;
            if (responseSerializedUs > 0) lifecycle["response_serialized_us"] = responseSerializedUs;
            if (responseSendStartedUs > 0) lifecycle["response_send_started_us"] = responseSendStartedUs;
            if (responseSendCompletedUs > 0) lifecycle["response_send_completed_us"] = responseSendCompletedUs;
            if (socketClosedUs > 0) lifecycle["socket_closed_us"] = socketClosedUs;
            if (futureCreatedUs > 0 && futureResolvedUs > 0) {
                const uint64_t taskExecToFutureResolveUs = futureResolvedUs > futureCreatedUs ? futureResolvedUs - futureCreatedUs : 0;
                lifecycle["task_exec_to_future_resolve_us"] = taskExecToFutureResolveUs;
                MetricsExporter::recordCustomMetric("pacificdb_pipeline_task_exec_to_future_resolve_us", static_cast<double>(taskExecToFutureResolveUs));
            }
            if (futureResolvedUs > 0 && responseSendStartedUs > 0) {
                const uint64_t futureResolveToResponseUs = responseSendStartedUs > futureResolvedUs ? responseSendStartedUs - futureResolvedUs : 0;
                lifecycle["future_resolve_to_response_us"] = futureResolveToResponseUs;
                MetricsExporter::recordCustomMetric("pacificdb_pipeline_future_resolve_to_response_us", static_cast<double>(futureResolveToResponseUs));
                lifecycle["future_resolved_to_response_send_started_us"] = futureResolveToResponseUs;
            }
            if (responseSendStartedUs > 0 && responseSendCompletedUs > 0) {
                lifecycle["response_send_started_to_response_send_completed_us"] = responseSendCompletedUs - responseSendStartedUs;
            }
            if (responseSendCompletedUs > 0 && socketClosedUs > 0) {
                lifecycle["response_send_completed_to_socket_closed_us"] = socketClosedUs - responseSendCompletedUs;
            }
            res["_engineTrace"] = {
            {"request_id", requestId},
            {"trace_id", traceId},
            {"action", action},
            {"node_role", RaftCore::instance().isLeader() ? "leader" : "follower"},
            {"timings_ms", {
                {"queue", queueWaitMs},
                {"read", readMs},
                {"parse", parseMs},
                {"exec", execMs}
            }},
            {"timings_us", {
                {"accept_wait", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::AcceptWait)},
                {"queue_wait", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::QueueWait)},
                {"parse", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::Parse)},
                {"auth", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::Auth)},
                {"wal_append", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::WalAppend)},
                {"wal_fsync", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::WalFsync)},
                {"lock_wait", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::LockWait)},
                {"memtable_insert", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::MemtableInsert)},
                {"replication", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::Replication)},
                {"response_send", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::ResponseSend)},
                {"db_queue_wait", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::DbQueueWait)},
                {"conn_pool_wait", pacificdb::timing::contextTotalUs(requestTiming, pacificdb::timing::Stage::ConnPoolWait)}
            }},
                {"lifecycle_us", lifecycle}
            };
        }
    }

    // Use an allowlist so future response decorations cannot silently leak
    // internals to an unauthenticated or unauthorized caller.
    if (authRejected) {
        json refusal = json::object();
        for (const char* key : {"error", "message", "action"}) {
            if (res.contains(key)) refusal[key] = res[key];
        }
        res = std::move(refusal);
    }

    if (deterministicLogEnabled()) {
        json logEntry = json::object();
        logEntry["seq"] = g_deterministicSeq.fetch_add(1) + 1;
        logEntry["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        logEntry["request_id"] = requestId;
        logEntry["trace_id"] = traceId;
        logEntry["action"] = action;
        if (req.is_object()) {
            logEntry["request"] = sanitizeDeterministicPayload(req);
        }
        logEntry["response"] = sanitizeDeterministicPayload(res);

        json readMetrics = json::object();
        if (readTelemetryActive) {
            uint64_t commitFinish = RaftCore::instance().getCommitIndex();
            uint64_t lastAppliedFinish = RaftCore::instance().getLastApplied();
            uint64_t termFinish = RaftCore::instance().getCurrentTerm();
            readMetrics["commit_index_at_read_start"] = readCommitIndexStart;
            readMetrics["commit_index_at_read_finish"] = commitFinish;
            readMetrics["last_applied_at_read"] = lastAppliedFinish;
            readMetrics["raft_term"] = termFinish;
            readMetrics["term_at_read_start"] = readTermStart;
            readMetrics["term_at_read_end"] = termFinish;
            readMetrics["leader_epoch"] = readBarrierTerm ? readBarrierTerm : g_lastReadBarrierTerm.load();
            readMetrics["barrier_wait_ms"] = readBarrierWaitMs >= 0 ? readBarrierWaitMs : g_lastReadBarrierWaitMs.load();
            readMetrics["apply_queue_depth"] = commitFinish > lastAppliedFinish ? commitFinish - lastAppliedFinish : 0;
            readMetrics["wal_flush_latency"] = WAL::getStats().avgFlushLatencyMs.load();
            readMetrics["returned_doc_version"] = hasReturnedDocVersion ? json(returnedDocVersion) : json(nullptr);
            readMetrics["minimum_visible_version"] = hasMinimumVisibleVersion ? json(minimumVisibleVersion) : json(nullptr);
            readMetrics["required_floor_version"] = hasMinimumVisibleVersion ? json(minimumVisibleVersion) : json(nullptr);
            readMetrics["read_floor_gap"] = readFloorGap;
            readMetrics["cache_invalidations"] = cacheInvalidations;
            readMetrics["cache_hit"] = readCacheHit;
            readMetrics["retry_count"] = readRetryCount;
            readMetrics["SST_visibility_source"] = sstVisibilitySource.empty() ? json(nullptr) : json(sstVisibilitySource);
            if (!clientSessionId.empty()) readMetrics["client_session_id"] = clientSessionId;
            if (hasClientLastSeen) readMetrics["last_seen_version"] = clientLastSeenVersion;
        }
        if (!readMetrics.empty()) {
            logEntry["read_metrics"] = readMetrics;
        }

        if (res.is_object() && res.contains("error")) {
            logEntry["error"] = res["error"];
        }

        appendDeterministicLog(logEntry);
    }

    responseSerializedUs = steadyNowUs();
    recordLifecycleCounter("response_serialized");
    std::string out;
    if (binaryWireV2) {
        const auto packed = json::to_msgpack(res);
        std::uint32_t framedLength = static_cast<std::uint32_t>(packed.size());
        if (res.is_object() && res.contains("error")) framedLength |= 0x80000000U;
        const std::uint32_t networkLength = htonl(framedLength);
        out.assign(kEngineWireV2Magic, sizeof(kEngineWireV2Magic));
        out.append(reinterpret_cast<const char*>(&networkLength), sizeof(networkLength));
        out.append(reinterpret_cast<const char*>(packed.data()), packed.size());
    } else {
        // Legacy clients retain newline JSON framing during migration.
        out = res.dump();
        out.push_back('\n');
    }
    if (res.is_object() && res.contains("error")) {
        try {
            if (res["error"].is_string() && res["error"].get<std::string>() == "server_busy") {
                recordPipelineCounter("pacificdb_pipeline_rejected_requests_total");
            }
            recordRejectReasonFromResponse(res);
        } catch (...) {}
    }

    const std::string responseClass = classifyResponseClass(res);
    recordPipelineCounter("pacificdb_pipeline_responses_total");
    recordPipelineCounter("pacificdb_pipeline_response_" + responseClass + "_total");

    // Send response with proper error handling
    recordLifecycleCounter("response_send_started");
    responseSendStartedUs = steadyNowUs();
    g_responsePending.fetch_add(1);
    updateInflightGauges();
    auto responseStart = std::chrono::steady_clock::now();
    int sendFlags = 0;
#ifdef __linux__
#ifdef MSG_ZEROCOPY
    if (g_socketZeroCopy) {
        sendFlags |= MSG_ZEROCOPY;
    }
#endif
#endif

    auto sendResult = sendTrackedPayload(sock, out, sendFlags);
    if (sendResult.partial) recordPipelineCounter("pacificdb_pipeline_response_send_partial_total");
    if (sendResult.eagain) recordPipelineCounter("pacificdb_pipeline_response_send_eagain_total");
    if (!sendResult.ok) {
        recordPipelineCounter("pacificdb_pipeline_response_send_failed_total");
        std::cerr << "[SERVER] Failed to send response, error: " << sendResult.errorCode << std::endl;
        recordTimeoutOriginCounter("response_send");
    }
    recordLifecycleCounter("response_send_completed");
    responseSendCompletedUs = steadyNowUs();

    // KEEPALIVE DECISION: keep connection open or close gracefully
    if (_kaMX > 1 && (_kaI + 1) < _kaMX && sendResult.ok) {
        // Request-boundary '\n' is already appended to the payload above.
        recordLifecycleCounter("keepalive_continue");
        socketClosedUs = 0;
    } else {
        // Graceful close: shutdown write side first, then close
#ifdef _WIN32
        shutdown(sock, SD_SEND);
#else
        shutdown(sock, SHUT_WR);
#endif
        recordLifecycleCounter("socket_closed");
        socketClosedUs = steadyNowUs();
        _kaSockClosed = true;
    }

    auto responseEnd = std::chrono::steady_clock::now();
    double responseMs = std::chrono::duration<double, std::milli>(responseEnd - responseStart).count();
    pacificdb::timing::recordStage(pacificdb::timing::Stage::ResponseSend,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(responseEnd - responseStart).count()));
    double totalMs = std::chrono::duration<double, std::milli>(responseEnd - workerStart).count();
    updateLatencyEwma(totalMs);

    // v5.5P-R8.1: op-level latency percentiles (p50/p95/p99/p999 emitted by the
    // exporter's histogram section) split by read vs write action class.
    {
        static const std::set<std::string> kWriteActions = {
            "insert", "updateOne", "deleteOne", "bulk", "createCollection",
            "createDatabase", "upload", "insertVector", "drop_database"
        };
        static const std::set<std::string> kReadActions = {
            "find", "count", "listCollections", "listDatabases",
            "queryVector", "aggregate"
        };
        const double totalUs = totalMs * 1000.0;
        if (kWriteActions.count(action)) {
            MetricsExporter::recordHistogram("pacificdb_write_latency_us", totalUs);
        } else if (kReadActions.count(action)) {
            MetricsExporter::recordHistogram("pacificdb_read_latency_us", totalUs);
        }
    }

    if (g_reqTrace && shouldTraceAction(action)) {
        if (g_reqTraceSlowMs <= 0 || totalMs >= static_cast<double>(g_reqTraceSlowMs)) {
            std::cerr << "[REQ] action=" << action
                      << " request_id=" << requestId
                      << " trace_id=" << traceId
                      << " queue_ms=" << queueWaitMs
                      << " read_ms=" << readMs
                      << " parse_ms=" << parseMs
                      << " exec_ms=" << execMs
                      << " response_ms=" << responseMs
                      << " total_ms=" << totalMs
                      << " bytes_in=" << totalBytes
                      << " bytes_out=" << out.size()
                      << " status=" << (res.contains("error") ? "error" : "ok")
                      << " ka_req=" << _kaI
                      << std::endl;
        }
    }

    g_responsePending.fetch_sub(1);
    updateInflightGauges();

    if (_kaSockClosed) break;
    } // end ENGINE_KEEPALIVE for loop

    clientSocketGuard.closeNow();
}

void startServer() {
    g_serverLifecycleStarted = 1;
    g_serverShutdownRequested = 0;
    g_serverReady = 0;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    try {
        initializeClientTls();
    } catch (const std::exception& error) {
        std::cerr << "[SERVER] FATAL: TLS configuration refused: "
                  << error.what() << std::endl;
        return;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server < 0) {
        std::cerr << "[SERVER] FATAL: Failed to create client listener socket" << std::endl;
        return;
    }
    g_serverListener = server;
    if (serverShutdownRequested()) {
        requestServerShutdown();
        return;
    }

    // Allow port reuse to avoid TIME_WAIT issues after crash/restart
    int optval = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Read ENGINE_PORT from environment (default 9000)
    int enginePort = 9000;
    if (char* e = getenv("ENGINE_PORT")) {
        try { enginePort = std::stoi(e); } catch (...) {}
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(enginePort);
    std::string engineBindHost = "0.0.0.0";
    if (const char* bindEnv = std::getenv("ENGINE_BIND_HOST")) {
        if (*bindEnv) engineBindHost = bindEnv;
    }
    if (engineBindHost == "0.0.0.0" || engineBindHost == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, engineBindHost.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[SERVER] FATAL: Invalid ENGINE_BIND_HOST=" << engineBindHost << std::endl;
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[SERVER] FATAL: Failed to bind " << engineBindHost << ":" << enginePort
                  << " (errno=" << errno << ")" << std::endl;
#ifdef _WIN32
        closesocket(server);
#else
        close(server);
#endif
        g_serverListener = INVALID_SOCKET;
        // The control plane routes to the assigned port. Falling back to another
        // port leaves a Raft member alive but makes its data plane unreachable.
        return;
    }
    if (listen(server, SOMAXCONN) < 0) {
        std::cerr << "[SERVER] FATAL: Failed to listen on port " << enginePort << std::endl;
#ifdef _WIN32
        closesocket(server);
#else
        close(server);
#endif
        g_serverListener = INVALID_SOCKET;
        return;
    }
    pacificdb::test::hitFailpoint("FP_SHUTDOWN_AFTER_BIND_BEFORE_READY", 1);
    if (serverShutdownRequested()) {
        requestServerShutdown();
        return;
    }

    // Initialize Tenant Manager for MongoDB-style user isolation
    std::string dataRoot = DatabaseEngine::getDataRoot();
    pacificdb::tenant::TenantManager::instance().initialize(dataRoot);
    std::cout << "[SERVER] TenantManager initialized for multi-tenant isolation" << std::endl;

    // Enable debug logging only if ENGINE_DEBUG_LOG=1
    const char* debugLogEnv = std::getenv("ENGINE_DEBUG_LOG");
    if (debugLogEnv && std::string(debugLogEnv) == "1") {
        g_debugLog = true;
        std::cout << "[SERVER] Debug logging ENABLED" << std::endl;
    }

    const char* reqTraceEnv = std::getenv("REQ_TRACE");
    if (reqTraceEnv) {
        std::string v(reqTraceEnv);
        if (v == "1" || v == "true" || v == "TRUE") {
            g_reqTrace = true;
        }
    }
    if (const char* slowMsEnv = std::getenv("REQ_TRACE_SLOW_MS")) {
        try { g_reqTraceSlowMs = std::stoll(slowMsEnv); } catch (...) { g_reqTraceSlowMs = 0; }
    }
    if (g_reqTrace) {
        std::cout << "[SERVER] Request tracing ENABLED for insert/find"
                  << " (slow threshold=" << g_reqTraceSlowMs << "ms)" << std::endl;
    }

    if (const char* consistencyEnv = std::getenv("CONSISTENCY_TRACE")) {
        std::string v(consistencyEnv);
        if (v == "1" || v == "true" || v == "TRUE") {
            g_consistencyTrace = true;
            std::cout << "[SERVER] Consistency tracing ENABLED" << std::endl;
        }
    }

    if (const char* zeroCopyEnv = std::getenv("SOCKET_ZEROCOPY")) {
        std::string v(zeroCopyEnv);
        if (v == "1" || v == "true" || v == "TRUE") {
            g_socketZeroCopy = true;
            std::cout << "[SERVER] Linux socket zero-copy requested (SOCKET_ZEROCOPY=1)" << std::endl;
        }
    }

    const char* hardReqLogEnv = std::getenv("HARD_REQ_LOG");
    if (hardReqLogEnv) {
        std::string v(hardReqLogEnv);
        if (v == "1" || v == "true" || v == "TRUE") {
            g_hardReqLog = true;
            std::cout << "[SERVER] Hard request logs ENABLED ([REQ START]/[EXEC START]/[EXEC END])" << std::endl;
        }
    }

    // Note: leader role is managed by RaftCore; reflect that dynamic state when printing

    // Initialize connection pool
    // Dynamic resource configuration - Calculate optimal values based on system
    size_t cpuCores = std::thread::hardware_concurrency();
    if (cpuCores == 0) cpuCores = 8; // Fallback if detection fails
    // ENGINE_CPU_CORES override: cap the perceived core count. High core counts (e.g. 16)
    // drive a larger worker/connection-thread fanout that exposes a concurrency-sensitive
    // divide-by-zero (SIGFPE) in the sharded task queue under contention. Capping the
    // perceived cores bounds that fanout to a level proven stable in the field.
    if (const char* envCores = std::getenv("ENGINE_CPU_CORES")) {
        try {
            size_t forced = static_cast<size_t>(std::stoul(envCores));
            if (forced >= 1 && forced <= cpuCores) {
                std::cout << "[DYNAMIC-RESOURCE] ENGINE_CPU_CORES override: " << cpuCores
                          << " -> " << forced << " (concurrency cap)\n";
                cpuCores = forced;
            }
        } catch (...) {}
    }

    // Read target CPU utilization (default 90%)
    double targetCpu = 90.0;
    if (char* envTarget = getenv("TARGET_CPU_UTILIZATION")) {
        try { targetCpu = std::stod(envTarget); } catch (...) {}
    }

    // Calculate optimal values based on CPU and target utilization
    size_t optimalWorkers = static_cast<size_t>(cpuCores * (targetCpu / 100.0));
    if (optimalWorkers < 4) optimalWorkers = 4; // Minimum for stability

    // RESOURCE SIZING: More aggressive for high-load scenarios
    size_t optimalThreads = optimalWorkers * 16; // 16 threads per worker for high I/O
    size_t minThreads = cpuCores;
    if (minThreads < 8) minThreads = 8;

    size_t maxThreads = optimalThreads * 4; // 4x for peak loads
    if (maxThreads < 512) maxThreads = 512; // Quick starvation test baseline
    size_t maxQueue = maxThreads * 64; // 64x threads for extreme burst capacity (15K+ users)

    // Read connection pool settings from environment with AUTO support
    char* envMin = getenv("CONN_MIN_THREADS");
    char* envMax = getenv("CONN_MAX_THREADS");
    char* envQueue = getenv("CONN_MAX_QUEUE");

    // Parse with AUTO support
    if (envMin && std::string(envMin) != "AUTO") {
        try { minThreads = std::stoul(envMin); } catch (...) {}
    }
    if (envMax && std::string(envMax) != "AUTO") {
        try { maxThreads = std::stoul(envMax); } catch (...) {}
    }
    if (envQueue && std::string(envQueue) != "AUTO") {
        try { maxQueue = std::stoul(envQueue); } catch (...) {}
    }

    std::cout << "[DYNAMIC-RESOURCE] CPU Cores: " << cpuCores << " | Target: " << targetCpu << "%\n";
    std::cout << "[DYNAMIC-RESOURCE] Optimal Workers: " << optimalWorkers << " | Threads: " << optimalThreads << "\n";
    std::cout << "[DYNAMIC-RESOURCE] Connection Pool: " << minThreads << "-" << maxThreads << " threads, queue: " << maxQueue << "\n";

    initConnectionPool(minThreads, maxThreads, maxQueue);

    // Admission control settings (core backpressure)
    size_t defaultEffectiveInflight = cpuCores <= 8 ? 1000 : 1500;
    size_t maxConnections = defaultEffectiveInflight;
    if (char* envInflight = getenv("EFFECTIVE_INFLIGHT_TARGET")) {
        try { maxConnections = std::stoul(envInflight); } catch (...) {}
    }
    if (char* envMaxConn = getenv("MAX_CONNECTIONS")) {
        try { maxConnections = std::stoul(envMaxConn); } catch (...) {}
    }

    size_t queueHighWatermark = (maxQueue * 75) / 100; // start shedding at 75%
    if (queueHighWatermark == 0) queueHighWatermark = 1;
    if (char* envQhwm = getenv("CONN_QUEUE_HIGH_WATERMARK")) {
        try { queueHighWatermark = std::stoul(envQhwm); } catch (...) {}
    }

    bool adaptiveAdmissionEnabled = true;
    if (const char* envAdaptive = std::getenv("ADAPTIVE_ADMISSION")) {
        std::string v(envAdaptive);
        adaptiveAdmissionEnabled = !(v == "0" || v == "false" || v == "FALSE");
    }

    double admissionTargetCpuPct = 85.0;
    if (const char* envCpu = std::getenv("ADMISSION_TARGET_CPU_PCT")) {
        try { admissionTargetCpuPct = std::stod(envCpu); } catch (...) {}
    }
    double admissionTargetLatencyMs = 400.0;
    if (const char* envLat = std::getenv("ADMISSION_TARGET_LATENCY_MS")) {
        try { admissionTargetLatencyMs = std::stod(envLat); } catch (...) {}
    }

    size_t minAdaptiveMaxConnections = std::max<size_t>(512, maxConnections / 8);
    if (const char* envMin = std::getenv("ADMISSION_MIN_CONNECTIONS")) {
        try { minAdaptiveMaxConnections = std::stoul(envMin); } catch (...) {}
    }
    size_t minAdaptiveQueueHighWatermark = std::max<size_t>(64, queueHighWatermark / 6);
    if (const char* envMin = std::getenv("ADMISSION_MIN_QUEUE_HWM")) {
        try { minAdaptiveQueueHighWatermark = std::stoul(envMin); } catch (...) {}
    }

    std::atomic<size_t> dynamicMaxConnections{maxConnections};
    std::atomic<size_t> dynamicQueueHighWatermark{queueHighWatermark};
    g_configuredMaxConnections.store(maxConnections);
    g_dynamicMaxConnectionsGauge.store(maxConnections);
    g_configuredQueueHighWatermark.store(queueHighWatermark);
    g_dynamicQueueHighWatermarkGauge.store(queueHighWatermark);

    // Configure DB task queue with AUTO support - RESOURCE SIZING
    size_t dbqShards = 32;
    size_t dbqWorkersPerShard = 4;
    size_t dbqMaxQueuePerShard = optimalWorkers * 32 * 1024;

    if (char* e = getenv("DBQ_SHARDS")) {
        if (std::string(e) != "AUTO") {
            try { dbqShards = std::stoul(e); } catch (...) {}
        }
    }
    if (char* e = getenv("DBQ_WORKERS_PER_SHARD")) {
        if (std::string(e) != "AUTO") {
            try { dbqWorkersPerShard = std::stoul(e); } catch (...) {}
        }
    } else if (char* e = getenv("DBQ_WORKERS")) {
        if (std::string(e) != "AUTO") {
            try { dbqWorkersPerShard = std::stoul(e); } catch (...) {}
        }
    }
    if (char* e = getenv("DBQ_MAX_QUEUE_PER_SHARD")) {
        if (std::string(e) != "AUTO") {
            try { dbqMaxQueuePerShard = std::stoul(e); } catch (...) {}
        }
    } else if (char* e = getenv("DBQ_MAX_QUEUE")) {
        if (std::string(e) != "AUTO") {
            try { dbqMaxQueuePerShard = std::stoul(e); } catch (...) {}
        }
    }

    std::cout << "[DYNAMIC-RESOURCE] DB Task Queue Partitioned: " << dbqShards
              << " shards, " << dbqWorkersPerShard << " workers/shard, queue/shard: "
              << dbqMaxQueuePerShard << "\n";
    DBTaskQueuePartitioned::configure(dbqShards, dbqWorkersPerShard, dbqMaxQueuePerShard);

    std::cout << "[SERVER] Listening on " << engineBindHost << ":" << enginePort << " with connection pool...\n";
    std::cout << "[SERVER] Connection pool: " << minThreads << "-" << maxThreads << " threads, max queue: " << maxQueue << "\n";
    std::cout << "[SERVER] Admission control: max connections=" << maxConnections
              << " (effective inflight target)"
              << ", queue high watermark=" << queueHighWatermark << "\n";
    if (adaptiveAdmissionEnabled) {
        std::cout << "[SERVER] Adaptive admission ENABLED: target_cpu=" << admissionTargetCpuPct
                  << "% target_latency=" << admissionTargetLatencyMs << "ms\n";
    }
    std::cout << "[SERVER] Role: " << (RaftCore::instance().isLeader() ? "LEADER" : "FOLLOWER") << "\n";

    // Start memory monitor thread to detect system memory pressure and enable adaptive backpressure
    std::thread memoryMonitor([](){
        // Configurable thresholds
        size_t checkIntervalMs = 1000; // default 1s
        int memThresholdPercent = 95; // default 95% — raised because we now use MemAvailable
        if (char* e = getenv("MEMORY_CHECK_INTERVAL_MS")) {
            try { checkIntervalMs = std::stoul(e); } catch(...) {}
        }
        if (char* e = getenv("MEM_PRESSURE_PERCENT")) {
            try { memThresholdPercent = std::stoi(e); } catch(...) {}
        }

        bool lastState = false;
        while (!serverShutdownRequested()) {
            int usedPercent = 0;
#ifdef _WIN32
            MEMORYSTATUSEX statex;
            statex.dwLength = sizeof(statex);
            GlobalMemoryStatusEx(&statex);
            DWORDLONG total = statex.ullTotalPhys;
            DWORDLONG avail = statex.ullAvailPhys;
            usedPercent = (int)(((double)(total - avail) / (double)total) * 100.0);
#else
            // Use /proc/meminfo MemAvailable — this correctly accounts for page cache & reclaimable
            // sysinfo::freeram is misleading (excludes buffers/cache that are reclaimable)
            unsigned long totalKB = 0, availKB = 0;
            std::ifstream meminfo("/proc/meminfo");
            if (meminfo.is_open()) {
                std::string line;
                while (std::getline(meminfo, line)) {
                    if (line.rfind("MemTotal:", 0) == 0) {
                        sscanf(line.c_str(), "MemTotal: %lu kB", &totalKB);
                    } else if (line.rfind("MemAvailable:", 0) == 0) {
                        sscanf(line.c_str(), "MemAvailable: %lu kB", &availKB);
                    }
                    if (totalKB && availKB) break;
                }
                meminfo.close();
            }
            if (totalKB > 0) {
                usedPercent = (int)(((double)(totalKB - availKB) / (double)totalKB) * 100.0);
            } else {
                // Fallback to sysinfo if /proc/meminfo unavailable
                struct sysinfo info;
                sysinfo(&info);
                unsigned long total = info.totalram * info.mem_unit;
                unsigned long avail = info.freeram * info.mem_unit;
                usedPercent = (int)(((double)(total - avail) / (double)total) * 100.0);
            }
#endif
            bool pressured = (usedPercent >= memThresholdPercent);
            g_memoryPressure.store(pressured);
            if (pressured != lastState) {
                std::lock_guard<std::mutex> lk(g_memoryLogMu);
                std::cerr << "[MEMORY_MONITOR] Memory used percent=" << usedPercent << "% threshold=" << memThresholdPercent << "% -> " << (pressured?"PRESSURE":"OK") << std::endl;
                lastState = pressured;
            }
            const size_t slices = std::max<size_t>(1, checkIntervalMs / 50);
            for (size_t slice = 0; slice < slices && !serverShutdownRequested(); ++slice) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    std::thread adaptiveAdmissionMonitor;
    if (adaptiveAdmissionEnabled) {
        adaptiveAdmissionMonitor = std::thread(
            [&, maxConnections, queueHighWatermark, minAdaptiveMaxConnections, minAdaptiveQueueHighWatermark,
             admissionTargetCpuPct, admissionTargetLatencyMs]() {
            while (!serverShutdownRequested()) {
                for (int slice = 0; slice < 20 && !serverShutdownRequested(); ++slice) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (serverShutdownRequested()) break;

                size_t queued = g_connectionPool ? g_connectionPool->getQueuedTasks() : 0;
                double latencyMs = g_requestLatencyEwmaMs.load(std::memory_order_relaxed);
                if (latencyMs <= 0.0) latencyMs = EngineMetrics::queryMetrics().avgLatencyMs();

                size_t walPending = WAL::getPendingCount();
                static const size_t walPendingHigh = [] {
                    if (const char* value = std::getenv("WAL_PENDING_PRESSURE")) {
                        try { return std::max<size_t>(1, std::stoul(value)); } catch (...) {}
                    }
                    return size_t{5000};
                }();

                double cpuPct = 0.0;
#ifdef __linux__
                cpuPct = sampleSystemCpuUsagePercent();
#endif

                bool walHot = (walPendingHigh > 0 && walPending >= walPendingHigh);
                bool tooHot = g_memoryPressure.load() || walHot || cpuPct > admissionTargetCpuPct || latencyMs > admissionTargetLatencyMs;
                bool tooCold = !g_memoryPressure.load()
                    && cpuPct > 0.1
                    && cpuPct < (admissionTargetCpuPct * 0.70)
                    && latencyMs > 0.1
                    && latencyMs < (admissionTargetLatencyMs * 0.70)
                    && queued < (dynamicQueueHighWatermark.load() / 2)
                    && !walHot;

                size_t currentConn = dynamicMaxConnections.load();
                size_t currentQhwm = dynamicQueueHighWatermark.load();

                size_t nextConn = currentConn;
                size_t nextQhwm = currentQhwm;

                if (tooHot) {
                    nextConn = std::max(minAdaptiveMaxConnections, (currentConn * 90) / 100);
                    nextQhwm = std::max(minAdaptiveQueueHighWatermark, (currentQhwm * 90) / 100);
                } else if (tooCold) {
                    nextConn = std::min(maxConnections, currentConn + std::max<size_t>(32, maxConnections / 25));
                    nextQhwm = std::min(queueHighWatermark, currentQhwm + std::max<size_t>(16, queueHighWatermark / 25));
                }

                dynamicMaxConnections.store(nextConn);
                dynamicQueueHighWatermark.store(nextQhwm);
                g_dynamicMaxConnectionsGauge.store(nextConn);
                g_dynamicQueueHighWatermarkGauge.store(nextQhwm);
            }
        });
    }

    // A connection-pool task owns the whole keepalive socket, not one query. Giving it
    // a 30-second cancellation deadline made every later request inherit an expired
    // QueryCancel token during soak tests. Individual query limits are enforced inside
    // handleClient; keep connection tasks deadline-free unless explicitly configured.
    int taskTimeoutMs = 0;
    if (const char* envTimeout = std::getenv("POOL_TASK_TIMEOUT_MS")) {
        taskTimeoutMs = std::atoi(envTimeout);
    }

    size_t connectionLimitWaitMs = 0;
    if (const char* envWait = std::getenv("CONNECTION_LIMIT_WAIT_MS")) {
        try { connectionLimitWaitMs = std::stoul(envWait); } catch (...) { connectionLimitWaitMs = 0; }
    }

    size_t dispatchBatchSize = 64;
    if (const char* envDispatchBatch = std::getenv("POOL_DISPATCH_BATCH")) {
        try { dispatchBatchSize = std::max<size_t>(1, std::stoul(envDispatchBatch)); } catch (...) {}
    }

    bool prioritySchedulerEnabled = true;
    if (const char* envPriority = std::getenv("PRIORITY_SCHEDULER")) {
        std::string v(envPriority);
        prioritySchedulerEnabled = !(v == "0" || v == "false" || v == "FALSE");
    }

    size_t coalesceWindowUs = 250;
    if (const char* envCoalesce = std::getenv("REQUEST_COALESCE_US")) {
        try { coalesceWindowUs = std::max<size_t>(0, std::stoul(envCoalesce)); } catch (...) {}
    }

    auto rejectClient = [&](SOCKET client, const std::string& payload, const std::string& reason = "") {
        recordPipelineCounter("pacificdb_pipeline_rejected_requests_total");
        if (!reason.empty()) {
            recordRejectReasonCounter(reason);
        }
        recordLifecycleCounter("response_send_started");
        const uint64_t localSendStartUs = steadyNowUs();
        auto sendResult = sendTrackedPayload(client, payload, 0);
        if (sendResult.partial) recordPipelineCounter("pacificdb_pipeline_response_send_partial_total");
        if (sendResult.eagain) recordPipelineCounter("pacificdb_pipeline_response_send_eagain_total");
        if (!sendResult.ok) recordPipelineCounter("pacificdb_pipeline_response_send_failed_total");
        recordLifecycleCounter("response_send_completed");
        recordLifecycleCounter("socket_closed");
        MetricsExporter::recordCustomMetric("pacificdb_pipeline_last_reject_send_us", static_cast<double>(steadyNowUs() - localSendStartUs));
        untrackClientSocket(client);
        CLOSE_SOCKET(client);
    };

    auto shouldRejectPreQueue = [&](SOCKET client) -> bool {
        size_t effectiveMaxConnections = adaptiveAdmissionEnabled ? dynamicMaxConnections.load() : maxConnections;
        size_t effectiveQueueHwm = adaptiveAdmissionEnabled ? dynamicQueueHighWatermark.load() : queueHighWatermark;

        size_t activeConnectionsNow = static_cast<size_t>(g_activeConnections.load());
        if (activeConnectionsNow >= effectiveMaxConnections) {
            if (connectionLimitWaitMs > 0) {
                g_connectionLimitWaits.fetch_add(1, std::memory_order_relaxed);
                const auto waitStart = std::chrono::steady_clock::now();
                const auto deadline = waitStart + std::chrono::milliseconds(connectionLimitWaitMs);
                while (std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    activeConnectionsNow = static_cast<size_t>(g_activeConnections.load());
                    if (activeConnectionsNow < effectiveMaxConnections) {
                        const auto waitedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - waitStart).count());
                        g_connectionLimitWaitSuccesses.fetch_add(1, std::memory_order_relaxed);
                        g_connectionLimitWaitTotalMs.fetch_add(waitedMs, std::memory_order_relaxed);
                        uint64_t prevMax = g_connectionLimitWaitMaxMs.load(std::memory_order_relaxed);
                        while (waitedMs > prevMax &&
                               !g_connectionLimitWaitMaxMs.compare_exchange_weak(prevMax, waitedMs, std::memory_order_relaxed)) {}
                        break;
                    }
                }
            }

            activeConnectionsNow = static_cast<size_t>(g_activeConnections.load());
        }
        if (activeConnectionsNow >= effectiveMaxConnections) {
            g_connectionLimitRejects.fetch_add(1, std::memory_order_relaxed);
            rejectClient(client, "{\"error\":\"server_busy\",\"reason\":\"connection_limit\",\"retry_after_ms\":200}", "connection_limit");
            return true;
        }

        size_t queuedNow = g_connectionPool ? g_connectionPool->getQueuedTasks() : 0;
        if (queuedNow >= effectiveQueueHwm) {
            g_queueHighRejects.fetch_add(1, std::memory_order_relaxed);
            size_t queuePressurePct = (maxQueue > 0) ? ((queuedNow * 100) / maxQueue) : 100;
            if (queuePressurePct > 100) queuePressurePct = 100;
            size_t retryAfterMs = 100 + (queuePressurePct * 9);
            rejectClient(client,
                "{\"error\":\"server_busy\",\"reason\":\"queue_high\",\"queue\":" + std::to_string(queuedNow) +
                ",\"retry_after_ms\":" + std::to_string(retryAfterMs) + "}",
                "queue_high");
            return true;
        }

        if (g_memoryPressure.load()) {
            rejectClient(client, "{\"error\":\"memory_pressure\",\"retry_after_ms\":500}", "memory_pressure");
            return true;
        }
        return false;
    };

    auto inferPriority = [&](SOCKET client) -> ConnectionPool::Priority {
        if (!prioritySchedulerEnabled) return ConnectionPool::Priority::NORMAL;
        // TLS records are intentionally opaque until the worker performs the
        // handshake. Never inspect encrypted bytes as if they were JSON.
        if (clientTlsEnabled()) return ConnectionPool::Priority::NORMAL;
#ifdef __linux__
        char peekBuf[768];
        int bytes = recv(client, peekBuf, sizeof(peekBuf), MSG_PEEK | MSG_DONTWAIT);
        if (bytes > 0) {
            std::string payload(peekBuf, peekBuf + bytes);
            if (payload.find("\"action\":\"ping\"") != std::string::npos ||
                payload.find("\"action\":\"find\"") != std::string::npos ||
                payload.find("\"action\":\"opStatus\"") != std::string::npos ||
                payload.find("\"action\":\"get_metrics\"") != std::string::npos) {
                return ConnectionPool::Priority::HIGH;
            }
            if (payload.find("\"action\":\"bulk\"") != std::string::npos ||
                payload.find("\"action\":\"bulkWrite\"") != std::string::npos ||
                payload.find("\"action\":\"insertMany\"") != std::string::npos) {
                return ConnectionPool::Priority::LOW;
            }
            if (payload.find("\"action\":\"insert\"") != std::string::npos ||
                payload.find("\"action\":\"deleteOne\"") != std::string::npos ||
                payload.find("\"action\":\"deleteMany\"") != std::string::npos ||
                payload.find("\"action\":\"updateOne\"") != std::string::npos ||
                payload.find("\"action\":\"updateMany\"") != std::string::npos ||
                payload.find("\"action\":\"insertVector\"") != std::string::npos) {
                return ConnectionPool::Priority::NORMAL;
            }
        }
#endif
        return ConnectionPool::Priority::NORMAL;
    };

    std::vector<std::pair<SOCKET, long long>> pendingHigh;
    std::vector<std::pair<SOCKET, long long>> pendingNormal;
    std::vector<std::pair<SOCKET, long long>> pendingLow;
    pendingHigh.reserve(dispatchBatchSize);
    pendingNormal.reserve(dispatchBatchSize);
    pendingLow.reserve(dispatchBatchSize);

    auto lastFlush = std::chrono::steady_clock::now();

    auto flushQueue = [&](std::vector<std::pair<SOCKET, long long>>& pending,
                         ConnectionPool::Priority priority) {
        if (pending.empty()) return;

        std::vector<ConnectionPool::Task> tasks;
        tasks.reserve(pending.size());
        for (const auto& entry : pending) {
            SOCKET client = entry.first;
            long long enqueuedAtUs = entry.second;
            tasks.emplace_back([client, enqueuedAtUs]() {
                handleClient((unsigned long long)client, enqueuedAtUs);
            });
        }

        size_t accepted = g_connectionPool
            ? g_connectionPool->submitBatch(std::move(tasks), std::chrono::milliseconds(taskTimeoutMs), priority)
            : 0;

        if (accepted > 0) {
            recordPipelineCounter("pacificdb_pipeline_accepted_requests_total", static_cast<double>(accepted));
        }

        if (accepted < pending.size()) {
            static std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now() - std::chrono::seconds(2);
            static int rejectCount = 0;
            rejectCount += static_cast<int>(pending.size() - accepted);

            auto now = std::chrono::steady_clock::now();
            auto msSince = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count();
            if (msSince > 1000) {
                std::cerr << "[SERVER] Connection pool overloaded, rejecting " << rejectCount
                          << " connection(s) in last " << msSince << "ms\n";
                lastLog = now;
                rejectCount = 0;
            }

            size_t queued = g_connectionPool ? g_connectionPool->getQueuedTasks() : maxQueue;
            size_t pressurePct = (maxQueue > 0) ? ((queued * 100) / maxQueue) : 100;
            if (pressurePct > 100) pressurePct = 100;
            static const std::optional<size_t> configuredBackoffMs = []() -> std::optional<size_t> {
                if (const char* value = std::getenv("CONN_REJECT_BACKOFF_MS")) {
                    try { return std::stoul(value); } catch (...) {}
                }
                return std::nullopt;
            }();
            const size_t backoffMs = configuredBackoffMs.value_or(50 + (pressurePct * 9));

            for (size_t i = accepted; i < pending.size(); ++i) {
                decrementGaugeSafe(g_acceptQueueDepth);
                recordTimeoutOriginCounter("connpool");
                g_serverOverloadedRejects.fetch_add(1, std::memory_order_relaxed);
                rejectClient(pending[i].first,
                             "{\"error\":\"server_overloaded\",\"retry_after_ms\":" + std::to_string(backoffMs) + "}",
                             "server_overloaded");
            }
        }

        updateInflightGauges();

        pending.clear();
    };

    auto flushPendingClients = [&]() {
        flushQueue(pendingHigh, ConnectionPool::Priority::HIGH);
        flushQueue(pendingNormal, ConnectionPool::Priority::NORMAL);
        flushQueue(pendingLow, ConnectionPool::Priority::LOW);
        lastFlush = std::chrono::steady_clock::now();
    };

    auto closePendingClients = [&]() {
        auto closeQueue = [&](std::vector<std::pair<SOCKET, long long>>& pending) {
            for (const auto& entry : pending) {
                decrementGaugeSafe(g_acceptQueueDepth);
                untrackClientSocket(entry.first);
                CLOSE_SOCKET(entry.first);
            }
            pending.clear();
        };
        closeQueue(pendingHigh);
        closeQueue(pendingNormal);
        closeQueue(pendingLow);
    };

    auto maybeFlushPendingClients = [&]() {
        size_t totalPending = pendingHigh.size() + pendingNormal.size() + pendingLow.size();
        if (totalPending >= dispatchBatchSize) {
            flushPendingClients();
            return;
        }
        if (coalesceWindowUs == 0) {
            flushPendingClients();
            return;
        }
        auto now = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - lastFlush).count();
        if (us >= static_cast<long long>(coalesceWindowUs)) {
            flushPendingClients();
        }
    };

    bool epollLoopCompleted = false;
    g_serverReady = 1;
#ifdef __linux__
    bool useEpoll = true;
    if (const char* ioModel = std::getenv("SERVER_IO_MODEL")) {
        std::string model(ioModel);
        if (model == "blocking" || model == "threadpool") {
            useEpoll = false;
        }
    }

    size_t epollEventCapacity = 256;
    if (const char* envEvents = std::getenv("EPOLL_EVENTS")) {
        try { epollEventCapacity = std::max<size_t>(16, std::stoul(envEvents)); } catch (...) {}
    }
    size_t epollAcceptBatch = 256;
    if (const char* envAcceptBatch = std::getenv("EPOLL_ACCEPT_BATCH")) {
        try { epollAcceptBatch = std::max<size_t>(1, std::stoul(envAcceptBatch)); } catch (...) {}
    }

    if (useEpoll && !serverShutdownRequested()) {
        int flags = fcntl(server, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(server, F_SETFL, flags | O_NONBLOCK);
        }

        int epollFd = epoll_create1(0);
        if (epollFd >= 0) {
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = server;

            if (epoll_ctl(epollFd, EPOLL_CTL_ADD, server, &ev) == 0) {
                std::vector<epoll_event> events(epollEventCapacity);
                std::cout << "[SERVER] I/O model: epoll (events=" << epollEventCapacity
                          << ", accept_batch=" << epollAcceptBatch
                          << ", dispatch_batch=" << dispatchBatchSize
                          << ", coalesce_us=" << coalesceWindowUs
                          << ", priority_scheduler=" << (prioritySchedulerEnabled ? "on" : "off") << ")\n";

                while (!serverShutdownRequested()) {
                    int n = epoll_wait(epollFd, events.data(), (int)events.size(), 1000);
                    if (n < 0) {
                        if (serverShutdownRequested()) break;
                        if (errno == EINTR) continue;
                        std::cerr << "[SERVER] epoll_wait error: " << errno << "\n";
                        continue;
                    }

                    for (int i = 0; i < n; ++i) {
                        if (events[i].data.fd != server) continue;

                        size_t acceptedInTick = 0;
                        while (acceptedInTick < epollAcceptBatch && !serverShutdownRequested()) {
                            SOCKET client = accept(server, nullptr, nullptr);
                            if (client == INVALID_SOCKET) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    break;
                                }
                                break;
                            }

                            trackClientSocket(client);
                            int clientFlags = fcntl(client, F_GETFL, 0);
                            if (clientFlags >= 0) {
                                fcntl(client, F_SETFL, clientFlags & ~O_NONBLOCK);
                            }

                            acceptedInTick++;
                            if (shouldRejectPreQueue(client)) {
                                continue;
                            }

                            long long enqueuedAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            recordLifecycleCounter("request_queued");
                            switch (inferPriority(client)) {
                                case ConnectionPool::Priority::HIGH:
                                    pendingHigh.emplace_back(client, enqueuedAtUs);
                                    recordPipelineCounter("pacificdb_pipeline_queued_requests_total");
                                    g_acceptQueueDepth.fetch_add(1);
                                    break;
                                case ConnectionPool::Priority::LOW:
                                    pendingLow.emplace_back(client, enqueuedAtUs);
                                    recordPipelineCounter("pacificdb_pipeline_queued_requests_total");
                                    g_acceptQueueDepth.fetch_add(1);
                                    break;
                                case ConnectionPool::Priority::NORMAL:
                                default:
                                    pendingNormal.emplace_back(client, enqueuedAtUs);
                                    recordPipelineCounter("pacificdb_pipeline_queued_requests_total");
                                    g_acceptQueueDepth.fetch_add(1);
                                    break;
                            }

                            updateInflightGauges();

                            maybeFlushPendingClients();
                        }
                    }

                    maybeFlushPendingClients();
                }
                epollLoopCompleted = true;
                CLOSE_SOCKET(epollFd);
            } else {
                std::cerr << "[SERVER] epoll_ctl add listen socket failed, falling back to blocking accept\n";
                CLOSE_SOCKET(epollFd);
            }
        } else {
            std::cerr << "[SERVER] epoll_create1 failed, falling back to blocking accept\n";
        }
    }
#endif

    if (!epollLoopCompleted) while (!serverShutdownRequested()) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (serverShutdownRequested()) break;
            continue;
        }

        trackClientSocket(client);
        if (shouldRejectPreQueue(client)) {
            continue;
        }

        long long enqueuedAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        recordLifecycleCounter("request_queued");
        switch (inferPriority(client)) {
            case ConnectionPool::Priority::HIGH:
                pendingHigh.emplace_back(client, enqueuedAtUs);
                recordPipelineCounter("pacificdb_pipeline_queued_requests_total");
                g_acceptQueueDepth.fetch_add(1);
                break;
            case ConnectionPool::Priority::LOW:
                pendingLow.emplace_back(client, enqueuedAtUs);
                recordPipelineCounter("pacificdb_pipeline_queued_requests_total");
                g_acceptQueueDepth.fetch_add(1);
                break;
            case ConnectionPool::Priority::NORMAL:
            default:
                pendingNormal.emplace_back(client, enqueuedAtUs);
                recordPipelineCounter("pacificdb_pipeline_queued_requests_total");
                g_acceptQueueDepth.fetch_add(1);
                break;
        }
        updateInflightGauges();
        maybeFlushPendingClients();
    }

    g_serverReady = 0;
    const auto serverStopStartedAt = std::chrono::steady_clock::now();
    const auto logServerStopElapsed = [&serverStopStartedAt](const char* stage) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - serverStopStartedAt).count();
        std::cerr << "[SERVER][SHUTDOWN] " << stage << " elapsed_ms=" << elapsed << std::endl;
    };
    closePendingClients();
    interruptTrackedClientSockets();
    logServerStopElapsed("clients_interrupted");

    // Cleanup connection pool on server shutdown
    destroyConnectionPool();
    logServerStopElapsed("connection_pool_stopped");
    DBTaskQueuePartitioned::shutdownInstance();
    logServerStopElapsed("db_task_queue_stopped");
    if (memoryMonitor.joinable()) memoryMonitor.join();
    if (adaptiveAdmissionMonitor.joinable()) adaptiveAdmissionMonitor.join();
    logServerStopElapsed("server_monitors_stopped");

    if (static_cast<SOCKET>(g_serverListener) == server) {
        g_serverListener = INVALID_SOCKET;
        CLOSE_SOCKET(server);
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
