#include "raft_core.hpp"
#include "database_engine.hpp"
#include "metrics_exporter.hpp"
#include "wal.hpp"
#include "lsm.hpp"
#include "data_durability.hpp"
#include "test_failpoint.hpp"
#include "tls_transport.hpp"
#include "snapshot_bundle.hpp"
#include "wal_integrity.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <random>
#include <future>
#include <condition_variable>
#include <limits>
#include <deque>
#include <cmath>
#include <map>
#include <functional>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <unordered_map>

extern "C" {
int LZ4_compressBound(int inputSize);
int LZ4_compress_default(const char* source, char* dest, int sourceSize, int maxDestSize);
int LZ4_decompress_safe(const char* source, char* dest, int compressedSize, int destCapacity);
}

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <fcntl.h>
#endif

namespace {

constexpr char kRaftMsgpackMagic[] = {'P', 'D', 'B', 'R', '2'};
constexpr char kRaftRecordMagic[] = {'P', 'D', 'B', 'R', '3'};
constexpr std::uint8_t kRaftCodecRaw = 0;
constexpr std::uint8_t kRaftCodecLz4 = 1;
constexpr std::uint32_t kMaxRaftPayloadBytes = 64U * 1024U * 1024U;
constexpr std::size_t kRaftRecordHeaderBytes =
    sizeof(kRaftRecordMagic) + 1 + 2 * sizeof(std::uint32_t);

void appendU32Le(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

bool readU32Le(const std::string& input, std::size_t offset, std::uint32_t& value) {
    if (offset > input.size() || input.size() - offset < sizeof(value)) return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(input[offset + static_cast<std::size_t>(shift / 8)])) << shift;
    }
    return true;
}

std::string encodeRaftPayload(const nlohmann::json& value) {
    const auto packed = nlohmann::json::to_msgpack(value);
    std::string out(kRaftMsgpackMagic, sizeof(kRaftMsgpackMagic));
    out.append(reinterpret_cast<const char*>(packed.data()), packed.size());
    return out;
}

std::string encodeRaftLogPayload(const nlohmann::json& value) {
    const auto packed = nlohmann::json::to_msgpack(value);
    if (packed.empty() || packed.size() > kMaxRaftPayloadBytes - kRaftRecordHeaderBytes ||
        packed.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Raft MessagePack payload is outside the supported size range");
    }

    std::string encoded;
    std::uint8_t codec = kRaftCodecRaw;
    const int bound = LZ4_compressBound(static_cast<int>(packed.size()));
    if (bound > 0) {
        encoded.resize(static_cast<std::size_t>(bound));
        const int compressedSize = LZ4_compress_default(
            reinterpret_cast<const char*>(packed.data()), encoded.data(),
            static_cast<int>(packed.size()), bound);
        if (compressedSize > 0 && static_cast<std::size_t>(compressedSize) < packed.size()) {
            encoded.resize(static_cast<std::size_t>(compressedSize));
            codec = kRaftCodecLz4;
        } else {
            encoded.assign(reinterpret_cast<const char*>(packed.data()), packed.size());
        }
    } else {
        encoded.assign(reinterpret_cast<const char*>(packed.data()), packed.size());
    }

    std::string out(kRaftRecordMagic, sizeof(kRaftRecordMagic));
    out.push_back(static_cast<char>(codec));
    appendU32Le(out, static_cast<std::uint32_t>(packed.size()));
    appendU32Le(out, pacificdb::durability::ChecksumCalculator::crc32c(
        packed.data(), packed.size()));
    out.append(encoded);
    return out;
}

bool decodeRaftPayload(const std::string& payload, nlohmann::json& value) {
    try {
        if (payload.size() >= sizeof(kRaftRecordMagic) &&
            std::equal(std::begin(kRaftRecordMagic), std::end(kRaftRecordMagic), payload.begin())) {
            if (payload.size() < kRaftRecordHeaderBytes) return false;
            const std::uint8_t codec = static_cast<std::uint8_t>(payload[sizeof(kRaftRecordMagic)]);
            std::uint32_t plainSize = 0;
            std::uint32_t expectedCrc = 0;
            if (!readU32Le(payload, sizeof(kRaftRecordMagic) + 1, plainSize) ||
                !readU32Le(payload, sizeof(kRaftRecordMagic) + 1 + sizeof(plainSize), expectedCrc) ||
                plainSize == 0 || plainSize > kMaxRaftPayloadBytes ||
                plainSize > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) return false;

            const std::size_t encodedSize = payload.size() - kRaftRecordHeaderBytes;
            std::string plain;
            if (codec == kRaftCodecRaw) {
                if (encodedSize != plainSize) return false;
                plain.assign(payload.data() + kRaftRecordHeaderBytes, encodedSize);
            } else if (codec == kRaftCodecLz4) {
                if (encodedSize == 0 || encodedSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                    return false;
                }
                plain.resize(plainSize);
                const int decoded = LZ4_decompress_safe(
                    payload.data() + kRaftRecordHeaderBytes, plain.data(), static_cast<int>(encodedSize),
                    static_cast<int>(plainSize));
                if (decoded != static_cast<int>(plainSize)) return false;
            } else {
                return false;
            }
            if (pacificdb::durability::ChecksumCalculator::crc32c(plain.data(), plain.size()) != expectedCrc) {
                return false;
            }
            value = nlohmann::json::from_msgpack(
                reinterpret_cast<const std::uint8_t*>(plain.data()),
                reinterpret_cast<const std::uint8_t*>(plain.data()) + plain.size());
        } else if (payload.size() >= sizeof(kRaftMsgpackMagic) &&
            std::equal(std::begin(kRaftMsgpackMagic), std::end(kRaftMsgpackMagic), payload.begin())) {
            const auto first = reinterpret_cast<const std::uint8_t*>(payload.data()) + sizeof(kRaftMsgpackMagic);
            const auto last = reinterpret_cast<const std::uint8_t*>(payload.data()) + payload.size();
            value = nlohmann::json::from_msgpack(first, last);
        } else {
            value = nlohmann::json::parse(payload);
        }
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json decodeRaftPayloadOrThrow(const std::string& payload) {
    nlohmann::json value;
    if (!decodeRaftPayload(payload, value)) {
        throw std::runtime_error("invalid Raft payload");
    }
    return value;
}

struct PersistentRaftLog {
    std::mutex mutex;
#ifndef _WIN32
    int fd = -1;
    std::string path;
    uint64_t endOffset = 0;
    bool endOffsetKnown = false;
    ~PersistentRaftLog() { if (fd >= 0) close(fd); }
#endif

    bool append(const std::string& logPath, const std::string& bytes, uint64_t* startOffset) {
        std::lock_guard<std::mutex> lock(mutex);
#ifdef _WIN32
        std::ofstream out(logPath, std::ios::binary | std::ios::app);
        out.seekp(0, std::ios::end);
        const auto start = out.tellp();
        if (start < 0) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (out && startOffset) *startOffset = static_cast<uint64_t>(start);
        return static_cast<bool>(out);
#else
        if (fd < 0 || path != logPath) {
            if (fd >= 0) close(fd);
            fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (fd < 0) { path.clear(); endOffsetKnown = false; return false; }
            path = logPath;
            const off_t end = lseek(fd, 0, SEEK_END);
            if (end < 0) { close(fd); fd = -1; path.clear(); endOffsetKnown = false; return false; }
            endOffset = static_cast<uint64_t>(end);
            endOffsetKnown = true;
        }
        if (!endOffsetKnown) {
            const off_t end = lseek(fd, 0, SEEK_END);
            if (end < 0) return false;
            endOffset = static_cast<uint64_t>(end);
            endOffsetKnown = true;
        }
        const uint64_t start = endOffset;
        size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = write(fd, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { endOffsetKnown = false; return false; }
            written += static_cast<size_t>(count);
        }
        endOffset += written;
        if (startOffset) *startOffset = start;
        return true;
#endif
    }

    bool sync(const std::string& logPath) {
        std::lock_guard<std::mutex> lock(mutex);
#ifdef _WIN32
        HANDLE handle = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        const bool ok = FlushFileBuffers(handle) != 0;
        CloseHandle(handle);
        return ok;
#else
        if (fd < 0 || path != logPath) {
            if (fd >= 0) close(fd);
            fd = open(logPath.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
            if (fd < 0) { path.clear(); return false; }
            path = logPath;
        }
        return fdatasync(fd) == 0;
#endif
    }

    void reset(const std::string& logPath) {
#ifndef _WIN32
        std::lock_guard<std::mutex> lock(mutex);
        if (fd >= 0 && path == logPath) {
            close(fd);
            fd = -1;
            path.clear();
            endOffset = 0;
            endOffsetKnown = false;
        }
#else
        (void)logPath;
#endif
    }
};

PersistentRaftLog g_persistentRaftLog;

void appendRaftRecordBytes(std::string& bytes, uint64_t index, uint64_t term,
                           const std::string& payload) {
    const uint32_t size = static_cast<uint32_t>(payload.size());
    bytes.append(reinterpret_cast<const char*>(&index), sizeof(index));
    bytes.append(reinterpret_cast<const char*>(&term), sizeof(term));
    bytes.append(reinterpret_cast<const char*>(&size), sizeof(size));
    bytes.append(payload);
}

bool appendRaftLog(const std::string& logPath, const std::string& bytes, uint64_t* startOffset = nullptr) {
    return g_persistentRaftLog.append(logPath, bytes, startOffset);
}

bool syncRaftLog(const std::string& logPath) {
    return g_persistentRaftLog.sync(logPath);
}

void resetRaftLog(const std::string& logPath) {
    g_persistentRaftLog.reset(logPath);
}

} // namespace

namespace {

std::mutex g_raftTlsMutex;
std::unordered_map<int,
                   std::shared_ptr<pacificdb::transport::TlsSession>>
    g_raftTlsSessions;
std::shared_ptr<pacificdb::transport::TlsContext> g_raftTlsServerContext;
std::shared_ptr<pacificdb::transport::TlsContext> g_raftTlsClientContext;

bool raftEnvironmentFlag(const char* name, bool fallback = false) {
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

std::string requiredRaftTlsPath(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        throw std::runtime_error(std::string(name) + " is required");
    }
    const std::filesystem::path value(raw);
    if (!value.is_absolute()) {
        throw std::runtime_error(std::string(name) + " must be an absolute path");
    }
    return value.lexically_normal().string();
}

void initializeRaftTls() {
    const bool legacyEnabled = raftEnvironmentFlag("RAFT_TLS_ENABLE", false);
    const bool enabled = raftEnvironmentFlag("RAFT_TLS_ENABLED", legacyEnabled);

    std::lock_guard<std::mutex> lock(g_raftTlsMutex);
    g_raftTlsSessions.clear();
    g_raftTlsServerContext.reset();
    g_raftTlsClientContext.reset();
    if (!enabled) return;

    pacificdb::transport::TlsConfig config;
    config.certificatePath = requiredRaftTlsPath("RAFT_TLS_CERT_PATH");
    config.privateKeyPath = requiredRaftTlsPath("RAFT_TLS_KEY_PATH");
    config.caPath = requiredRaftTlsPath("RAFT_TLS_CA_PATH");
    config.verifyPeer = true;
    config.requirePeerCertificate = true;

    g_raftTlsServerContext = pacificdb::transport::TlsContext::create(
        pacificdb::transport::TlsRole::Server, config);
    g_raftTlsClientContext = pacificdb::transport::TlsContext::create(
        pacificdb::transport::TlsRole::Client, config);
}

bool raftTlsEnabled() {
    std::lock_guard<std::mutex> lock(g_raftTlsMutex);
    return static_cast<bool>(g_raftTlsServerContext);
}

std::shared_ptr<pacificdb::transport::TlsSession> raftTlsSession(int socket) {
    std::lock_guard<std::mutex> lock(g_raftTlsMutex);
    const auto found = g_raftTlsSessions.find(socket);
    return found == g_raftTlsSessions.end() ? nullptr : found->second;
}

bool attachRaftTls(int socket, bool server, const std::string& expectedPeer = {}) {
    std::shared_ptr<pacificdb::transport::TlsContext> context;
    {
        std::lock_guard<std::mutex> lock(g_raftTlsMutex);
        context = server ? g_raftTlsServerContext : g_raftTlsClientContext;
    }
    if (!context) return true;

    try {
        auto session = std::make_shared<pacificdb::transport::TlsSession>(
            context, socket);
        if (!session->handshake(expectedPeer)) {
            std::cerr << "[RAFTCORE] TLS handshake refused: "
                      << session->lastError() << std::endl;
            return false;
        }
        std::lock_guard<std::mutex> lock(g_raftTlsMutex);
        g_raftTlsSessions[socket] = std::move(session);
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[RAFTCORE] TLS handshake refused: " << error.what()
                  << std::endl;
        return false;
    }
}

void detachRaftTls(int socket) {
    std::shared_ptr<pacificdb::transport::TlsSession> session;
    {
        std::lock_guard<std::mutex> lock(g_raftTlsMutex);
        const auto found = g_raftTlsSessions.find(socket);
        if (found != g_raftTlsSessions.end()) {
            session = std::move(found->second);
            g_raftTlsSessions.erase(found);
        }
    }
    if (session) session->shutdown();
}

int raftSocketSend(int socket, const void* buffer, size_t length, int flags) {
    if (auto session = raftTlsSession(socket)) {
        return session->write(buffer, length);
    }
    if (raftTlsEnabled()) {
        errno = EPROTO;
        return -1;
    }
    return ::send(socket, static_cast<const char*>(buffer),
                  static_cast<int>(length), flags);
}

int raftSocketRecv(int socket, void* buffer, size_t length, int flags) {
    if (auto session = raftTlsSession(socket)) {
        return session->read(buffer, length);
    }
    if (raftTlsEnabled()) {
        errno = EPROTO;
        return -1;
    }
    return ::recv(socket, static_cast<char*>(buffer), static_cast<int>(length),
                  flags);
}

void closeRaftNetworkSocket(int socket) {
    if (socket < 0) return;
    detachRaftTls(socket);
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

class RaftReadBarrierExecutor {
public:
    explicit RaftReadBarrierExecutor(size_t workers = 0, size_t queueMax = 0) {
        size_t workerCount = workers ? workers : 8;
        size_t queueLimit = queueMax ? queueMax : 4096;
        if (!workers) {
            if (const char* w = std::getenv("RAFT_READ_BARRIER_WORKERS")) {
                try { workerCount = std::max<size_t>(2, std::stoull(w)); } catch (...) {}
            }
        }
        if (!queueMax) {
            if (const char* limit = std::getenv("RAFT_READ_BARRIER_QUEUE_MAX")) {
                try { queueLimit = std::max<size_t>(64, std::stoull(limit)); } catch (...) {}
            }
        }
        queueLimit_ = queueLimit;
        workers_.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mutex_);
                        cv_.wait(lk, [this]() { return stopping_ || !queue_.empty(); });
                        if (stopping_ && queue_.empty()) return;
                        task = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    task();
                }
            });
        }
    }

    ~RaftReadBarrierExecutor() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }

    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (stopping_ || queue_.size() >= queueLimit_) return false;
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    size_t queueLimit_ = 4096;
    bool stopping_ = false;
};

RaftReadBarrierExecutor& raftReadBarrierExecutor() {
    static RaftReadBarrierExecutor executor;
    return executor;
}

// Per-peer read-barrier executors.
//
// The barrier previously submitted every peer's heartbeat into ONE shared pool. A slow
// peer's blocking sendToPeer() occupied workers for its full latency, so under
// concurrency the pool filled with slow-peer tasks and the tasks targeting healthy peers
// sat in the queue past the barrier timeout. The barrier then reported a quorum failure
// while leader + healthy follower were a live quorum the entire time. Giving each peer
// its own executor bounds a peer's slowness to its own queue.
RaftReadBarrierExecutor& raftPeerBarrierExecutor(const std::string& peer) {
    static std::mutex mapMutex;
    static std::map<std::string, std::unique_ptr<RaftReadBarrierExecutor>> executors;
    std::lock_guard<std::mutex> lk(mapMutex);
    auto it = executors.find(peer);
    if (it == executors.end()) {
        // Rounds are coalesced, so a peer sees at most a few concurrent heartbeats.
        it = executors.emplace(peer, std::make_unique<RaftReadBarrierExecutor>(2, 64)).first;
    }
    return *it->second;
}

int raftBarrierRoundTimeoutMs() {
    static const int value = [] {
        const char* v = std::getenv("RAFT_BARRIER_ROUND_TIMEOUT_MS");
        if (!v) return 1000;
        try { return std::max(50, std::stoi(v)); } catch (...) { return 1000; }
    }();
    return value;
}

bool parseStaleTermResponse(const std::string& resp, uint64_t& termOut) {
    const std::string prefixes[] = {"stale_term:", "vote_denied:"};
    for (const auto& prefix : prefixes) {
        if (resp.rfind(prefix, 0) != 0) continue;
        try {
            termOut = std::stoull(resp.substr(prefix.size()));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool isWriteLikeEntry(const json& entry) {
    const std::string action = entry.value("action", "");
    const std::string op = entry.value("op", "");
    auto isWriteLike = [](const std::string& v) {
        return v == "insert" || v == "INSERT" ||
               v == "updateOne" || v == "UPDATE" ||
               v == "deleteOne" || v == "DELETE" ||
               v == "bulk" || v == "bulkInsert" ||
               v == "createDatabase" || v == "createCollection" ||
               v == "dropDatabase" || v == "dropCollection" ||
               v == "insertVector" ||
               v == "split_shard" || v == "rebalance_shards" ||
               v == "CREATE_DB" || v == "CREATE_COLLECTION" ||
               v == "DROP_DB" || v == "DROP_DATABASE" ||
               v == "DROP_COLLECTION";
    };

    return isWriteLike(action) || isWriteLike(op);
}

bool isRaftNoopEntry(const json& entry) {
    return entry.value("type", "") == "raft_noop" || entry.value("action", "") == "raft_noop";
}

struct PersistedRaftEntry {
    uint64_t index = 0;
    uint64_t term = 0;
    json payload;
    uint64_t startOffset = 0;
    uint64_t endOffset = 0;
};

std::string snapshotManifestChecksum(const json& payload) {
    json manifest = json::object();
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        if (it.key() != "files") manifest[it.key()] = it.value();
    }
    manifest["files"] = json::array();
    if (!payload.contains("files") || !payload["files"].is_array()) return {};
    for (const auto& file : payload["files"]) {
        if (!file.is_object() || !file.contains("content") || !file["content"].is_string()) {
            return {};
        }
        json entry = json::object();
        for (auto it = file.begin(); it != file.end(); ++it) {
            if (it.key() != "content") entry[it.key()] = it.value();
        }
        const std::string& content = file["content"].get_ref<const std::string&>();
        entry["contentBytes"] = content.size();
        entry["contentSha256"] =
            pacificdb::durability::ChecksumCalculator::sha256(
                content.data(), content.size());
        manifest["files"].push_back(std::move(entry));
    }
    const std::string encoded = manifest.dump();
    return pacificdb::durability::ChecksumCalculator::sha256(
        encoded.data(), encoded.size());
}

std::string snapshotEnvelopeManifestChecksum(const json& snapshot) {
    json manifest = json::object();
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (it.key() != "lsm_payload" && it.key() != "snapshot_checksum") {
            manifest[it.key()] = it.value();
        }
    }
    const std::string encoded = manifest.dump();
    return pacificdb::durability::ChecksumCalculator::sha256(
        encoded.data(), encoded.size());
}

bool isSha256Hex(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool validateRaftSnapshot(const json& snap, std::string& reason) {
    reason.clear();
    if (!snap.is_object() || snap.value("version", 0) != 1 || !snap.value("complete", false)) {
        reason = "invalid_snapshot_envelope";
        return false;
    }

    if (!snap.contains("lastIncludedIndex") ||
        !snap["lastIncludedIndex"].is_number_unsigned() ||
        !snap.contains("lastIncludedTerm") ||
        !snap["lastIncludedTerm"].is_number_unsigned() ||
        !snap.contains("commitIndex") || !snap["commitIndex"].is_number_unsigned()) {
        reason = "invalid_snapshot_boundary";
        return false;
    }
    const uint64_t included = snap["lastIncludedIndex"].get<uint64_t>();
    if (included == 0 || !snap.contains("lsm_payload") || !snap["lsm_payload"].is_object()) {
        reason = "missing_state_machine_payload";
        return false;
    }
    const json& payload = snap["lsm_payload"];
    const int payloadVersion = payload.value("version", 0);
    if ((payloadVersion != 1 && payloadVersion != 2 && payloadVersion != 3) ||
        payload.value("lastIncludedIndex", static_cast<uint64_t>(0)) != included ||
        !payload.contains("files") || !payload["files"].is_array() ||
        snap["commitIndex"].get<uint64_t>() != included) {
        reason = "state_machine_boundary_mismatch";
        return false;
    }

    if (payloadVersion == 3) {
        if (snap.value("snapshot_format", std::string()) !=
                "pdb-snapshot-bundle-v3" ||
            snap.value("snapshot_checksum_format", std::string()) != "bundle-v3") {
            reason = "snapshot_bundle_format_mismatch";
            return false;
        }
        uint64_t expectedOffset = 0;
        for (const auto& artifact : payload["files"]) {
            if (!artifact.is_object() ||
                !artifact.contains("path") || !artifact["path"].is_string() ||
                artifact["path"].get_ref<const std::string&>().empty() ||
                artifact.value("contentEncoding", std::string()) != "raw" ||
                artifact.contains("content") ||
                !artifact.contains("artifactOffset") ||
                !artifact["artifactOffset"].is_number_unsigned() ||
                !artifact.contains("artifactBytes") ||
                !artifact["artifactBytes"].is_number_unsigned() ||
                !artifact.contains("artifactSha256") ||
                !artifact["artifactSha256"].is_string() ||
                artifact["artifactOffset"].get<uint64_t>() != expectedOffset ||
                !isSha256Hex(artifact["artifactSha256"].get<std::string>())) {
                reason = "snapshot_bundle_artifact_invalid";
                return false;
            }
            const uint64_t bytes = artifact["artifactBytes"].get<uint64_t>();
            if (bytes > std::numeric_limits<uint64_t>::max() - expectedOffset) {
                reason = "snapshot_bundle_artifact_invalid";
                return false;
            }
            expectedOffset += bytes;
        }
        const std::string expected = snap.value("snapshot_checksum", std::string());
        json checksummed = snap;
        checksummed.erase("snapshot_checksum");
        const std::string encoded = checksummed.dump();
        if (!isSha256Hex(expected) || expected !=
            pacificdb::durability::ChecksumCalculator::sha256(
                encoded.data(), encoded.size())) {
            reason = "snapshot_checksum_mismatch";
            return false;
        }
        return true;
    }

    const std::string expectedPayloadChecksum =
        snap.value("lsm_payload_checksum", std::string());
    const bool manifestV2 =
        snap.value("snapshot_checksum_format", std::string()) == "manifest-v2";
    std::string actualPayloadChecksum;
    if (manifestV2) {
        actualPayloadChecksum = snapshotManifestChecksum(payload);
    } else {
        const std::string payloadDump = payload.dump();
        actualPayloadChecksum = pacificdb::durability::ChecksumCalculator::sha256(
            payloadDump.data(), payloadDump.size());
    }
    if (expectedPayloadChecksum.empty() || expectedPayloadChecksum != actualPayloadChecksum) {
        reason = "state_machine_checksum_mismatch";
        return false;
    }

    const std::string expectedSnapshotChecksum = snap.value("snapshot_checksum", std::string());
    std::string actualSnapshotChecksum;
    if (manifestV2) {
        actualSnapshotChecksum = snapshotEnvelopeManifestChecksum(snap);
    } else {
        json envelope = snap;
        envelope.erase("snapshot_checksum");
        const std::string envelopeDump = envelope.dump();
        actualSnapshotChecksum = pacificdb::durability::ChecksumCalculator::sha256(
            envelopeDump.data(), envelopeDump.size());
    }
    if (expectedSnapshotChecksum.empty() || expectedSnapshotChecksum != actualSnapshotChecksum) {
        reason = "snapshot_checksum_mismatch";
        return false;
    }
    return true;
}

bool readTrustedSnapshotChecksum(const std::string& snapshotPath, std::string& checksum) {
    std::ifstream sidecar(snapshotPath + ".sha256");
    checksum.resize(64);
    if (!sidecar.read(checksum.data(), checksum.size()) || !isSha256Hex(checksum) ||
        (sidecar >> std::ws).peek() != std::char_traits<char>::eof()) return false;
    return pacificdb::durability::ChecksumCalculator::sha256File(snapshotPath) == checksum;
}

// The persisted sidecar covers an already validated payload. Skip its bytes,
// retaining only the outer envelope; ordinary SAX parsing still allocates each
// potentially multi-GB payload string before invoking its callback.
json readSnapshotEnvelope(std::istream& input) {
    constexpr size_t kMetadataBytes = 64U * 1024U;
    size_t retained = 0;
    const auto invalid = [] { throw std::runtime_error("snapshot_metadata_invalid"); };
    const auto space = [&] {
        while (input.peek() == ' ' || input.peek() == '\n' ||
               input.peek() == '\r' || input.peek() == '\t') input.get();
    };
    const auto append = [&](std::string& out, char c) {
        if (++retained > kMetadataBytes) invalid();
        out += c;
    };
    const auto value = [&](bool keep) {
        std::string out, closing;
        bool quoted = false, escaped = false;
        space();
        while (true) {
            const int next = input.peek();
            if (next == std::char_traits<char>::eof()) invalid();
            if (!quoted && closing.empty() &&
                (next == ',' || next == '}' || next == ':' ||
                 next == ' ' || next == '\n' || next == '\r' || next == '\t')) break;
            const char c = static_cast<char>(input.get());
            if (keep) append(out, c);
            if (quoted) {
                if (static_cast<unsigned char>(c) < 0x20) invalid();
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') {
                if (closing.size() >= 128) invalid();
                closing += c == '{' ? '}' : ']';
            } else if (c == '}' || c == ']') {
                if (closing.empty() || closing.back() != c) invalid();
                closing.pop_back();
                if (closing.empty()) break;
            }
        }
        return out;
    };
    space();
    if (input.get() != '{') invalid();
    json envelope = json::object();
    while (true) {
        space();
        if (input.peek() != '"') invalid();
        const json key = json::parse(value(true));
        if (!key.is_string()) invalid();
        const std::string name = key.get<std::string>();
        if (envelope.contains(name)) invalid();
        space();
        if (input.get() != ':') invalid();
        space();
        if (name == "lsm_payload") {
            if (input.peek() != '{') invalid();
            value(false);
            envelope[name] = json::object();
        } else {
            envelope[name] = json::parse(value(true));
        }
        space();
        const int separator = input.get();
        if (separator == '}') break;
        if (separator != ',') invalid();
    }
    space();
    if (input.bad() || input.peek() != std::char_traits<char>::eof()) invalid();
    return envelope;
}

bool validateTransferredRaftSnapshot(const json& snap,
                                     uint64_t expectedIndex,
                                     uint64_t expectedTerm,
                                     std::string& reason) {
    reason.clear();
    if (!snap.is_object() || snap.value("version", 0) != 1 || !snap.value("complete", false)) {
        reason = "invalid_snapshot_envelope";
        return false;
    }
    const uint64_t included = snap.value("lastIncludedIndex", static_cast<uint64_t>(0));
    const uint64_t includedTerm = snap.value("lastIncludedTerm", static_cast<uint64_t>(0));
    const bool bundled = snap.value("snapshot_format", std::string()) ==
        "pdb-snapshot-bundle-v3";
    if (included == 0 || included != expectedIndex || includedTerm != expectedTerm ||
        (!bundled && !isSha256Hex(snap.value("snapshot_checksum", std::string()))) ||
        (!bundled && !isSha256Hex(snap.value("lsm_payload_checksum", std::string()))) ||
        !snap.contains("lsm_payload") || !snap["lsm_payload"].is_object()) {
        reason = "snapshot_transfer_boundary_mismatch";
        return false;
    }
    const json& payload = snap["lsm_payload"];
    const int payloadVersion = payload.value("version", 0);
    if ((payloadVersion != 1 && payloadVersion != 2 && payloadVersion != 3) ||
        payload.value("lastIncludedIndex", static_cast<uint64_t>(0)) != included ||
        !payload.contains("files") || !payload["files"].is_array()) {
        reason = "state_machine_boundary_mismatch";
        return false;
    }
    return validateRaftSnapshot(snap, reason);
}

void durableAtomicReplaceFile(const std::string& tempPath,
                              const std::string& finalPath,
                              const std::string& description) {
#ifdef _WIN32
    HANDLE handle = CreateFileA(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE || !FlushFileBuffers(handle)) {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        throw std::runtime_error("cannot fsync " + description + " temp file");
    }
    CloseHandle(handle);
    std::error_code removeEc;
    std::filesystem::remove(finalPath, removeEc);
#else
    int fileFd = open(tempPath.c_str(), O_WRONLY);
    if (fileFd < 0 || fsync(fileFd) != 0) {
        if (fileFd >= 0) close(fileFd);
        throw std::runtime_error("cannot fsync " + description + " temp file");
    }
    close(fileFd);
#endif

    std::error_code renameEc;
    std::filesystem::rename(tempPath, finalPath, renameEc);
    if (renameEc) {
        std::filesystem::remove(tempPath);
        throw std::runtime_error("cannot atomically replace " + description + ": " + renameEc.message());
    }

#ifndef _WIN32
    const std::string parent = std::filesystem::path(finalPath).parent_path().string();
    int dirFd = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirFd < 0) throw std::runtime_error("cannot open " + description + " directory for fsync");
    if (fsync(dirFd) != 0) {
        close(dirFd);
        throw std::runtime_error("cannot fsync " + description + " directory");
    }
    close(dirFd);
#endif
}

void persistRaftSnapshotChecksum(const std::string& snapshotPath, const std::string& checksum) {
    if (!isSha256Hex(checksum)) throw std::runtime_error("invalid Raft snapshot file checksum");
    const std::string path = snapshotPath + ".sha256";
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << checksum << '\n';
    output.flush();
    output.close();
    if (!output) throw std::runtime_error("cannot write Raft snapshot checksum");
    durableAtomicReplaceFile(temporary, path, "Raft snapshot checksum");
}

bool persistRaftSnapshotFile(const std::string& root, const json& snap) {
    try {
        std::string base = root;
        if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";
        const std::string path = base + ".snapshot";
        const std::string tmpPath = path + ".install.tmp";
        std::ofstream out(tmpPath, std::ios::trunc);
        if (!out.is_open()) return false;
        out << snap;
        out.flush();
        out.close();
        if (!out) {
            std::filesystem::remove(tmpPath);
            return false;
        }
        durableAtomicReplaceFile(tmpPath, path, "installed Raft snapshot");
        persistRaftSnapshotChecksum(path,
            pacificdb::durability::ChecksumCalculator::sha256File(path));
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[RAFTCORE] Installed snapshot persistence failed: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[RAFTCORE] Installed snapshot persistence failed: unknown error" << std::endl;
        return false;
    }
}

std::string peerNodeId(const std::string& peer) {
    auto at = peer.find('@');
    if (at == std::string::npos || at == 0) return "";
    return peer.substr(0, at);
}

std::string peerEndpoint(const std::string& peer) {
    auto at = peer.find('@');
    if (at == std::string::npos) return peer;
    return peer.substr(at + 1);
}

json markEntryCommittedVisible(json entry, uint64_t index, uint64_t term) {
    entry["_raft_commit_index"] = index;
    entry["_raft_term"] = term;
    entry["_visibility_floor"] = index;
    entry["committed"] = true;
    entry["_visibility_state"] = "COMMITTED_VISIBLE";

    if (entry.contains("data") && entry["data"].is_object()) {
        entry["data"]["_raft_commit_index"] = index;
        entry["data"]["_raft_term"] = term;
        entry["data"]["_visibility_floor"] = index;
        entry["data"]["committed"] = true;
        const bool tombstone = entry["data"].value("_deleted", false);
        entry["data"]["_visibility_state"] = tombstone ? "TOMBSTONED" : "COMMITTED_VISIBLE";
    }

    return entry;
}

uint64_t commitEpochMs() {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    static std::atomic<uint64_t> last{0};
    uint64_t observed = last.load(std::memory_order_relaxed);
    while (observed < now &&
           !last.compare_exchange_weak(observed, now, std::memory_order_relaxed)) {}
    return std::max(now, observed);
}

uint64_t steadyNowMs() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();

    const auto now = clock::now();
    const auto baseMs = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count());

    const auto envInt = [](const char* name, int64_t defVal) {
        const char* v = std::getenv(name);
        if (!v) return defVal;
        try { return static_cast<int64_t>(std::stoll(v)); } catch (...) { return defVal; }
    };
    static const int64_t offsetMs = envInt("RAFT_TIME_OFFSET_MS", 0);
    static const int64_t driftMsPerSec = envInt("RAFT_TIME_DRIFT_MS_PER_SEC", 0);
    static const int64_t jitterMs = envInt("RAFT_TIME_JITTER_MS", 0);

    int64_t driftMs = 0;
    if (driftMsPerSec != 0) {
        const auto elapsedMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        driftMs = (elapsedMs * driftMsPerSec) / 1000;
    }

    int64_t jitter = 0;
    if (jitterMs > 0) {
        thread_local std::mt19937 rng(static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<int64_t> dist(-jitterMs, jitterMs);
        jitter = dist(rng);
    }

    const int64_t adjusted = baseMs + offsetMs + driftMs + jitter;
    return adjusted < 0 ? 0 : static_cast<uint64_t>(adjusted);
}

int64_t raftEnvIntMs(const char* name, int64_t defVal) {
    const char* v = std::getenv(name);
    if (!v) return defVal;
    try { return static_cast<int64_t>(std::stoll(v)); } catch (...) { return defVal; }
}

int64_t raftEnvIntMs(const char* name) {
    return raftEnvIntMs(name, 0);
}

static bool raftEnvBool(const char* name, bool defVal = false) {
    const char* v = std::getenv(name);
    if (!v) return defVal;
    std::string s(v);
    return !(s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "NO" || s.empty());
}

// v5.5P-R2B FIX C: cap any single peer RPC at a small bound so one slow/unreachable
// follower cannot hold a replication worker for the full 15s operation timeout.
// The overall operation is still bounded by the caller's
// timeout via the quorum-wait deadline; this only limits per-attempt blocking.
static inline int raftPeerRpcTimeoutMs(int opTimeoutMs) {
    static const int cap = [] {
        if (const char* e = std::getenv("RAFT_PEER_RPC_TIMEOUT_MS")) {
            try { return std::max(200, std::stoi(e)); } catch (...) {}
        }
        return 1500;
    }();
    if (opTimeoutMs <= 0) return cap;
    return std::min(opTimeoutMs, cap);
}

// v5.5P-R2B FIX D: shared ack accounting for synchronous replication. Held by a
// shared_ptr so replication workers may be DETACHED once a quorum is reached — a
// straggler (slow/down follower) then finishes in the background instead of blocking
// the leader on a join.
struct RaftAckState {
    std::mutex m;
    std::condition_variable cv;
    int acks = 1;          // self counts
    int needed = 1;
    int finished = 0;      // workers that have returned
    int total = 0;         // workers spawned
};

// Wait until a quorum of acks is observed or the deadline elapses. Returns the ack
// count seen. Does not block beyond timeoutMs even if some peers never answer.
static int raftWaitForQuorum(const std::shared_ptr<RaftAckState>& st, int timeoutMs) {
    auto waitStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(st->m);
    if (timeoutMs <= 0) timeoutMs = 1;
    st->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                    [&]() { return st->acks >= st->needed || st->finished >= st->total; });
    // v5.5P-R8.1: quorum wait percentiles (p50/p95/p99/p999 via exporter histograms).
    auto waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - waitStart).count();
    MetricsExporter::recordHistogram("pacificdb_quorum_wait_us", static_cast<double>(waitUs));
    return st->acks;
}

static bool raftCommitTraceOn() {
    static const bool on = []() {
        const char* e = std::getenv("RAFT_COMMIT_TRACE");
        return e && std::string(e) != "0" && std::string(e) != "";
    }();
    return on;
}
#define RAFT_CT(x) do { if (raftCommitTraceOn()) { std::cerr << "[CT] " << x << std::endl; } } while (0)

static bool raftSchemaTraceOn() {
    static const bool on = []() {
        const char* e = std::getenv("RAFT_SCHEMA_TRACE");
        return e && std::string(e) != "0" && std::string(e) != "";
    }();
    return on;
}

static bool isSchemaEntry(const json& entry) {
    std::string action = entry.value("action", "");
    std::string op = entry.value("op", "");
    std::string legacy = entry.value("legacyOp", "");
    return action == "createDatabase" || action == "createCollection" ||
           action == "dropDatabase" || action == "dropCollection" ||
           op == "createDatabase" || op == "createCollection" ||
           op == "dropDatabase" || op == "dropCollection" ||
           op == "CREATE_DB" || op == "CREATE_COLLECTION" ||
           op == "DROP_DB" || op == "DROP_COLLECTION" ||
           legacy == "CREATE_DB" || legacy == "CREATE_COLLECTION" ||
           legacy == "DROP_DB" || legacy == "DROP_COLLECTION";
}

static std::string schemaTraceId(const json& entry) {
    std::string rid = entry.value("requestId", "");
    if (rid.empty()) rid = entry.value("trace_id", "");
    if (rid.empty()) rid = entry.value("op", entry.value("action", std::string("schema")));
    return rid;
}

#define RAFT_SCHEMA_LOG(tag, entry, msg) do { \
    if (raftSchemaTraceOn() && isSchemaEntry(entry)) { \
        std::cerr << tag << " requestId=" << schemaTraceId(entry) \
                  << " action=" << entry.value("action", std::string("")) \
                  << " op=" << entry.value("op", std::string("")) \
                  << " legacyOp=" << entry.value("legacyOp", std::string("")) \
                  << " user=" << entry.value("userId", std::string("")) \
                  << " db=" << entry.value("db", entry.value("dbName", std::string(""))) \
                  << " collection=" << entry.value("collection", std::string("")) \
                  << " " << msg << std::endl; \
    } \
} while (0)

static std::atomic<int> g_raftWriteInFlight{0};
static std::mutex g_raftCommitOrderMutex;
static std::condition_variable g_raftCommitOrderCv;
static std::mutex g_raftPeerMutexesMutex;
static std::unordered_map<std::string, std::shared_ptr<std::mutex>> g_raftPeerMutexes;
static std::mutex g_raftWriteMetricsMutex;
static std::deque<long long> g_raftReplicationMs;
static std::deque<long long> g_raftQuorumWaitMs;
static std::deque<long long> g_raftQueueWaitMs;
static std::deque<long long> g_raftBatchSize;
static std::deque<long long> g_raftBatchWaitMs;
static std::deque<long long> g_raftBatchQuorumMs;
static std::deque<long long> g_raftBatchApplyMs;
static std::deque<long long> g_raftReplicationSerialWaitMs;
static std::deque<long long> g_replicaIndexMutexWaitUs;
static std::deque<long long> g_applyMutexWaitUs;
static std::deque<long long> g_applyWaitMs;
static std::deque<long long> g_applyEntryDurationUs;
static std::deque<long long> g_applyStorageDurationUs;
static std::deque<long long> g_applyBatchEntries;
static std::deque<long long> g_applyBatchBytes;
static std::deque<long long> g_applyJsonPrepareUs;
static std::atomic<long long> g_raftSlowFollowerCount{0};
static std::atomic<long long> g_raftWriteRejected{0};
static std::atomic<long long> g_raftSingleWriteBypassCount{0};
static std::atomic<long long> g_raftWriteAdmissionRejected{0};
static std::atomic<long long> g_writeLifecycleNew{0};
static std::atomic<long long> g_writeLifecycleAdmitted{0};
static std::atomic<long long> g_writeLifecycleRaftSubmitted{0};
static std::atomic<long long> g_writeLifecycleQuorumReached{0};
static std::atomic<long long> g_writeLifecycleApplied{0};
static std::atomic<long long> g_writeLifecycleResponded{0};
static std::atomic<long long> g_writeLifecycleClientCancelled{0};
static std::atomic<long long> g_writeLifecycleDeadlineExpired{0};
static std::atomic<long long> g_writeLifecycleRejectedBusy{0};
static std::atomic<long long> g_writeLifecycleNotLeader{0};
static std::atomic<long long> g_writeLifecycleFailed{0};
static std::atomic<long long> g_ambiguousLateCommitCount{0};
static std::atomic<long long> g_fencedBeforeCommitCount{0};
static std::atomic<long long> g_idempotentLateCommitCount{0};
static std::atomic<long long> g_idempotencyHits{0};
static std::atomic<long long> g_idempotencyMisses{0};
static std::atomic<long long> g_idempotencyStored{0};
static std::atomic<long long> g_idempotencyEvicted{0};
static std::atomic<long long> g_idempotencyReplayRecovered{0};
static std::atomic<long long> g_duplicateSuppressed{0};
static std::atomic<long long> g_applyAdmissionRejected{0};
static std::atomic<long long> g_applyAdmissionAccepted{0};
static std::atomic<long long> g_applyOverloadActive{0};
static std::atomic<long long> g_applyRetryAfterMs{25};
static std::atomic<uint64_t> g_applyOldestQueuedSinceMs{0};
static std::atomic<long long> g_applyEntryStarted{0};
static std::atomic<long long> g_applyEntrySucceeded{0};
static std::atomic<long long> g_applyEntryFailed{0};
static std::atomic<long long> g_applyEntrySkipped{0};
static std::atomic<long long> g_lastAppliedAdvanceCount{0};
static std::atomic<long long> g_lastAppliedAdvanceWithoutPayloadApply{0};
static std::atomic<long long> g_applyDecodeFailures{0};
static std::atomic<long long> g_applyStorageFailures{0};
static std::atomic<long long> g_applyVisibilityFailures{0};
static std::atomic<long long> g_applyRetryCount{0};
static std::atomic<long long> g_applyFatalStopCount{0};
static std::atomic<long long> g_applyVisibilitySkipped{0};
static std::atomic<long long> g_concurrentApplyDetectedTotal{0};
static std::atomic<long long> g_duplicateApplyAttemptsTotal{0};
static std::atomic<long long> g_outOfOrderApplyAttemptsTotal{0};
static std::atomic<long long> g_backwardLastAppliedAttemptsTotal{0};
static std::atomic<long long> g_skippedApplyIndexTotal{0};
static std::atomic<long long> g_visibilityAheadOfAppliedTotal{0};
static std::atomic<long long> g_activeApplyOwners{0};
static std::atomic<long long> g_applySourceWorkerTotal{0};
static std::atomic<long long> g_applySourceAppendEntriesTotal{0};
static std::atomic<long long> g_applySourceLeaderBypassTotal{0};
static std::atomic<long long> g_applySourceLeaderAsyncTotal{0};
static std::atomic<long long> g_applySourceRecoveryTotal{0};
static std::atomic<long long> g_applySourceStartupTotal{0};
static std::atomic<long long> g_applySourceStrongReadTotal{0};
static std::atomic<long long> g_applySourceSnapshotTotal{0};
static std::atomic<long long> g_storageMarkerChecks{0};
static std::atomic<long long> g_queryVisibilityChecks{0};
static std::deque<long long> g_applyVisibilityCheckUs;
static std::mutex g_raftHeartbeatBackfillMutex;
static std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> g_raftHeartbeatBackfillActive;

static std::string raftShardScopeKey();

static std::shared_ptr<std::atomic<bool>> raftHeartbeatBackfillFlag(const std::string& peer) {
    std::lock_guard<std::mutex> lk(g_raftHeartbeatBackfillMutex);
    auto& flag = g_raftHeartbeatBackfillActive[peer];
    if (!flag) flag = std::make_shared<std::atomic<bool>>(false);
    return flag;
}

// v5.5P-R2C: coalescing per-peer replicator. The R2 design spawned one detached worker
// per write per peer; with the commit-order throttle removed those workers serialized on
// the per-peer mutex and piled up unboundedly (observed: thread count climbing ~100/s to
// >2000, then pthread_create failure / process death under 100 concurrent writers).
// Instead there is at most ONE replicator thread per peer at a time. Writers publish the
// highest index they need (target) and wait on a shared condition variable; the single
// replicator pushes the log suffix to its peer (group commit) and advances matchIndex_,
// notifying waiters. Threads are therefore bounded by peer count, not write concurrency.
struct PeerReplState {
    std::mutex m;
    std::condition_variable cv;
    bool active = false;
    uint64_t target = 0;
    uint64_t lastObservedMatch = 0;
    uint64_t maxQueueDepth = 0;
    long long wakeups = 0;
    long long threadStarts = 0;
    long long idleReuses = 0;
    long long chunksSent = 0;
    long long reconnects = 0;
    long long timeouts = 0;
    long long conflictRetries = 0;
    long long bytesSent = 0;
    long long rejectionCount = 0;
    long long lastAckMs = 0;
    std::deque<long long> ackLatencyMs;
    std::string scopeKey;
    std::string peer;
};
static std::mutex g_peerReplMapMutex;
static std::unordered_map<std::string, std::shared_ptr<PeerReplState>> g_peerReplMap;

static std::shared_ptr<PeerReplState> raftPeerReplState(const std::string& peer) {
    const std::string scopeKey = raftShardScopeKey();
    const std::string mapKey = scopeKey + "|" + peer;
    std::lock_guard<std::mutex> lk(g_peerReplMapMutex);
    auto& s = g_peerReplMap[mapKey];
    if (!s) {
        s = std::make_shared<PeerReplState>();
        s->scopeKey = scopeKey;
        s->peer = peer;
    }
    return s;
}


// v5.5P-R4.2.2 coalescing-replicator tuning.
static uint64_t raftReplicatorMaxChunkEntries() {
    static const uint64_t value = [] {
        const char* e = std::getenv("RAFT_REPLICATOR_MAX_CHUNK_ENTRIES");
        if (!e) e = std::getenv("RAFT_APPEND_BATCH_MAX_ENTRIES");
        if (!e) return uint64_t{256};
        try { return std::max<uint64_t>(1, std::stoull(e)); } catch (...) { return uint64_t{256}; }
    }();
    return value;
}
static bool raftBatchCoalescingReplicatorEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("RAFT_BATCH_COALESCING_REPLICATOR");
        if (!e) return true;
        const std::string s(e);
        return !(s == "0" || s == "false" || s == "FALSE");
    }();
    return enabled;
}
static int raftReplicatorRpcTimeoutMs() {
    static const int value = [] {
        const char* e = std::getenv("RAFT_REPLICATOR_RPC_TIMEOUT_MS");
        if (e) { try { return std::max(100, std::stoi(e)); } catch (...) {} }
        return raftPeerRpcTimeoutMs(0);
    }();
    return value;
}
// Keep a caught-up per-peer replicator alive briefly so the next write can reuse
// both its worker and its established TCP connection. The previous implementation
// called this path "persistent", but immediately exited and closed the socket whenever
// matchIndex reached the current target. At normal write rates that recreated a thread
// and TCP connection for nearly every small burst, directly inflating follower P99.
static int raftReplicatorIdleLingerMs() {
    static const int value = [] {
        const char* e = std::getenv("RAFT_REPLICATOR_IDLE_LINGER_MS");
        if (!e) return 1000;
        try { return std::max(0, std::stoi(e)); } catch (...) { return 1000; }
    }();
    return value;
}
// Per-peer coalescing-replicator metrics.
static std::atomic<long long> g_peerReplicatorChunksSent{0};
static std::atomic<long long> g_peerReplicatorConflictRetries{0};
static std::atomic<long long> g_peerReplicatorWakeups{0};
static std::atomic<long long> g_peerReplicatorSocketCreates{0};
static std::atomic<long long> g_peerReplicatorReconnects{0};
static std::atomic<long long> g_peerReplicatorTimeouts{0};
static std::deque<long long> g_replicatorSocketCreateUs;
static std::deque<long long> g_replicatorSendUs;
static std::deque<long long> g_replicatorAckMs;
static std::deque<long long> g_appendEntriesBatchEntries;
static std::deque<long long> g_appendEntriesBatchBytes;
static std::deque<long long> g_followerAppendEntries;
static std::deque<long long> g_followerAppendBytes;
static std::deque<long long> g_followerAppendUs;
static std::deque<long long> g_followerDurableAppendMs;
static std::deque<long long> g_timeToFirstFollowerAckMs;
static std::deque<long long> g_timeToSecondFollowerAckMs;
static std::deque<long long> g_inboundRaftQueueWaitUs;

static int raftMaxWriteInFlight() {
    static const int value = [] {
        const char* e = std::getenv("RAFT_MAX_WRITE_IN_FLIGHT");
        if (!e) e = std::getenv("MAX_LEADER_WRITE_IN_FLIGHT");
        if (!e) e = std::getenv("ASYNC_WRITE_MAX_IN_FLIGHT_PER_SHARD");
        if (!e) return 256;
        try { return std::max(1, std::stoi(e)); } catch (...) { return 256; }
    }();
    return value;
}

static int raftQuorumWaitTimeoutMs(int opTimeoutMs) {
    static const int cap = [] {
        if (const char* e = std::getenv("RAFT_QUORUM_WAIT_TIMEOUT_MS")) {
            try { return std::max(200, std::stoi(e)); } catch (...) {}
        }
        return 30000;
    }();
    if (opTimeoutMs <= 0) return cap;
    return std::min(opTimeoutMs, cap);
}

static std::shared_ptr<std::mutex> raftPeerMutex(const std::string& peer) {
    std::lock_guard<std::mutex> lk(g_raftPeerMutexesMutex);
    auto& m = g_raftPeerMutexes[peer];
    if (!m) m = std::make_shared<std::mutex>();
    return m;
}

static void raftRecordMetric(std::deque<long long>& q, long long value) {
    q.push_back(value);
    while (q.size() > 2048) q.pop_front();
}

static json raftMetricBlock(const std::deque<long long>& q) {
    json out;
    out["count"] = q.size();
    if (q.empty()) {
        out["p50"] = 0;
        out["p95"] = 0;
        out["p99"] = 0;
        out["max"] = 0;
        return out;
    }
    std::vector<long long> v(q.begin(), q.end());
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) -> long long {
        size_t idx = static_cast<size_t>(std::ceil((p / 100.0) * v.size()));
        if (idx == 0) idx = 1;
        return v[std::min(v.size() - 1, idx - 1)];
    };
    out["p50"] = pct(50);
    out["p95"] = pct(95);
    out["p99"] = pct(99);
    out["p999"] = pct(99.9);
    out["p99_9"] = out["p999"];
    out["max"] = v.back();
    return out;
}

static std::string raftShardScopeKey() {
    static const std::string key = [] {
        const char* cluster = std::getenv("RAFT_CLUSTER_ID");
        const char* shard = std::getenv("RAFT_SHARD_ID");
        const char* node = std::getenv("RAFT_NODE_ID");
        if (!node) node = std::getenv("NODE_ID");
        const char* port = std::getenv("RAFT_LISTEN_PORT");

        std::string value = (cluster && *cluster) ? cluster : "default-cluster";
        value += "/";
        value += (shard && *shard) ? shard : "default-shard";
        value += "/";
        value += (node && *node) ? node : "unknown-node";
        if (port && *port) { value += "@"; value += port; }
        return value;
    }();
    return key;
}

static json raftPeerReplicatorScopedStatus() {
    json shards = json::object();
    const std::string localScope = raftShardScopeKey();
    std::vector<std::shared_ptr<PeerReplState>> states;
    {
        std::lock_guard<std::mutex> lk(g_peerReplMapMutex);
        states.reserve(g_peerReplMap.size());
        for (const auto& kv : g_peerReplMap) {
            if (kv.second) states.push_back(kv.second);
        }
    }

    for (const auto& st : states) {
        if (!st) continue;
        json peerState;
        std::string scope;
        {
            std::lock_guard<std::mutex> lk(st->m);
            scope = st->scopeKey.empty() ? localScope : st->scopeKey;
            uint64_t queueDepth = st->target > st->lastObservedMatch ? st->target - st->lastObservedMatch : 0;
            peerState = {
                {"peer", st->peer},
                {"active", st->active},
                {"targetIndex", st->target},
                {"lastObservedMatch", st->lastObservedMatch},
                {"appendQueueDepth", queueDepth},
                {"maxAppendQueueDepth", st->maxQueueDepth},
                {"inFlightAppends", st->active ? 1 : 0},
                {"wakeups", st->wakeups},
                {"threadStarts", st->threadStarts},
                {"idleReuses", st->idleReuses},
                {"chunksSent", st->chunksSent},
                {"socketReconnectCount", st->reconnects},
                {"timeoutCount", st->timeouts},
                {"conflictRetries", st->conflictRetries},
                {"bytesSent", st->bytesSent},
                {"rejectionCount", st->rejectionCount},
                {"lastAckMs", st->lastAckMs},
                {"ackLatencyMs", raftMetricBlock(st->ackLatencyMs)}
            };
        }
        if (!shards.contains(scope)) {
            shards[scope] = {
                {"shardScope", scope},
                {"peers", json::array()},
                {"appendQueueDepth", 0},
                {"inFlightAppends", 0},
                {"socketReconnectCount", 0},
                {"slowFollower", false},
                {"backpressureState", "NORMAL"}
            };
        }
        shards[scope]["peers"].push_back(peerState);
        shards[scope]["appendQueueDepth"] =
            std::max<uint64_t>(shards[scope].value("appendQueueDepth", 0ULL),
                               peerState.value("appendQueueDepth", 0ULL));
        shards[scope]["inFlightAppends"] =
            shards[scope].value("inFlightAppends", 0) + peerState.value("inFlightAppends", 0);
        shards[scope]["socketReconnectCount"] =
            shards[scope].value("socketReconnectCount", 0LL) + peerState.value("socketReconnectCount", 0LL);
        if (peerState.value("timeoutCount", 0LL) > 0) shards[scope]["slowFollower"] = true;
    }
    return shards;
}

static bool raftPayloadNeedsVisibilityCheck(const json& payload) {
    if (isRaftNoopEntry(payload)) return false;
    const std::string action = payload.value("action", payload.value("op", std::string("")));
    std::string upper = action;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){ return std::toupper(c); });
    if (upper == "CREATE_DB" || upper == "CREATEDATABASE" ||
        upper == "CREATE_COLLECTION" || upper == "CREATECOLLECTION") {
        return false;
    }
    json data = json::object();
    if (payload.contains("data")) data = payload["data"];
    else if (payload.contains("doc")) data = payload["doc"];
    if (!data.is_object()) return false;
    if (data.value("_deleted", false)) return false;
    return data.contains("logicalWriteId") || data.contains("id") ||
           payload.contains("logicalWriteId") || payload.contains("id");
}

static const std::string& raftApplyVisibilityCheckMode() {
    static const std::string mode = [] {
        const char* value = std::getenv("APPLY_VISIBILITY_CHECK_MODE");
        return value && *value ? std::string(value) : std::string("strict_query");
    }();
    return mode;
}

static bool raftVerifyPayloadVisible(const json& payload, std::string& reason) {
    if (!raftPayloadNeedsVisibilityCheck(payload)) {
        g_applyVisibilitySkipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const auto checkStart = std::chrono::steady_clock::now();
    auto recordCheck = [&](bool ok) {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_applyVisibilityCheckUs,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - checkStart).count());
        return ok;
    };
    try {
        const std::string& mode = raftApplyVisibilityCheckMode();
        static const bool strict = raftEnvBool("APPLY_VISIBILITY_STRICT", true);
        if (mode == "storage_marker") {
            g_storageMarkerChecks.fetch_add(1, std::memory_order_relaxed);
            if (strict) return recordCheck(true);
            g_applyVisibilitySkipped.fetch_add(1, std::memory_order_relaxed);
            return recordCheck(true);
        }

        g_queryVisibilityChecks.fetch_add(1, std::memory_order_relaxed);
        const std::string userId = payload.value("userId", "system");
        const std::string dbName = payload.value("db", "");
        const std::string collection = payload.value("collection", "");
        json data = json::object();
        if (payload.contains("data")) data = payload["data"];
        else if (payload.contains("doc")) data = payload["doc"];
        if (userId.empty() || dbName.empty() || collection.empty() || !data.is_object()) {
            reason = "missing_visibility_context";
            return recordCheck(false);
        }

        // Prefer the immutable primary-key index. Checking logicalWriteId first
        // forced a collection scan after every apply even when both values were
        // identical, making strict visibility proof progressively slower as the
        // collection grew.
        // Match the string form LSM::put stores. Requiring is_string() here meant a
        // document with a numeric id skipped every lookup below and was declared
        // invisible even though it had applied cleanly, failing the entry forever and
        // stalling the apply loop — and with it every subsequent write.
        auto storedIdForm = [](const json& value) {
            return value.is_string() ? value.get<std::string>() : value.dump();
        };
        if (data.contains("id") && !data["id"].is_null()) {
            json filter = {{"id", storedIdForm(data["id"])}};
            if (!DatabaseEngine::find(userId, dbName, collection, filter, 1).empty()) return recordCheck(true);
        }
        if (payload.contains("id") && !payload["id"].is_null()) {
            json filter = {{"id", storedIdForm(payload["id"])}};
            if (!DatabaseEngine::find(userId, dbName, collection, filter, 1).empty()) return recordCheck(true);
        }
        if (data.contains("logicalWriteId") && data["logicalWriteId"].is_string()) {
            json filter = {{"logicalWriteId", data["logicalWriteId"].get<std::string>()}};
            if (!DatabaseEngine::find(userId, dbName, collection, filter, 1).empty()) return recordCheck(true);
        }
        if (payload.contains("logicalWriteId") && payload["logicalWriteId"].is_string()) {
            json filter = {{"logicalWriteId", payload["logicalWriteId"].get<std::string>()}};
            if (!DatabaseEngine::find(userId, dbName, collection, filter, 1).empty()) return recordCheck(true);
        }
        reason = "payload_not_visible_after_apply";
        return recordCheck(false);
    } catch (const std::exception& ex) {
        reason = std::string("visibility_check_exception:") + ex.what();
        return recordCheck(false);
    } catch (...) {
        reason = "visibility_check_unknown_exception";
        return recordCheck(false);
    }
}

int64_t raftJitterMs(int64_t rangeMs) {
    if (rangeMs <= 0) return 0;
    thread_local std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int64_t> dist(-rangeMs, rangeMs);
    return dist(rng);
}

void raftMaybeDelayMs(int64_t baseMs, int64_t jitterMs) {
    const int64_t total = baseMs + raftJitterMs(jitterMs);
    if (total <= 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(total));
}

bool raftBatchEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("RAFT_BATCH_ENABLE");
        if (!v) return true;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE");
    }();
    return enabled;
}

bool raftGroupCommitEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("RAFT_GROUP_COMMIT_ENABLED");
        if (!v) return false;
        const std::string s(v);
        return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES";
    }();
    return enabled;
}

size_t raftMaxWriteQueueDepth() {
    static const size_t value = [] {
        const char* v = std::getenv("RAFT_MAX_WRITE_QUEUE_DEPTH");
        if (!v) v = std::getenv("MAX_WRITE_QUEUE_DEPTH_PER_SHARD");
        if (!v) v = std::getenv("ASYNC_WRITE_MAX_PENDING_PER_SHARD");
        if (!v) return size_t{2048};
        try { return std::max<size_t>(1, static_cast<size_t>(std::stoul(v))); } catch (...) { return size_t{2048}; }
    }();
    return value;
}

size_t raftBatchMaxCount() {
    static const size_t value = [] {
        const char* v = std::getenv("RAFT_BATCH_MAX_COUNT");
        if (!v) return size_t{128};
        try { return std::max<size_t>(1, static_cast<size_t>(std::stoul(v))); } catch (...) { return size_t{128}; }
    }();
    return value;
}

size_t raftBatchMaxBytes() {
    static const size_t value = [] {
        const char* v = std::getenv("RAFT_BATCH_MAX_BYTES");
        if (!v) return size_t{1024 * 1024};
        try { return std::max<size_t>(1024, static_cast<size_t>(std::stoul(v))); } catch (...) { return size_t{1024 * 1024}; }
    }();
    return value;
}

int raftBatchWindowMs() {
    static const int value = [] {
        const char* v = std::getenv("RAFT_BATCH_WINDOW_MS");
        if (!v) return 2;
        try { return std::max(0, std::stoi(v)); } catch (...) { return 2; }
    }();
    return value;
}

int raftStrongReadLeaseMs() {
    static const int value = [] {
        const char* v = std::getenv("RAFT_STRONG_READ_LEASE_MS");
        if (!v) return 0;
        try { return std::max(0, std::stoi(v)); } catch (...) { return 0; }
    }();
    return value;
}

bool raftApplyWorkerEnabled() {
    // Ordered state-machine application is a safety property, not a tuning
    // switch. Older builds allowed false here and moved apply work back onto
    // RPC/caller threads, recreating multiple mutation owners.
    return true;
}

size_t raftApplyBatchMaxEntries() {
    static const size_t value = [] {
        const char* v = std::getenv("RAFT_APPLY_BATCH_MAX_ENTRIES");
        if (!v) return size_t{256};
        try { return std::max<size_t>(1, static_cast<size_t>(std::stoul(v))); } catch (...) { return size_t{256}; }
    }();
    return value;
}

size_t raftApplyBatchMaxBytes() {
    static const size_t value = [] {
        const char* v = std::getenv("RAFT_APPLY_BATCH_MAX_BYTES");
        if (!v) return size_t{1024 * 1024};
        try { return std::max<size_t>(1024, static_cast<size_t>(std::stoul(v))); } catch (...) { return size_t{1024 * 1024}; }
    }();
    return value;
}

int raftApplyBatchMaxMs() {
    static const int value = [] {
        const char* v = std::getenv("RAFT_APPLY_BATCH_MAX_MS");
        if (!v) return 2;
        try { return std::max(0, std::stoi(v)); } catch (...) { return 2; }
    }();
    return value;
}

bool applyAdmissionEnabled() {
    static const bool enabled = raftEnvBool("APPLY_ADMISSION_ENABLED", false);
    return enabled;
}

uint64_t applyAdmissionMaxLagEntries() {
    static const uint64_t value = [] {
        const char* v = std::getenv("APPLY_MAX_LAG_ENTRIES");
        if (!v) return uint64_t{1000};
        try { return std::max<uint64_t>(1, std::stoull(v)); } catch (...) { return uint64_t{1000}; }
    }();
    return value;
}

uint64_t applyAdmissionMaxQueueDepth() {
    static const uint64_t value = [] {
        const char* v = std::getenv("APPLY_MAX_QUEUE_DEPTH");
        if (!v) return applyAdmissionMaxLagEntries();
        try { return std::max<uint64_t>(1, std::stoull(v)); } catch (...) { return applyAdmissionMaxLagEntries(); }
    }();
    return value;
}

long long applyAdmissionMaxWaitP99Ms() {
    static const long long value = [] {
        const char* v = std::getenv("APPLY_MAX_WAIT_P99_MS");
        if (!v) return 3000LL;
        try { return std::max<long long>(1, std::stoll(v)); } catch (...) { return 3000LL; }
    }();
    return value;
}

long long applyAdmissionRetryAfterMs() {
    static const long long value = std::max<int64_t>(1, raftEnvIntMs("APPLY_OVERLOAD_RETRY_AFTER_MS", 25));
    return value;
}

long long applyAdmissionRetryJitterMs() {
    static const long long value = std::max<int64_t>(0, raftEnvIntMs("APPLY_OVERLOAD_MAX_JITTER_MS", 0));
    return value;
}

long long metricP99Value(const std::deque<long long>& q) {
    if (q.empty()) return 0;
    std::vector<long long> v(q.begin(), q.end());
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::ceil(0.99 * v.size()));
    if (idx == 0) idx = 1;
    return v[std::min(v.size() - 1, idx - 1)];
}

bool raftBypassActive() {
    static const bool explicitlyEnabled = [] {
        const char* v = std::getenv("RAFT_BYPASS_SINGLE_NODE");
        if (!v) return false;
        const std::string s(v);
        return s == "1" || s == "true" || s == "TRUE";
    }();
    if (explicitlyEnabled) return true;
    // If no explicit env var set, allow automatic bypass when no peers are configured.
    try {
        if (RaftCore::instance().peerCount() == 0) return true;
    } catch (...) {}
    return false;
}

size_t raftReplicationRateLimitBytesPerSec() {
    static const size_t value = [] {
        const char* v = std::getenv("RAFT_REPL_RATE_LIMIT_BPS");
        if (!v) return size_t{0};
        try { return static_cast<size_t>(std::stoull(v)); } catch (...) { return size_t{0}; }
    }();
    return value;
}

size_t raftSnapshotChunkBytes() {
    static const size_t value = [] {
        const char* v = std::getenv("RAFT_SNAPSHOT_CHUNK_BYTES");
        if (!v) return size_t{4 * 1024 * 1024};
        try { return std::max<size_t>(32 * 1024, static_cast<size_t>(std::stoull(v))); }
        catch (...) { return size_t{4 * 1024 * 1024}; }
    }();
    return value;
}

void raftThrottleBytes(size_t bytes) {
    size_t rate = raftReplicationRateLimitBytesPerSec();
    if (rate == 0 || bytes == 0) return;

    using clock = std::chrono::steady_clock;
    static std::mutex limiterMu;
    static clock::time_point nextAllowed = clock::now();

    std::lock_guard<std::mutex> lk(limiterMu);
    auto now = clock::now();
    if (now < nextAllowed) {
        std::this_thread::sleep_for(nextAllowed - now);
        now = clock::now();
    }

    auto micros = static_cast<long long>((static_cast<long double>(bytes) * 1000000.0L) /
                                         static_cast<long double>(rate));
    if (micros < 1) micros = 1;
    nextAllowed = now + std::chrono::microseconds(micros);
}

struct SnapshotChunkTransfer {
    uint64_t term = 0;
    uint64_t lastIncludedIndex = 0;
    uint64_t lastIncludedTerm = 0;
    size_t totalChunks = 0;
    size_t nextChunk = 0;
    std::string tempPath;
    std::string payloadSha256;
    std::chrono::steady_clock::time_point updatedAt;
};

std::mutex g_snapshotChunkMu;
std::unordered_map<std::string, SnapshotChunkTransfer> g_snapshotChunkTransfers;
std::atomic<uint64_t> g_snapshotTransfersCompleted{0};

std::string snapshotTransferKey(const std::string& leader, uint64_t transferId) {
    return leader + ":" + std::to_string(transferId);
}

void pruneSnapshotChunkTransfers() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_snapshotChunkMu);
    for (auto it = g_snapshotChunkTransfers.begin(); it != g_snapshotChunkTransfers.end();) {
        auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.updatedAt).count();
        if (ageMs > 120000) {
            std::error_code ec;
            std::filesystem::remove(it->second.tempPath, ec);
            it = g_snapshotChunkTransfers.erase(it);
        } else {
            ++it;
        }
    }
}
}

// Suppress high-frequency Raft logs unless RAFT_DEBUG=1
static bool g_raftDebug = false;
#define RAFT_DLOG(x) do { if (g_raftDebug) { std::cout << x; } } while(0)

static bool raftTraceEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("RAFT_TRACE");
        const char* c = std::getenv("CONSISTENCY_TRACE");
        return (v && std::string(v) == "1") || (c && std::string(c) == "1");
    }();
    return enabled;
}

#define RAFT_TLOG(x) do { if (raftTraceEnabled()) { std::cout << "[RAFTTRACE] " << x; } } while(0)

// ── v5.5P-R3: async/group raft-log fsync ───────────────────────────────────────
// The commit round previously fsync'd the raft log inline on BOTH the leader (per
// write) and every follower (per AppendEntries) before acking. With a single serial
// replicator per peer that fsync sits squarely on the critical path and capped pure
// writes at ~600/s regardless of write concurrency. When RAFT_ASYNC_LOG_FSYNC=1 the
// inline fsync is replaced by a single background thread that fdatasync's the log
// every RAFT_ASYNC_LOG_FSYNC_MS (default 5ms). Acked data still reaches a quorum's
// page cache before the client ack (survives a process crash on any node and the
// total loss of any single node); only a simultaneous power loss of a quorum within
// the flush window can drop an acked write. Opt-in, off by default.
static bool raftAsyncLogFsyncEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("RAFT_ASYNC_LOG_FSYNC");
        return v && (std::string(v) == "1" || std::string(v) == "true" || std::string(v) == "TRUE");
    }();
    return enabled;
}
static int raftAsyncLogFsyncMs() {
    static const int value = [] {
        const char* v = std::getenv("RAFT_ASYNC_LOG_FSYNC_MS");
        if (!v) return 5;
        try { return std::max(1, std::stoi(v)); } catch (...) { return 5; }
    }();
    return value;
}
// Toggle for the Nagle fix (default on). Lets us A/B the win and bail out if a
// platform ever misbehaves with TCP_NODELAY on the raft sockets.
static bool raftTcpNoDelayEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("RAFT_TCP_NODELAY");
        if (!v) return true;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE");
    }();
    return enabled;
}
// v5.5P-R5.6: persistent replicator connections. When enabled (default), the per-peer
// replicator reuses one long-lived TCP connection across AppendEntries rounds instead of
// connect()+close() per RPC, and the follower handler serves multiple framed requests per
// connection. This removes the per-round connect/accept cost that dominated the C-led
// replication tail (5-6.5s) on 4-core nodes. Can be disabled for regression isolation.
static bool raftReplicatorReuseConnection() {
    static const bool enabled = [] {
        const char* v = std::getenv("RAFT_REPLICATOR_REUSE_CONNECTION");
        if (!v) return true;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE");
    }();
    return enabled;
}
static void raftStartLogFsyncer(const std::string& logPath) {
    static std::once_flag once;
    std::call_once(once, [logPath]() {
        std::thread([logPath]() {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(raftAsyncLogFsyncMs()));
                syncRaftLog(logPath);
            }
        }).detach();
    });
}
// Durably sync the raft log, or defer to the background fsyncer in async mode.
static inline void raftFdatasyncFile(const std::string& logPath) {
    syncRaftLog(logPath);
}

static inline void raftSyncOrDeferLog(const std::string& logPath) {
    if (raftAsyncLogFsyncEnabled()) { raftStartLogFsyncer(logPath); return; }
    raftFdatasyncFile(logPath);
}

// v5.5P-R4.4: Raft-log GROUP FSYNC for the single-write path. A single fdatasync() of
// raft/log.bin durably persists EVERY entry appended before it, so concurrent writers can
// share one fsync instead of each paying ~ms of disk latency (the dominant write-tail cost
// at 1000 users on 4-core boxes). This keeps one logical Raft entry per write and the fast
// single-write coalescing replication path — only the durability fsync is coalesced.
static bool raftLogGroupFsyncEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("RAFT_LOG_GROUP_FSYNC_ENABLED");
        if (!v) return false;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE");
    }();
    return enabled;
}
static long raftLogGroupFsyncWindowUs() {
    static const long value = [] {
        const char* v = std::getenv("RAFT_LOG_GROUP_FSYNC_WINDOW_US");
        if (!v) return 500L;
        try { return std::max(0L, std::stol(v)); } catch (...) { return 500L; }
    }();
    return value;
}
// Monotonic "highest index durably appended to log.bin" (file-scope so the coordinator is
// shared by all writers). Bumped (fetch_max) right after a writer's PHASE-1 append.
static std::atomic<uint64_t> g_logAppendSeq{0};
static inline void raftLogAppendSeqBump(uint64_t idx) {
    uint64_t cur = g_logAppendSeq.load(std::memory_order_relaxed);
    while (cur < idx && !g_logAppendSeq.compare_exchange_weak(cur, idx, std::memory_order_acq_rel)) {}
}
struct RaftLogFsyncCoord {
    std::mutex m;
    std::condition_variable cv;
    uint64_t fsyncedSeq = 0;   // entries <= this are durable
    bool running = false;
};
static RaftLogFsyncCoord g_logFsyncCoord;
static std::atomic<long long> g_raftLogFsyncCount{0};
static std::atomic<long long> g_raftLogFsyncBypassCount{0};
static std::mutex g_raftLogFsyncMetricsMutex;
static std::deque<long long> g_raftLogFsyncGroupSize;
static std::deque<long long> g_raftLogFsyncWaitMs;
static std::deque<long long> g_raftLogFsyncDurationMs;

// Ensure the writer's entry (myIndex) is durable, coalescing the fdatasync across
// concurrent writers. Returns only after a fdatasync that began AFTER myIndex was appended
// has completed — so durability is never reported early.
static void raftGroupLogFsync(const std::string& logPath, uint64_t myIndex) {
    if (!raftLogGroupFsyncEnabled()) {
        g_raftLogFsyncBypassCount.fetch_add(1, std::memory_order_relaxed);
        raftSyncOrDeferLog(logPath);
        // Track durability for any concurrent group waiters that may be enabled later.
        raftLogAppendSeqBump(myIndex);
        { std::lock_guard<std::mutex> lk(g_logFsyncCoord.m);
          if (myIndex > g_logFsyncCoord.fsyncedSeq) g_logFsyncCoord.fsyncedSeq = myIndex; }
        return;
    }
    const long windowUs = raftLogGroupFsyncWindowUs();
    auto enqueued = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(g_logFsyncCoord.m);
    while (g_logFsyncCoord.fsyncedSeq < myIndex) {
        if (!g_logFsyncCoord.running) {
            g_logFsyncCoord.running = true;
            uint64_t prevSynced = g_logFsyncCoord.fsyncedSeq;
            lk.unlock();
            if (windowUs > 0) std::this_thread::sleep_for(std::chrono::microseconds(windowUs));
            uint64_t target = g_logAppendSeq.load(std::memory_order_acquire);
            if (target < myIndex) target = myIndex; // our entry is in the file
            auto t0 = std::chrono::steady_clock::now();
            raftFdatasyncFile(logPath);
            auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            lk.lock();
            if (target > g_logFsyncCoord.fsyncedSeq) g_logFsyncCoord.fsyncedSeq = target;
            g_logFsyncCoord.running = false;
            g_raftLogFsyncCount.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> ml(g_raftLogFsyncMetricsMutex);
                raftRecordMetric(g_raftLogFsyncGroupSize, (long long)(target - prevSynced));
                raftRecordMetric(g_raftLogFsyncDurationMs, durMs);
            }
            g_logFsyncCoord.cv.notify_all();
        } else {
            g_logFsyncCoord.cv.wait(lk);
        }
    }
    auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - enqueued).count();
    { std::lock_guard<std::mutex> ml(g_raftLogFsyncMetricsMutex);
      raftRecordMetric(g_raftLogFsyncWaitMs, waitMs); }
}

RaftCore& RaftCore::instance() {
    static RaftCore r;
    return r;
}

RaftCore::RaftCore() {
    // Initialize health monitoring
    health_.lastHealthCheck = std::chrono::steady_clock::now();
    // Seed RNG for election jitter
    try {
        std::random_device rd;
        rng_.seed(rd());
    } catch (...) {
        rng_.seed( static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()) );
    }
}

// Helper to check if a peer address is self (to avoid connecting to ourselves)
bool RaftCore::isSelfPeer(const std::string& peer) const {
    std::string explicitNodeId = peerNodeId(peer);
    if (!explicitNodeId.empty() && explicitNodeId == nodeId_) {
        return true;
    }

    std::string endpoint = peerEndpoint(peer);
    // Extract host and port from peer address
    auto pos = endpoint.rfind(":");
    if (pos == std::string::npos) return false;

    std::string host = endpoint.substr(0, pos);
    std::string portStr = endpoint.substr(pos + 1);

    int peerPort = -1;
    try {
        peerPort = std::stoi(portStr);
    } catch (...) {
        return false;
    }

    if (peerPort != listenPort_) {
        return false;
    }

    // Only treat as self if it's clearly loopback/local host
    if (host == "127.0.0.1" || host == "localhost" || host == "::1") {
        return true;
    }

    return false;
}

int RaftCore::clusterNodeCount() const {
    int remotePeers = 0;
    for (const auto& p : peers_) {
        if (!isSelfPeer(p)) {
            ++remotePeers;
        }
    }
    return remotePeers + 1; // include self
}

int RaftCore::quorumSize() const {
    int total = clusterNodeCount();
    return (total / 2) + 1;
}

RaftCore::~RaftCore() {
    stop();
}

void RaftCore::init(const std::vector<std::string>& peers, const std::string& nodeId, int listenPort, bool startAsLeader) {
    // Establish both the inbound and outbound contexts before any listener or
    // peer connection can start. Invalid certificates fail startup instead of
    // silently falling back to plaintext Raft.
    initializeRaftTls();
    peers_ = peers;
    nodeId_ = nodeId;
    listenPort_ = listenPort;
    leaderEligible_ = raftEnvironmentFlag("RAFT_LEADER_ELIGIBLE", true);
    if (startAsLeader && !leaderEligible_) {
        throw std::runtime_error(
            "RAFT_IS_LEADER=true conflicts with RAFT_LEADER_ELIGIBLE=false");
    }
    leader_.store(startAsLeader);
    initialized_.store(true);

    // Check RAFT_DEBUG env var
    const char* raftDebugEnv = std::getenv("RAFT_DEBUG");
    if (raftDebugEnv && std::string(raftDebugEnv) == "1") {
        g_raftDebug = true;
    }

    std::string base = dataRoot();
    std::filesystem::create_directories(base + "/raft");

    // Load persisted state
    recoveryComplete_.store(false);
    loadPersistedState();

    // Existing installations may hold a large JSON/base64 snapshot. A clean
    // root already has fully flushed LSM state, so replace that legacy file
    // before opening the Raft listener. The startup marker remains present
    // during conversion: if power fails, the next start safely retries instead
    // of attempting the legacy high-memory restore.
    const std::filesystem::path persistedSnapshot =
        std::filesystem::path(base) / ".snapshot";
    if (cleanShutdownRecovery_.load(std::memory_order_acquire) &&
        std::filesystem::is_regular_file(persistedSnapshot) &&
        !pacificdb::snapshot_bundle::isBundle(persistedSnapshot)) {
        if (createBackupSnapshot() == 0 ||
            !pacificdb::snapshot_bundle::isBundle(persistedSnapshot)) {
            throw std::runtime_error("cannot migrate legacy Raft snapshot");
        }
        std::cout << "[RAFTCORE] Migrated legacy Raft snapshot to bounded bundle format"
                  << std::endl;
    }

    // Read max workers from env (default 256)
    const char* maxWEnv = std::getenv("RAFT_MAX_WORKERS");
    if (maxWEnv) {
        try { maxWorkers_.store(std::max(size_t(1), (size_t)std::stoi(maxWEnv))); } catch (...) {}
    }
    // v5.5P-R2B: cap on concurrent inbound Raft connection handlers (bounds fds/threads).
    raftMaxWorkers_ = 16;
    if (maxWEnv) { try { raftMaxWorkers_ = std::max(4, std::stoi(maxWEnv)); } catch (...) {} }
    // Read min workers from env (default 16)
    const char* minWEnv = std::getenv("RAFT_MIN_WORKERS");
    if (minWEnv) {
        try { minWorkers_.store(std::max(size_t(1), (size_t)std::stoi(minWEnv))); } catch (...) {}
    }

    // Worker threads must be started only after running_=true in start().
    // Starting them here races with running_=false and lets them exit before
    // the node can ever drain the write queue.
    size_t configuredWorkers = std::min(maxWorkers_.load(), std::max(size_t(32), minWorkers_.load()));
    std::cout << "[RAFTCORE] Worker pool configured initial=" << configuredWorkers
              << " max=" << maxWorkers_.load() << "\n";

        // Initialize per-peer replication indices
        std::lock_guard<std::mutex> lk(replicaIndexMutex_);
        uint64_t startNext = lastIndex_ + 1;
        for (const auto &p : peers_) {
            nextIndex_[p] = startNext;
            matchIndex_[p] = 0;
        }

    std::cout << "[RAFTCORE] Production Raft initialized node=" << nodeId_
              << " leader=" << (leader_.load() ? "true" : "false")
              << " peers=" << peers_.size()
              << " configuredWorkers=" << configuredWorkers
              << " dataRoot=" << base << std::endl;
}

void RaftCore::start() {
    if (!initialized_.load()) return;
    if (running_.exchange(true)) return;

    // Read legacy replication mode for backward compatibility
    const char* mode = std::getenv("RAFT_REPLICATION_MODE");
    if (mode) {
        if (std::string(mode) == "async") {
            replicationMode_.store(ReplicationMode::ASYNCHRONOUS);
        } else if (std::string(mode) == "sync") {
            replicationMode_.store(ReplicationMode::SYNCHRONOUS);
        } else {
            replicationMode_.store(ReplicationMode::HYBRID_ADAPTIVE);
        }
    }

    // Initialize lastHeartbeat with the same monotonic clock used by monitorLoop().
    // A previous wall-clock value compared against steadyNowMs() pushed the first
    // election deadline far into the future, leaving a fresh 3-node group stuck as
    // followers after restart.
    lastHeartbeat_.store(steadyNowMs());

    // Initialize worker pool after running_=true. Older builds started worker
    // threads in init(); if any stale pre-start threads exist, join and replace
    // them so enqueue/dequeue cannot silently stall.
    if (!workerThreads_.empty() && activeWorkers_.load() == 0) {
        for (auto& t : workerThreads_) {
            if (t.joinable()) t.join();
        }
        workerThreads_.clear();
    }
    size_t initialWorkers = std::min(maxWorkers_.load(), std::max(size_t(32), minWorkers_.load()));
    resizeWorkerPool(initialWorkers);

    // Start core threads
    listenerThread_ = std::thread([this]() { runListener(); });
    monitorThread_ = std::thread([this]() { monitorLoop(); });
    snapshotThread_ = std::thread([this]() { snapshotLoop(); });
    healthMonitorThread_ = std::thread([this]() { healthMonitorLoop(); });
    adaptiveControllerThread_ = std::thread([this]() { adaptiveControllerLoop(); });
    applyWorkerThread_ = std::thread([this]() { applyWorkerLoop(); });

    barrierRoundThread_ = std::thread([this]() { barrierRoundLoop(); });

    std::cout << "[RAFTCORE] Production Raft started (leader=" << (leader_.load() ? "true" : "false")
              << ") mode=" << static_cast<int>(replicationMode_.load())
              << ") circuit=" << static_cast<int>(circuitState_.load())
              << ") workers=" << workerThreads_.size() << std::endl;
}

void RaftCore::stop() {
    if (!running_.exchange(false)) return;
    const auto stopStartedAt = std::chrono::steady_clock::now();
    const auto elapsedMs = [&stopStartedAt]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stopStartedAt).count();
    };

    // Signal all threads to stop
    shutdownCv_.notify_all();
    queueCV_.notify_all();
    applyWorkerCv_.notify_all();
    const std::intptr_t listenerSocket = raftListenerSocket_.exchange(
        -1, std::memory_order_acq_rel);
    if (listenerSocket >= 0) {
#ifdef _WIN32
        shutdown(static_cast<SOCKET>(listenerSocket), SD_BOTH);
        closesocket(static_cast<SOCKET>(listenerSocket));
#else
        shutdown(static_cast<int>(listenerSocket), SHUT_RDWR);
        close(static_cast<int>(listenerSocket));
#endif
    }
    // Wake any caught-up per-peer worker that is retaining its connection during
    // the idle reuse window. Workers observe running_=false and exit.
    {
        std::lock_guard<std::mutex> lk(g_peerReplMapMutex);
        for (const auto& kv : g_peerReplMap) {
            if (kv.second) kv.second->cv.notify_all();
        }
    }

    // Join worker threads
    std::cerr << "[RAFTCORE] stopping operation workers" << std::endl;
    for (auto& t : workerThreads_) {
        if (t.joinable()) t.join();
    }
    workerThreads_.clear();
    std::cerr << "[RAFTCORE] operation workers stopped elapsed_ms="
              << elapsedMs() << std::endl;

    // Join core threads
    auto joinThread = [&elapsedMs](std::thread& thread, const char* name) {
        if (!thread.joinable()) return;
        std::cerr << "[RAFTCORE] stopping " << name << std::endl;
        thread.join();
        std::cerr << "[RAFTCORE] stopped " << name
                  << " elapsed_ms=" << elapsedMs() << std::endl;
    };
    joinThread(listenerThread_, "listener");
    joinThread(monitorThread_, "monitor");
    joinThread(snapshotThread_, "snapshot maintenance");
    joinThread(healthMonitorThread_, "health monitor");
    joinThread(adaptiveControllerThread_, "adaptive controller");
    joinThread(applyWorkerThread_, "apply worker");
    barrierRoundCv_.notify_all();
    joinThread(barrierRoundThread_, "read barrier");

    // commitIndex and lastApplied are volatile Raft state.  Checkpoint them on a
    // clean stop to shorten replay, but never put this extra fsync in the client
    // acknowledgement path: raft/log.bin was already made durable before quorum.
    try {
        persistProgress();
    } catch (const std::exception& error) {
        std::cerr << "[RAFTCORE] final progress checkpoint failed: "
                  << error.what() << std::endl;
    }
}

bool RaftCore::waitForShutdown(std::chrono::milliseconds delay) {
    std::unique_lock<std::mutex> lock(shutdownMutex_);
    return shutdownCv_.wait_for(lock, delay, [this]() {
        return !running_.load(std::memory_order_acquire);
    });
}

uint64_t RaftCore::getCurrentTerm() {
    std::lock_guard<std::mutex> lk(electionMutex_);
    return currentTerm_;
}

uint64_t RaftCore::getCommitIndex() {
    // v5.5P-R6.10: lock-free read. Every find()/read calls this for the commit-visibility
    // check; locking electionMutex_ here serialized reads behind the write path.
    return commitIndex_.load(std::memory_order_acquire);
}

uint64_t RaftCore::getLastIndex() {
    std::lock_guard<std::mutex> lk(logMutex_);
    return lastIndex_;
}

uint64_t RaftCore::getLastLogTerm() {
    std::lock_guard<std::mutex> lk(logMutex_);
    return lastEntryTerm_;
}

uint64_t RaftCore::getLastApplied() {
    return lastApplied_.load();
}

uint64_t RaftCore::createBackupSnapshot() {
    if (!initialized_.load(std::memory_order_acquire) ||
        !recoveryComplete_.load(std::memory_order_acquire)) {
        return 0;
    }
    std::lock_guard<std::mutex> snapshotGuard(snapshotMutex_);
    uint64_t stableApplied = 0;
    std::filesystem::path pinnedRoot;
    try {
        {
            std::unique_lock<std::mutex> applyGuard(applyMutex_);
            stableApplied = lastApplied_.load(std::memory_order_acquire);
            if (stableApplied == 0) return 0;
            pinnedRoot = LSM::pinSnapshotFiles(stableApplied);
        }
        const bool created = createSnapshot(stableApplied, pinnedRoot);
        LSM::discardSnapshotFiles(pinnedRoot);
        return created ? stableApplied : 0;
    } catch (const std::exception& error) {
        std::cerr << "[RAFTCORE] Backup snapshot capture failed: "
                  << error.what() << std::endl;
        LSM::discardSnapshotFiles(pinnedRoot);
        return 0;
    } catch (...) {
        LSM::discardSnapshotFiles(pinnedRoot);
        return 0;
    }
}

json RaftCore::decodePersistedLogPayload(const std::string& payload) {
    return decodeRaftPayloadOrThrow(payload);
}

bool RaftCore::validatePersistedSnapshot(const json& snapshot, std::string& reason) {
    return validateRaftSnapshot(snapshot, reason);
}

bool RaftCore::readPersistedSnapshotMetadata(
    const std::string& snapshotPath,
    uint64_t& lastIncludedIndex,
    uint64_t& lastIncludedTerm,
    std::string& reason) {
    lastIncludedIndex = 0;
    lastIncludedTerm = 0;
    reason.clear();

    std::string checksum;
    if (!readTrustedSnapshotChecksum(snapshotPath, checksum)) {
        reason = "snapshot_file_checksum_mismatch";
        return false;
    }

    std::ifstream input(snapshotPath, std::ios::binary);
    if (!input) {
        reason = "snapshot_file_unreadable";
        return false;
    }
    try {
        const json envelope = pacificdb::snapshot_bundle::isBundle(snapshotPath)
            ? pacificdb::snapshot_bundle::read(snapshotPath).manifest
            : readSnapshotEnvelope(input);
        for (const char* key : {"version", "commitIndex", "lastIncludedIndex", "lastIncludedTerm"}) {
            if (!envelope.contains(key) || !envelope[key].is_number_unsigned()) {
                throw std::runtime_error("snapshot_metadata_invalid");
            }
        }
        if (envelope["version"] != 1 || !envelope.contains("complete") ||
            envelope["complete"] != true || !envelope.contains("lsm_payload") ||
            envelope["lastIncludedIndex"] == 0 ||
            envelope["commitIndex"] != envelope["lastIncludedIndex"]) {
            throw std::runtime_error("snapshot_metadata_invalid");
        }
        lastIncludedIndex = envelope["lastIncludedIndex"].get<uint64_t>();
        lastIncludedTerm = envelope["lastIncludedTerm"].get<uint64_t>();
    } catch (const std::exception&) {
        reason = "snapshot_metadata_invalid";
        return false;
    }
    return true;
}

long long RaftCore::getAmbiguousLateCommitCount() {
    return g_ambiguousLateCommitCount.load(std::memory_order_relaxed);
}

std::string RaftCore::getNodeId() {
    std::lock_guard<std::mutex> lk(electionMutex_);
    return nodeId_;
}

std::string RaftCore::getVotedFor() {
    std::lock_guard<std::mutex> lk(electionMutex_);
    return votedFor_;
}

std::string RaftCore::getLeaderId() {
    std::lock_guard<std::mutex> lk(electionMutex_);
    return leaderId_;
}

std::string RaftCore::getRole() {
    if (leader_.load()) return "leader";
    return "follower";
}

uint64_t RaftCore::getLastHeartbeatMs() const {
    return lastHeartbeat_.load();
}

uint64_t RaftCore::millisSinceLastHeartbeat() const {
    uint64_t now = steadyNowMs();
    uint64_t last = lastHeartbeat_.load();
    if (last == 0 || now < last) return std::numeric_limits<uint64_t>::max();
    return now - last;
}

bool RaftCore::applyCommittedUpTo(uint64_t upToIndex) {
    if (upToIndex == 0) return true;
    return applyCommittedOrWait(
        upToIndex, upToIndex, 30000, ApplySource::STRONG_READ);
}

// ═══════════════════════════════════════════════════════════════
// RAFT HARDENING: Quorum health and leader lease methods
// These are critical for split-brain prevention and read safety.
// ═══════════════════════════════════════════════════════════════

bool RaftCore::hasQuorum() {
    if (!initialized_.load() || !leader_.load()) return false;

    static const int electionTimeoutMs = [] {
        if (const char* env = std::getenv("RAFT_ELECTION_TIMEOUT_MS")) {
            try { return std::max(500, std::stoi(env)); } catch (...) {}
        }
        return 3200;
    }();

    uint64_t nowMs = steadyNowMs();
    int remotePeers = 0;
    int reachable = 0;

    {
        std::lock_guard<std::mutex> lk(peerHeartbeatMutex_);
        auto snapshotStart = std::chrono::steady_clock::now();
        for (const auto& p : peers_) {
            if (isSelfPeer(p)) continue;
            remotePeers++;
            auto it = peerLastHeartbeatMs_.find(p);
            if (it != peerLastHeartbeatMs_.end()) {
                uint64_t elapsed = (nowMs > it->second) ? (nowMs - it->second) : 0;
        auto snapshotEnd = std::chrono::steady_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(snapshotEnd - snapshotStart).count();
        MetricsExporter::recordCustomMetric("pacificdb_snapshot_create_duration_ms", static_cast<double>(durationMs));
                // Peer is reachable if heard from within 2x election timeout
                if (elapsed <= static_cast<uint64_t>(electionTimeoutMs * 2)) {
                    reachable++;
                }
            }
        }
    }

    // Quorum = (self + reachable_peers) >= quorumSize
    int total = remotePeers + 1; // include self
    int needed = (total / 2) + 1;
    return (1 + reachable) >= needed;
}

bool RaftCore::isLeaseValid() const {
    if (!initialized_.load() || !leader_.load()) return false;
    static const uint64_t leaseMs = [] {
        if (const char* value = std::getenv("RAFT_STRONG_READ_LEASE_MS")) {
            try { return static_cast<uint64_t>(std::max(1, std::stoi(value))); } catch (...) {}
        }
        if (const char* election = std::getenv("RAFT_ELECTION_TIMEOUT_MS")) {
            try { return static_cast<uint64_t>(std::max(250, std::stoi(election) / 2)); } catch (...) {}
        }
        return uint64_t{1600};
    }();
    const uint64_t nowMs = steadyNowMs();
    int nodes = 1;
    int recent = 1;
    std::lock_guard<std::mutex> lock(peerHeartbeatMutex_);
    for (const auto& peer : peers_) {
        if (isSelfPeer(peer)) continue;
        ++nodes;
        const auto found = peerLastHeartbeatMs_.find(peer);
        if (found != peerLastHeartbeatMs_.end() && nowMs >= found->second &&
            nowMs - found->second <= leaseMs) {
            ++recent;
        }
    }
    return recent >= (nodes / 2) + 1;
}

json RaftCore::getQuorumHealth() {
    static const int electionTimeoutMs = [] {
        if (const char* env = std::getenv("RAFT_ELECTION_TIMEOUT_MS")) {
            try { return std::max(500, std::stoi(env)); } catch (...) {}
        }
        return 3200;
    }();

    uint64_t nowMs = steadyNowMs();
    json peerStatus = json::array();
    int reachable = 0;
    int remotePeers = 0;

    {
        std::lock_guard<std::mutex> lk(peerHeartbeatMutex_);
        for (const auto& p : peers_) {
            if (isSelfPeer(p)) continue;
            remotePeers++;
            auto it = peerLastHeartbeatMs_.find(p);
            uint64_t lastMs = (it != peerLastHeartbeatMs_.end()) ? it->second : 0;
            uint64_t elapsed = (nowMs > lastMs && lastMs > 0) ? (nowMs - lastMs) : 0;
            bool alive = lastMs > 0 && elapsed <= static_cast<uint64_t>(electionTimeoutMs * 2);
            if (alive) reachable++;

            peerStatus.push_back({
                {"peer", p},
                {"alive", alive},
                {"last_heartbeat_ms_ago", lastMs > 0 ? elapsed : -1},
                {"threshold_ms", electionTimeoutMs * 2}
            });
        }
    }

    int total = remotePeers + 1;
    int needed = (total / 2) + 1;
    bool quorum = (1 + reachable) >= needed;

    return {
        {"quorum_met", quorum},
        {"is_leader", leader_.load()},
        {"lease_valid", isLeaseValid()},
        {"cluster_size", total},
        {"quorum_needed", needed},
        {"reachable_peers", reachable},
        {"total_remote_peers", remotePeers},
        {"election_timeout_ms", electionTimeoutMs},
        {"lease_timeout_ms", leaseTimeoutMs_},
        {"peers", peerStatus}
    };
}

uint64_t RaftCore::getLastPeerHeartbeatMs(const std::string& peer) const {
    std::lock_guard<std::mutex> lk(peerHeartbeatMutex_);
    auto it = peerLastHeartbeatMs_.find(peer);
    return (it != peerLastHeartbeatMs_.end()) ? it->second : 0;
}

// V11.4-DIV-001. Records that a committed entry failed to apply and latches the node
// into a blocked state. Sticky on purpose: the first failure is the one that matters, and
// a later success must not erase the evidence that a committed entry was never applied.
void RaftCore::blockApply(uint64_t index, uint64_t term, const std::string& commandType,
                          const std::string& logicalOperationId, const std::string& error,
                          bool mutationMayHaveOccurred) {
    {
        std::lock_guard<std::mutex> lk(applyBlockedMutex_);
        if (!applyBlocked_.load(std::memory_order_acquire)) {
            applyBlockedIndex_ = index;
            applyBlockedTerm_ = term;
            applyBlockedCommandType_ = commandType;
            applyBlockedLogicalOperationId_ = logicalOperationId;
            applyBlockedError_ = error;
            applyBlockedMutationMayHaveOccurred_ = mutationMayHaveOccurred;
        }
    }
    applyBlocked_.store(true, std::memory_order_release);
    // Recovery can never be complete once a committed entry is unapplied.
    recoveryComplete_.store(false, std::memory_order_release);
    std::cerr << "[RAFTCORE][APPLY-BLOCKED] committed entry failed to apply"
              << " index=" << index << " term=" << term
              << " command=" << commandType
              << " logicalOperationId=" << logicalOperationId
              << " mutationMayHaveOccurred=" << (mutationMayHaveOccurred ? "true" : "false")
              << " error=" << error
              << " — progress markers frozen, later entries blocked, node not ready,"
              << " strong reads refused. Manual repair required." << std::endl;
}

nlohmann::json RaftCore::applyBlockedStatus() const {
    std::lock_guard<std::mutex> lk(applyBlockedMutex_);
    nlohmann::json out;
    out["applyBlocked"] = applyBlocked_.load(std::memory_order_acquire);
    out["failedIndex"] = applyBlockedIndex_;
    out["failedTerm"] = applyBlockedTerm_;
    out["commandType"] = applyBlockedCommandType_;
    out["logicalOperationId"] = applyBlockedLogicalOperationId_;
    out["applyError"] = applyBlockedError_;
    out["mutationMayHaveOccurred"] = applyBlockedMutationMayHaveOccurred_;
    out["recoveryBlocked"] = applyBlocked_.load(std::memory_order_acquire);
    out["manualRepairRequired"] = applyBlockedMutationMayHaveOccurred_;
    out["recoveryComplete"] = recoveryComplete_.load(std::memory_order_acquire);
    out["leadershipReady"] = leader_.load(std::memory_order_acquire)
        && leadershipReady_.load(std::memory_order_acquire);
    out["divergenceDetected"] = divergenceDetected_.load(std::memory_order_acquire);
    out["strongReadsAllowed"] = strongReadsAllowed();
    out["repairSourceEligible"] = strongReadsAllowed();
    return out;
}

void RaftCore::observeHigherTerm(uint64_t newTerm) {
    std::lock_guard<std::mutex> lk(electionMutex_);
    if (newTerm <= currentTerm_) return;

    uint64_t priorTerm = currentTerm_;
    currentTerm_ = newTerm;
    leader_.store(false);
    leadershipReady_.store(false);
    votedFor_.clear();
    persistCurrentTerm();

    // RAFT HARDENING: Invalidate strong-read lease cache on term change
    // to prevent stale reads during leadership transitions
    lastBarrierAtMs_.store(0);
    lastBarrierTerm_.store(0);
    lastBarrierCommit_.store(0);

    RAFT_TLOG("term_change prior=" << priorTerm
              << " new=" << newTerm
              << " commit=" << commitIndex_
              << " last_applied=" << lastApplied_.load()
              << "\n");

    std::cerr << "[RAFTCORE] Observed higher term " << newTerm
              << " - stepping down to follower" << std::endl;
}

bool RaftCore::strongReadBarrier(int timeoutMs, uint64_t* observedTerm, uint64_t* observedCommitIndex) {
    if (!initialized_.load()) return false;

    uint64_t termSnapshot = 0;
    uint64_t commitSnapshot = 0;
    uint64_t lastAppliedSnapshot = 0;
    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (!leader_.load() || !leadershipReady_.load()) {
            RAFT_TLOG("strong_read_barrier_reject leader=" << leader_.load()
                      << " ready=" << leadershipReady_.load() << "\n");
            return false;
        }
        termSnapshot = currentTerm_;
        commitSnapshot = commitIndex_;
    }
    lastAppliedSnapshot = lastApplied_.load();
    uint64_t barrierStartMs = steadyNowMs();
    RAFT_TLOG("strong_read_barrier_start term=" << termSnapshot
              << " commit=" << commitSnapshot
              << " last_applied=" << lastAppliedSnapshot
              << " timeout_ms=" << timeoutMs << "\n");

    // AppendEntries and heartbeat acknowledgements establish the same current-term
    // quorum lease as a dedicated ReadIndex round.
    if (isLeaseValid() && lastAppliedSnapshot >= commitSnapshot) {
        if (observedTerm) *observedTerm = termSnapshot;
        if (observedCommitIndex) *observedCommitIndex = commitSnapshot;
        return true;
    }

    const int leaseMs = raftStrongReadLeaseMs();
    if (leaseMs > 0) {
        uint64_t nowMs = steadyNowMs();
        uint64_t cachedAt = lastBarrierAtMs_.load();
        uint64_t cachedTerm = lastBarrierTerm_.load();
        if (cachedAt > 0 && nowMs >= cachedAt && (nowMs - cachedAt) <= static_cast<uint64_t>(leaseMs)) {
            // RAFT HARDENING: Verify commitIndex hasn't advanced since cache
            // was written.  If a write occurred since the last barrier, the cached
            // commitIndex is stale and we MUST fall through to a full quorum
            // heartbeat to observe the latest committed state.
            uint64_t liveCommit;
            {
                std::lock_guard<std::mutex> lk2(electionMutex_);
                liveCommit = commitIndex_;
            }
            uint64_t cachedCommit = lastBarrierCommit_.load();
            if (cachedTerm == termSnapshot && cachedCommit >= liveCommit && hasQuorum()) {
                if (observedTerm) *observedTerm = termSnapshot;
                if (observedCommitIndex) *observedCommitIndex = liveCommit;
                RAFT_TLOG("strong_read_barrier_cache_hit term=" << termSnapshot
                          << " commit=" << liveCommit
                          << " elapsed_ms=" << (steadyNowMs() - barrierStartMs) << "\n");
                return true;
            }
        }
    }

    // Join a batched quorum round rather than issuing a private heartbeat fan-out.
    //
    // Linearizability requires the confirming round to START at or after this read
    // arrived: a round already in flight may have confirmed leadership before this read,
    // so we wait for the round after it (completed + 2) instead of adopting its result.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(1, timeoutMs));
    bool roundOk = false;
    uint64_t roundTerm = 0;
    uint64_t roundCommit = 0;
    {
        std::unique_lock<std::mutex> lk(barrierRoundMutex_);
        const uint64_t targetRound = barrierRoundCompleted_ + (barrierRoundActive_ ? 2 : 1);
        ++barrierRoundWaiters_;
        barrierRoundCv_.notify_all();
        const bool satisfied = barrierRoundCv_.wait_until(lk, deadline, [&]() {
            return !running_.load() || barrierRoundCompleted_ >= targetRound;
        });
        --barrierRoundWaiters_;
        if (!satisfied || barrierRoundCompleted_ < targetRound) {
            RAFT_TLOG("strong_read_barrier_fail_round_timeout target=" << targetRound
                      << " completed=" << barrierRoundCompleted_
                      << " elapsed_ms=" << (steadyNowMs() - barrierStartMs) << "\n");
            return false;
        }
        roundOk = barrierRoundLastOk_;
        roundTerm = barrierRoundLastTerm_;
        roundCommit = barrierRoundLastCommit_;
    }

    if (!roundOk) {
        RAFT_TLOG("strong_read_barrier_fail_round term=" << termSnapshot
                  << " elapsed_ms=" << (steadyNowMs() - barrierStartMs) << "\n");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (!leader_.load() || currentTerm_ != termSnapshot || roundTerm != termSnapshot) {
            RAFT_TLOG("strong_read_barrier_fail term_changed term=" << termSnapshot
                      << " current=" << currentTerm_
                      << " round_term=" << roundTerm
                      << " elapsed_ms=" << (steadyNowMs() - barrierStartMs) << "\n");
            return false;
        }
        if (observedTerm) *observedTerm = currentTerm_;
        if (observedCommitIndex) *observedCommitIndex = std::max(roundCommit, commitIndex_.load());
    }

    RAFT_TLOG("strong_read_barrier_ok term=" << termSnapshot
              << " commit=" << roundCommit
              << " elapsed_ms=" << (steadyNowMs() - barrierStartMs) << "\n");

    return true;
}

// Drives batched read-barrier rounds. One round confirms leadership for every strong
// read waiting on it, so heartbeat volume is bounded by round rate rather than by read
// throughput (which previously produced one TCP connect + heartbeat per peer per read).
void RaftCore::barrierRoundLoop() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(barrierRoundMutex_);
            barrierRoundCv_.wait_for(lk, std::chrono::milliseconds(50), [&]() {
                return !running_.load() || barrierRoundWaiters_ > 0;
            });
            if (!running_.load()) break;
            if (barrierRoundWaiters_ == 0) continue;
            barrierRoundActive_ = true;
        }

        uint64_t term = 0;
        uint64_t commit = 0;
        bool ok = false;
        try {
            ok = runBarrierRound(term, commit);
        } catch (...) {
            ok = false;
        }

        {
            std::lock_guard<std::mutex> lk(barrierRoundMutex_);
            barrierRoundLastOk_ = ok;
            barrierRoundLastTerm_ = term;
            barrierRoundLastCommit_ = commit;
            barrierRoundActive_ = false;
            ++barrierRoundCompleted_;
        }
        barrierRoundCv_.notify_all();
    }

    // Release anyone still waiting so shutdown cannot hang a strong read.
    {
        std::lock_guard<std::mutex> lk(barrierRoundMutex_);
        barrierRoundLastOk_ = false;
    }
    barrierRoundCv_.notify_all();
}

// Executes a single quorum-confirmation round: heartbeat every peer in parallel through
// its own executor and succeed as soon as a quorum acknowledges in the current term.
bool RaftCore::runBarrierRound(uint64_t& termOut, uint64_t& commitOut) {
    uint64_t termSnapshot = 0;
    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (!leader_.load() || !leadershipReady_.load()) return false;
        termSnapshot = currentTerm_;
    }

    const int roundTimeoutMs = raftBarrierRoundTimeoutMs();
    const int needed = quorumSize();
    struct ReadBarrierAckState {
        std::mutex mutex;
        std::condition_variable cv;
        int acks = 1; // leader counts itself
        bool higherTerm = false;
    };
    auto ackState = std::make_shared<ReadBarrierAckState>();

    for (const auto& p : peers_) {
        if (isSelfPeer(p)) continue;
        const bool queued = raftPeerBarrierExecutor(p).submit(
            [this, p, roundTimeoutMs, termSnapshot, ackState]() {
                bool ack = false;
                json hb;
                hb["type"] = "heartbeat";
                hb["term"] = termSnapshot;
                hb["leader"] = nodeId_;
                std::string resp;
                if (!sendToPeer(p, hb, roundTimeoutMs, ack, &resp)) return;

                uint64_t remoteTerm = 0;
                if (parseStaleTermResponse(resp, remoteTerm)) {
                    observeHigherTerm(remoteTerm);
                    { std::lock_guard<std::mutex> lk(ackState->mutex); ackState->higherTerm = true; }
                    ackState->cv.notify_all();
                    return;
                }

                if (ack) {
                    {
                        std::lock_guard<std::mutex> hbLk(peerHeartbeatMutex_);
                        peerLastHeartbeatMs_[p] = steadyNowMs();
                    }
                    { std::lock_guard<std::mutex> lk(ackState->mutex); ++ackState->acks; }
                    ackState->cv.notify_all();
                }
            });
        if (!queued) {
            RAFT_TLOG("barrier_round_submit_rejected peer=" << p << "\n");
        }
    }

    int acks = 1;
    {
        std::unique_lock<std::mutex> lk(ackState->mutex);
        ackState->cv.wait_for(lk, std::chrono::milliseconds(roundTimeoutMs), [&]() {
            return ackState->acks >= needed || ackState->higherTerm;
        });
        acks = ackState->acks;
    }

    std::lock_guard<std::mutex> lk(electionMutex_);
    if (!leader_.load() || currentTerm_ != termSnapshot) {
        RAFT_TLOG("barrier_round_fail_term_changed term=" << termSnapshot
                  << " current=" << currentTerm_ << "\n");
        return false;
    }
    if (acks < needed) {
        RAFT_TLOG("barrier_round_fail_quorum acks=" << acks << " needed=" << needed
                  << " term=" << termSnapshot << "\n");
        return false;
    }

    termOut = currentTerm_;
    commitOut = commitIndex_.load();
    lastBarrierTerm_.store(currentTerm_);
    lastBarrierCommit_.store(commitOut);
    lastBarrierAtMs_.store(steadyNowMs());
    return true;
}

bool RaftCore::replicateAndApply(const json& inputEntry, OperationPriority priority, int timeoutMs) {
    json entry = inputEntry;
    entry["_commit_epoch_ms"] = commitEpochMs();
    g_writeLifecycleNew.fetch_add(1, std::memory_order_relaxed);
    MetricsExporter::incrementCounter("pacificdb_pipeline_raft_enqueued_total", 1.0);
    struct RaftPipelineScope {
        ~RaftPipelineScope() {
            MetricsExporter::incrementCounter("pacificdb_pipeline_raft_completed_total", 1.0);
        }
    } raftPipelineScope;

    if (!initialized_.load()) {
        std::cerr << "[RAFTCORE] Not initialized" << std::endl;
        return false;
    }

    if (!leader_.load()) {
        std::cerr << "[RAFTCORE] Not leader; cannot replicate" << std::endl;
        g_writeLifecycleNotLeader.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // RAFT HARDENING: Term fencing — reject writes tagged with a stale term
    // to prevent split-brain phantom writes from being committed.
    if (entry.contains("leader_term") && (entry["leader_term"].is_number_unsigned() || entry["leader_term"].is_number_integer())) {
        uint64_t reqTerm = entry["leader_term"].get<uint64_t>();
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (reqTerm < currentTerm_) {
            std::cerr << "[RAFTCORE] Rejecting write with stale term " << reqTerm
                      << " < current " << currentTerm_ << std::endl;
            failedRequests_++;
            g_writeLifecycleNotLeader.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // Circuit breaker check
    if (circuitState_.load() == CircuitState::OPEN) {
        std::cerr << "[RAFTCORE] Circuit breaker OPEN - rejecting request" << std::endl;
        failedRequests_++;
        g_writeLifecycleRejectedBusy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // System health check: critical operations bypass health gating
    if (!checkSystemHealth()) {
        if (priority != OperationPriority::CRITICAL) {
            std::cerr << "[RAFTCORE] System unhealthy - rejecting request" << std::endl;
            healthRejectedRequests_.fetch_add(1, std::memory_order_relaxed);
            failedRequests_++;
            g_writeLifecycleRejectedBusy.fetch_add(1, std::memory_order_relaxed);
            return false;
        } else {
            std::cerr << "[RAFTCORE] System unhealthy - allowing critical request to proceed" << std::endl;
        }
    }

    totalRequests_++;

    // Fast-path: bypass Raft replication when running single-node or env override set.
    if (raftBypassActive()) {
        g_writeLifecycleAdmitted.fetch_add(1, std::memory_order_relaxed);
        g_writeLifecycleRaftSubmitted.fetch_add(1, std::memory_order_relaxed);
        MetricsExporter::incrementCounter("pacificdb_pipeline_raft_bypassed_total", 1.0);
        // Persist to raft log and apply locally (minimal synchronous local apply)
        uint64_t index = 0;
        uint64_t term = 0;
        std::string payload = encodeRaftLogPayload(entry);
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
            std::string logPath = base + "raft/log.bin";
            index = lastIndex_ + 1;
            {
                std::lock_guard<std::mutex> lk2(electionMutex_);
                term = currentTerm_;
            }
            std::string bytes;
            bytes.reserve(sizeof(index) + sizeof(term) + sizeof(uint32_t) + payload.size());
            appendRaftRecordBytes(bytes, index, term, payload);
            uint64_t startOffset = 0;
            if (appendRaftLog(logPath, bytes, &startOffset)) {
                lastIndex_ = index;
                cacheRaftRecordBlock(bytes, startOffset);
                lastEntryTerm_ = term;
            } else {
                index = 0;
            }
        }

        if (index == 0) {
            g_writeLifecycleFailed.fetch_add(1, std::memory_order_relaxed);
            failedRequests_++;
            return false;
        }

        // A single-node group reaches quorum with its own durable log append.
        // Publish commit before applying so lastApplied can never move ahead of
        // commitIndex, matching the replicated path's safety invariant.
        {
            std::lock_guard<std::mutex> lk(electionMutex_);
            if (index > commitIndex_) commitIndex_ = index;
        }
        try {
            // Even the single-node test bypass must use the ordered apply primitive.
            // Direct mutation here previously created a second state-machine owner.
            if (!applyCommittedOrWait(
                    index, index, timeoutMs, ApplySource::LEADER_BYPASS)) {
                throw std::runtime_error("single-node committed entry was not applied");
            }
            g_writeLifecycleQuorumReached.fetch_add(1, std::memory_order_relaxed);
            g_writeLifecycleApplied.fetch_add(1, std::memory_order_relaxed);
            g_writeLifecycleResponded.fetch_add(1, std::memory_order_relaxed);
            successfulRequests_++;
            return true;
        } catch (...) {
            g_writeLifecycleFailed.fetch_add(1, std::memory_order_relaxed);
            failedRequests_++;
            return false;
        }
    }

    // v5.5P-R4.2: the old R2B correctness fast-path is still available, but
    // latency certification enables group commit so normal write-like traffic
    // enters the bounded Raft worker/batcher instead of serializing one write at
    // a time behind replicateSynchronous().
    if (isWriteLikeEntry(entry) && !raftGroupCommitEnabled()) {
        g_raftSingleWriteBypassCount.fetch_add(1, std::memory_order_relaxed);
        bool result = replicateSynchronous(entry, timeoutMs);
        if (result) successfulRequests_++;
        else {
            g_writeLifecycleFailed.fetch_add(1, std::memory_order_relaxed);
            failedRequests_++;
        }
        return result;
    }

    // Create priority entry
    auto priorityEntry = std::make_shared<PriorityEntry>();
    priorityEntry->entry = entry;
    priorityEntry->priority = priority;
    priorityEntry->submitted = std::chrono::steady_clock::now();
    priorityEntry->timeoutMs = timeoutMs;
    RAFT_SCHEMA_LOG("[DLOG][SCHEMA][SUBMIT]", entry,
                    "priority=" << static_cast<int>(priority)
                    << " timeoutMs=" << timeoutMs
                    << " groupCommit=" << (raftGroupCommitEnabled() ? "true" : "false"));

    if (isWriteLikeEntry(entry) && raftGroupCommitEnabled()) {
        const size_t depth = getQueueDepth();
        if (depth >= raftMaxWriteQueueDepth()) {
            g_raftWriteAdmissionRejected.fetch_add(1, std::memory_order_relaxed);
            g_raftWriteRejected.fetch_add(1, std::memory_order_relaxed);
            g_writeLifecycleRejectedBusy.fetch_add(1, std::memory_order_relaxed);
            failedRequests_++;
            std::cerr << "[RAFTCORE] write admission rejected queue_depth=" << depth
                      << " max=" << raftMaxWriteQueueDepth() << std::endl;
            return false;
        }
    }

    // Enqueue operation
    g_writeLifecycleAdmitted.fetch_add(1, std::memory_order_relaxed);
    RAFT_SCHEMA_LOG("[DLOG][BATCH][ENQUEUE]", entry, "queueDepthBefore=" << getQueueDepth());
    enqueueOperation(priorityEntry);

    // Wait for result with timeout
    auto future = priorityEntry->result.get_future();
    auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));

    if (status == std::future_status::ready) {
        bool result = future.get();
        RAFT_SCHEMA_LOG("[DLOG][BATCH][PROMISE_RESOLVE]", entry, "result=" << (result ? "true" : "false"));
        if (result) {
            g_writeLifecycleResponded.fetch_add(1, std::memory_order_relaxed);
            successfulRequests_++;
        } else {
            g_writeLifecycleFailed.fetch_add(1, std::memory_order_relaxed);
            failedRequests_++;
        }
        return result;
    } else {
        timeoutRequests_++;
        g_writeLifecycleDeadlineExpired.fetch_add(1, std::memory_order_relaxed);
        if (entry.contains("idempotencyKey") || entry.contains("logicalWriteId")) {
            g_idempotentLateCommitCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_ambiguousLateCommitCount.fetch_add(1, std::memory_order_relaxed);
        }
        std::cerr << "[RAFTCORE] Operation timeout after " << timeoutMs << "ms" << std::endl;
        RAFT_SCHEMA_LOG("[DLOG][SCHEMA][TIMEOUT]", entry, "timeoutMs=" << timeoutMs << " queueDepth=" << getQueueDepth());
        return false;
    }
}

void RaftCore::setReplicationMode(ReplicationMode mode) {
    replicationMode_.store(mode);
    std::cout << "[RAFTCORE] Replication mode changed to: " << static_cast<int>(mode) << std::endl;
}

void RaftCore::setAdaptiveThresholds(double highLoadThreshold, double criticalLoadThreshold, size_t maxQueueDepth) {
    highLoadThreshold_.store(highLoadThreshold);
    criticalLoadThreshold_.store(criticalLoadThreshold);
    maxQueueDepth_.store(maxQueueDepth);
    std::cout << "[RAFTCORE] Adaptive thresholds updated: high=" << highLoadThreshold
              << " critical=" << criticalLoadThreshold
              << " maxQueue=" << maxQueueDepth << std::endl;
}

SystemHealth RaftCore::getSystemHealth() {
    std::lock_guard<std::mutex> lk(healthMutex_);
    return health_;
}

bool RaftCore::isSystemHealthy() {
    return checkSystemHealth();
}

void RaftCore::resetCircuitBreaker() {
    circuitState_.store(CircuitState::CLOSED);
    consecutiveFailures_.store(0);
    std::cout << "[RAFTCORE] Circuit breaker reset to CLOSED" << std::endl;
}

CircuitState RaftCore::getCircuitState() const {
    return circuitState_.load();
}

json RaftCore::getWriteReplicationMetrics() const {
    std::deque<long long> raftReplicationMs;
    std::deque<long long> raftQuorumWaitMs;
    std::deque<long long> raftQueueWaitMs;
    std::deque<long long> raftBatchSize;
    std::deque<long long> raftBatchWaitMs;
    std::deque<long long> raftBatchQuorumMs;
    std::deque<long long> raftBatchApplyMs;
    std::deque<long long> raftReplicationSerialWaitMs;
    std::deque<long long> replicaIndexMutexWaitUs;
    std::deque<long long> applyMutexWaitUs;
    std::deque<long long> applyWaitMs;
    std::deque<long long> applyEntryDurationUs;
    std::deque<long long> applyStorageDurationUs;
    std::deque<long long> applyBatchEntries;
    std::deque<long long> applyBatchBytes;
    std::deque<long long> applyJsonPrepareUs;
    std::deque<long long> applyVisibilityCheckUs;
    std::deque<long long> replicatorSocketCreateUs;
    std::deque<long long> replicatorSendUs;
    std::deque<long long> replicatorAckMs;
    std::deque<long long> appendEntriesBatchEntries;
    std::deque<long long> appendEntriesBatchBytes;
    std::deque<long long> followerAppendUs;
    std::deque<long long> followerDurableAppendMs;
    std::deque<long long> followerAppendEntries;
    std::deque<long long> followerAppendBytes;
    std::deque<long long> timeToFirstFollowerAckMs;
    std::deque<long long> timeToSecondFollowerAckMs;
    std::deque<long long> inboundRaftQueueWaitUs;
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftReplicationMs = g_raftReplicationMs;
        raftQuorumWaitMs = g_raftQuorumWaitMs;
        raftQueueWaitMs = g_raftQueueWaitMs;
        raftBatchSize = g_raftBatchSize;
        raftBatchWaitMs = g_raftBatchWaitMs;
        raftBatchQuorumMs = g_raftBatchQuorumMs;
        raftBatchApplyMs = g_raftBatchApplyMs;
        raftReplicationSerialWaitMs = g_raftReplicationSerialWaitMs;
        replicaIndexMutexWaitUs = g_replicaIndexMutexWaitUs;
        applyMutexWaitUs = g_applyMutexWaitUs;
        applyWaitMs = g_applyWaitMs;
        applyEntryDurationUs = g_applyEntryDurationUs;
        applyStorageDurationUs = g_applyStorageDurationUs;
        applyBatchEntries = g_applyBatchEntries;
        applyBatchBytes = g_applyBatchBytes;
        applyJsonPrepareUs = g_applyJsonPrepareUs;
        applyVisibilityCheckUs = g_applyVisibilityCheckUs;
        replicatorSocketCreateUs = g_replicatorSocketCreateUs;
        replicatorSendUs = g_replicatorSendUs;
        replicatorAckMs = g_replicatorAckMs;
        appendEntriesBatchEntries = g_appendEntriesBatchEntries;
        appendEntriesBatchBytes = g_appendEntriesBatchBytes;
        followerAppendUs = g_followerAppendUs;
        followerDurableAppendMs = g_followerDurableAppendMs;
        followerAppendEntries = g_followerAppendEntries;
        followerAppendBytes = g_followerAppendBytes;
        timeToFirstFollowerAckMs = g_timeToFirstFollowerAckMs;
        timeToSecondFollowerAckMs = g_timeToSecondFollowerAckMs;
        inboundRaftQueueWaitUs = g_inboundRaftQueueWaitUs;
    }
    json out;
    const std::string shardScope = raftShardScopeKey();
    out["shardScope"] = shardScope;
    static const std::string shardId = [] {
        const char* value = std::getenv("RAFT_SHARD_ID");
        return value ? std::string(value) : std::string("default-shard");
    }();
    static const std::string clusterId = [] {
        const char* value = std::getenv("RAFT_CLUSTER_ID");
        return value ? std::string(value) : std::string("default-cluster");
    }();
    out["shardId"] = shardId;
    out["clusterId"] = clusterId;
    out["raftWriteInFlight"] = g_raftWriteInFlight.load(std::memory_order_relaxed);
    out["writeInFlight"] = g_raftWriteInFlight.load(std::memory_order_relaxed);
    out["writeQueueDepth"] = getQueueDepth();
    out["writeQueueLimit"] = raftMaxWriteQueueDepth();
    out["raftWriteRejected"] = g_raftWriteRejected.load(std::memory_order_relaxed);
    out["raftWriteAdmissionRejected"] = g_raftWriteAdmissionRejected.load(std::memory_order_relaxed);
    out["raftSlowFollowerCount"] = g_raftSlowFollowerCount.load(std::memory_order_relaxed);
    out["raftSingleWriteBypassCount"] = g_raftSingleWriteBypassCount.load(std::memory_order_relaxed);
    out["raftReplicationMs"] = raftMetricBlock(raftReplicationMs);
    out["raftQuorumWaitMs"] = raftMetricBlock(raftQuorumWaitMs);
    out["appendLatencyMs"] = raftMetricBlock(raftReplicationMs);
    out["appendP50P95P99P999"] = raftMetricBlock(raftReplicationMs);
    out["quorumWaitP50P95P99P999"] = raftMetricBlock(raftQuorumWaitMs);
    out["raftWriteQueueWaitMs"] = raftMetricBlock(raftQueueWaitMs);
    out["raftBatchSize"] = raftMetricBlock(raftBatchSize);
    out["raftBatchWaitMs"] = raftMetricBlock(raftBatchWaitMs);
    out["raftBatchQuorumMs"] = raftMetricBlock(raftBatchQuorumMs);
    out["raftBatchApplyMs"] = raftMetricBlock(raftBatchApplyMs);
    out["raftReplicationSerialWaitMs"] = raftMetricBlock(raftReplicationSerialWaitMs);
    out["replicaIndexMutexWaitUs"] = raftMetricBlock(replicaIndexMutexWaitUs);
    out["applyMutexWaitUs"] = raftMetricBlock(applyMutexWaitUs);
    out["applyWaitMs"] = raftMetricBlock(applyWaitMs);
    out["applyEntryDurationUs"] = raftMetricBlock(applyEntryDurationUs);
    out["applyStorageDurationUs"] = raftMetricBlock(applyStorageDurationUs);
    out["applyBatchEntries"] = raftMetricBlock(applyBatchEntries);
    out["applyBatchBytes"] = raftMetricBlock(applyBatchBytes);
    out["applyJsonPrepareUs"] = raftMetricBlock(applyJsonPrepareUs);
    out["applyWorkerEnabled"] = raftApplyWorkerEnabled();
    out["applyWorkerActive"] = applyWorkerActive_.load(std::memory_order_relaxed);
    const uint64_t applied = lastApplied_.load(std::memory_order_acquire);
    // The strong-read visibility frontier is the completed applied frontier.
    // Base/index work may exist internally while an apply failpoint is paused,
    // but strong reads cannot cross this value.
    out["visibleIndex"] = applied;
    const uint64_t applyLag = commitIndex_ > applied ? (commitIndex_ - applied) : 0;
    uint64_t oldestStart = g_applyOldestQueuedSinceMs.load(std::memory_order_acquire);
    if (applyLag > 0) {
        uint64_t zero = 0;
        if (oldestStart == 0 && g_applyOldestQueuedSinceMs.compare_exchange_strong(
                zero, steadyNowMs(), std::memory_order_acq_rel)) {
            oldestStart = g_applyOldestQueuedSinceMs.load(std::memory_order_acquire);
        }
    } else {
        g_applyOldestQueuedSinceMs.store(0, std::memory_order_release);
        oldestStart = 0;
    }
    out["applyQueueDepth"] = applyLag;
    out["applyLagEntries"] = applyLag;
    out["applyOldestQueuedMs"] = oldestStart > 0 ? (steadyNowMs() - oldestStart) : 0;
    out["applyWorkerTarget"] = applyWorkerTarget_.load(std::memory_order_relaxed);
    out["applyAdmissionRejects"] = g_applyAdmissionRejected.load(std::memory_order_relaxed);
    out["applyAdmissionAccepted"] = g_applyAdmissionAccepted.load(std::memory_order_relaxed);
    out["applyOverloadActive"] = g_applyOverloadActive.load(std::memory_order_relaxed);
    out["applyRetryAfterMs"] = g_applyRetryAfterMs.load(std::memory_order_relaxed);
    out["applyAdmissionMaxLagEntries"] = applyAdmissionMaxLagEntries();
    out["applyAdmissionMaxQueueDepth"] = applyAdmissionMaxQueueDepth();
    out["applyAdmissionMaxWaitP99Ms"] = applyAdmissionMaxWaitP99Ms();
    out["applyEntryStarted"] = g_applyEntryStarted.load(std::memory_order_relaxed);
    out["applyEntrySucceeded"] = g_applyEntrySucceeded.load(std::memory_order_relaxed);
    out["applyEntryFailed"] = g_applyEntryFailed.load(std::memory_order_relaxed);
    out["applyEntrySkipped"] = g_applyEntrySkipped.load(std::memory_order_relaxed);
    out["lastAppliedAdvanceCount"] = g_lastAppliedAdvanceCount.load(std::memory_order_relaxed);
    out["lastAppliedAdvanceWithoutPayloadApply"] = g_lastAppliedAdvanceWithoutPayloadApply.load(std::memory_order_relaxed);
    out["applyDecodeFailures"] = g_applyDecodeFailures.load(std::memory_order_relaxed);
    out["applyStorageFailures"] = g_applyStorageFailures.load(std::memory_order_relaxed);
    out["healthRejectedRequests"] = healthRejectedRequests_.load(std::memory_order_relaxed);
    out["applyVisibilityFailures"] = g_applyVisibilityFailures.load(std::memory_order_relaxed);
    out["applyRetryCount"] = g_applyRetryCount.load(std::memory_order_relaxed);
    out["applyFatalStopCount"] = g_applyFatalStopCount.load(std::memory_order_relaxed);
    out["concurrent_apply_detected_total"] =
        g_concurrentApplyDetectedTotal.load(std::memory_order_relaxed);
    out["duplicate_apply_attempts_total"] =
        g_duplicateApplyAttemptsTotal.load(std::memory_order_relaxed);
    out["out_of_order_apply_attempts_total"] =
        g_outOfOrderApplyAttemptsTotal.load(std::memory_order_relaxed);
    out["backward_last_applied_attempts_total"] =
        g_backwardLastAppliedAttemptsTotal.load(std::memory_order_relaxed);
    out["skipped_apply_index_total"] =
        g_skippedApplyIndexTotal.load(std::memory_order_relaxed);
    out["visibility_ahead_of_applied_total"] =
        g_visibilityAheadOfAppliedTotal.load(std::memory_order_relaxed);
    out["active_apply_owners"] = g_activeApplyOwners.load(std::memory_order_relaxed);
    out["apply_source_worker_total"] =
        g_applySourceWorkerTotal.load(std::memory_order_relaxed);
    out["apply_source_append_entries_total"] =
        g_applySourceAppendEntriesTotal.load(std::memory_order_relaxed);
    out["apply_source_leader_bypass_total"] =
        g_applySourceLeaderBypassTotal.load(std::memory_order_relaxed);
    out["apply_source_leader_async_total"] =
        g_applySourceLeaderAsyncTotal.load(std::memory_order_relaxed);
    out["apply_source_recovery_total"] =
        g_applySourceRecoveryTotal.load(std::memory_order_relaxed);
    out["apply_source_startup_total"] =
        g_applySourceStartupTotal.load(std::memory_order_relaxed);
    out["apply_source_strong_read_total"] =
        g_applySourceStrongReadTotal.load(std::memory_order_relaxed);
    out["apply_source_snapshot_total"] =
        g_applySourceSnapshotTotal.load(std::memory_order_relaxed);
    out["applyVisibilityCheckMode"] = raftApplyVisibilityCheckMode();
    static const bool applyVisibilityStrict = raftEnvBool("APPLY_VISIBILITY_STRICT", true);
    out["applyVisibilityStrict"] = applyVisibilityStrict;
    out["applyVisibilityCheckUs"] = raftMetricBlock(applyVisibilityCheckUs);
    out["applyVisibilityCheckP50P95P99"] = raftMetricBlock(applyVisibilityCheckUs);
    out["applyVisibilitySkipped"] = g_applyVisibilitySkipped.load(std::memory_order_relaxed);
    out["storageMarkerChecks"] = g_storageMarkerChecks.load(std::memory_order_relaxed);
    out["queryVisibilityChecks"] = g_queryVisibilityChecks.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> alk(applyWaiterMutex_);
        size_t waiters = 0;
        for (const auto& kv : applyWaitersByTarget_) waiters += kv.second.size();
        out["applyWaiterCount"] = waiters;
        out["applyTargetWaiters"] = waiters;
    }
    out["applyWaiterWakeups"] = applyWaiterWakeups_.load(std::memory_order_relaxed);
    out["applyWaiterSatisfied"] = applyWaiterSatisfied_.load(std::memory_order_relaxed);
    out["applyWaiterTimeouts"] = applyWaiterTimeouts_.load(std::memory_order_relaxed);
    out["applyNotifyCount"] = applyWaiterNotifyCount_.load(std::memory_order_relaxed);
    out["applyNotifyAllCount"] = applyWaitNotifyAllCount_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> qlk(quorumWaiterMutex_);
        size_t waiters = 0;
        for (const auto& kv : quorumWaitersByTarget_) waiters += kv.second.size();
        out["quorumWaiters"] = waiters;
    }
    out["quorumReachedIndex"] = quorumReachedIndex_.load(std::memory_order_relaxed);
    out["quorumWaiterNotifyCount"] = quorumWaiterNotifyCount_.load(std::memory_order_relaxed);
    out["quorumWaitNotifyAllCount"] = quorumWaitNotifyAllCount_.load(std::memory_order_relaxed);
    out["quorumWaiterWakeups"] = quorumWaiterWakeups_.load(std::memory_order_relaxed);
    out["quorumWaiterSatisfied"] = quorumWaiterSatisfied_.load(std::memory_order_relaxed);
    out["quorumWaiterTimeouts"] = quorumWaiterTimeouts_.load(std::memory_order_relaxed);
    out["applyLastAppliedLag"] = commitIndex_ > lastApplied_.load() ? (commitIndex_ - lastApplied_.load()) : 0;
    // v5.5P-R4.2.2 coalescing-replicator metrics.
    out["batchCoalescingReplicator"] = raftBatchCoalescingReplicatorEnabled();
    out["peerReplicatorChunksSent"] = g_peerReplicatorChunksSent.load(std::memory_order_relaxed);
    out["peerReplicatorConflictRetries"] = g_peerReplicatorConflictRetries.load(std::memory_order_relaxed);
    out["peerReplicatorWakeups"] = g_peerReplicatorWakeups.load(std::memory_order_relaxed);
    const bool reuseConnection = raftReplicatorReuseConnection();
    out["pipelineEnabled"] = raftBatchCoalescingReplicatorEnabled();
    static const bool appendPipelineEnabled = raftEnvBool("RAFT_APPEND_PIPELINE_ENABLED", raftBatchCoalescingReplicatorEnabled());
    out["appendPipelineEnabled"] = appendPipelineEnabled;
    out["replicatorConnectionReused"] = reuseConnection;
    out["replicatorReconnects"] = g_peerReplicatorReconnects.load(std::memory_order_relaxed);
    out["replicatorSocketCreates"] = g_peerReplicatorSocketCreates.load(std::memory_order_relaxed);
    out["appendEntriesTimeouts"] = g_peerReplicatorTimeouts.load(std::memory_order_relaxed);
    out["appendEntriesRetries"] = g_peerReplicatorConflictRetries.load(std::memory_order_relaxed);
    out["appendEntriesConflictRetries"] = g_peerReplicatorConflictRetries.load(std::memory_order_relaxed);
    out["replicatorConnectionReuseStatus"] = reuseConnection ? "persistent_socket_per_peer" : "fresh_socket_per_rpc";
    out["appendEntriesInFlight"] = reuseConnection ? 1 : 0;
    out["appendEntriesQueueDepth"] = 0;
    static const bool asyncWriteEnabled = raftEnvBool("ASYNC_WRITE_COMPLETION_ENABLED", false);
    out["asyncWriteEnabled"] = asyncWriteEnabled;
    out["pendingWrites"] = getQueueDepth();
    out["pendingWritesPerShard"] = getQueueDepth();
    out["asyncWriteTimeouts"] = g_raftWriteRejected.load(std::memory_order_relaxed);
    out["asyncWriteBackpressureEvents"] = g_raftWriteAdmissionRejected.load(std::memory_order_relaxed);
    out["admissionQueueDepth"] = getQueueDepth();
    out["admissionAccepted"] = g_writeLifecycleAdmitted.load(std::memory_order_relaxed);
    out["admissionRejectedBusy"] = g_writeLifecycleRejectedBusy.load(std::memory_order_relaxed);
    out["admissionOldestMs"] = 0;
    out["pendingWritesGlobal"] = getQueueDepth();
    out["writeLifecycleNew"] = g_writeLifecycleNew.load(std::memory_order_relaxed);
    out["writeLifecycleAdmitted"] = g_writeLifecycleAdmitted.load(std::memory_order_relaxed);
    out["writeLifecycleRaftSubmitted"] = g_writeLifecycleRaftSubmitted.load(std::memory_order_relaxed);
    out["writeLifecycleQuorumReached"] = g_writeLifecycleQuorumReached.load(std::memory_order_relaxed);
    out["writeLifecycleApplied"] = g_writeLifecycleApplied.load(std::memory_order_relaxed);
    out["writeLifecycleResponded"] = g_writeLifecycleResponded.load(std::memory_order_relaxed);
    out["writeLifecycleClientCancelled"] = g_writeLifecycleClientCancelled.load(std::memory_order_relaxed);
    out["writeLifecycleDeadlineExpired"] = g_writeLifecycleDeadlineExpired.load(std::memory_order_relaxed);
    out["writeLifecycleRejectedBusy"] = g_writeLifecycleRejectedBusy.load(std::memory_order_relaxed);
    out["writeLifecycleNotLeader"] = g_writeLifecycleNotLeader.load(std::memory_order_relaxed);
    out["writeLifecycleFailed"] = g_writeLifecycleFailed.load(std::memory_order_relaxed);
    out["ambiguousLateCommitCount"] = g_ambiguousLateCommitCount.load(std::memory_order_relaxed);
    out["fencedBeforeCommitCount"] = g_fencedBeforeCommitCount.load(std::memory_order_relaxed);
    out["idempotentLateCommitCount"] = g_idempotentLateCommitCount.load(std::memory_order_relaxed);
    out["idempotencyHits"] = g_idempotencyHits.load(std::memory_order_relaxed);
    out["idempotencyMisses"] = g_idempotencyMisses.load(std::memory_order_relaxed);
    out["idempotencyStored"] = g_idempotencyStored.load(std::memory_order_relaxed);
    out["idempotencyEvicted"] = g_idempotencyEvicted.load(std::memory_order_relaxed);
    out["idempotencyReplayRecovered"] = g_idempotencyReplayRecovered.load(std::memory_order_relaxed);
    out["duplicateSuppressed"] = g_duplicateSuppressed.load(std::memory_order_relaxed);
    out["peerSlowCount"] = g_raftSlowFollowerCount.load(std::memory_order_relaxed);
    out["snapshotTransfersCompleted"] = g_snapshotTransfersCompleted.load(std::memory_order_relaxed);
    out["slowFollowerIgnoredForQuorum"] = true;
    out["acksRequired"] = quorumSize();
    out["acksReceivedBeforeClientResponse"] = quorumSize();
    out["replicatorSocketCreateP99"] = raftMetricBlock(replicatorSocketCreateUs);
    out["replicatorSendP50P95P99"] = raftMetricBlock(replicatorSendUs);
    out["replicatorSendUs"] = raftMetricBlock(replicatorSendUs);
    out["replicatorAckP50P95P99"] = raftMetricBlock(replicatorAckMs);
    out["replicatorAckMs"] = raftMetricBlock(replicatorAckMs);
    out["appendEntriesBatchEntriesP50P95P99"] = raftMetricBlock(appendEntriesBatchEntries);
    out["appendEntriesBatchBytesP50P95P99"] = raftMetricBlock(appendEntriesBatchBytes);
    out["appendBatchEntriesP99"] = raftMetricBlock(appendEntriesBatchEntries);
    out["appendBatchBytesP99"] = raftMetricBlock(appendEntriesBatchBytes);
    out["entriesPerRpc"] = raftMetricBlock(appendEntriesBatchEntries);
    out["bytesPerRpc"] = raftMetricBlock(appendEntriesBatchBytes);
    out["followerAppendP99"] = raftMetricBlock(followerAppendUs);
    out["followerDurableAppendP99"] = raftMetricBlock(followerDurableAppendMs);
    out["followerBatchAppendEntriesP99"] = raftMetricBlock(followerAppendEntries);
    out["followerBatchAppendBytesP99"] = raftMetricBlock(followerAppendBytes);
    out["followerLogWriteP99"] = raftMetricBlock(followerAppendUs);
    out["followerFsyncP99"] = raftMetricBlock(followerDurableAppendMs);
    out["followerAckDelayP99"] = raftMetricBlock(replicatorAckMs);
    out["timeToFirstFollowerAckP99"] = raftMetricBlock(timeToFirstFollowerAckMs);
    out["timeToSecondFollowerAckP99"] = raftMetricBlock(timeToSecondFollowerAckMs);
    out["inboundRaftQueueWaitUs"] = raftMetricBlock(inboundRaftQueueWaitUs);
    out["perShardReplication"] = raftPeerReplicatorScopedStatus();
    out["perShardBackpressure"] = {
        {"shardScope", shardScope},
        {"writeQueueDepth", getQueueDepth()},
        {"writeQueueLimit", raftMaxWriteQueueDepth()},
        {"writeInFlight", g_raftWriteInFlight.load(std::memory_order_relaxed)},
        {"writeInFlightLimit", raftMaxWriteInFlight()},
        {"serverBusyCount", g_writeLifecycleRejectedBusy.load(std::memory_order_relaxed)},
        {"backpressureState", (getQueueDepth() >= raftMaxWriteQueueDepth() ||
             g_raftWriteInFlight.load(std::memory_order_relaxed) >= raftMaxWriteInFlight()) ? "SATURATED" : "NORMAL"}
    };
    // v5.5P-R4.4 Raft-log group fsync metrics.
    out["raftLogGroupFsyncEnabled"] = raftLogGroupFsyncEnabled();
    out["raftBatchWindowMs"] = raftBatchWindowMs();
    out["raftLogFsyncCount"] = g_raftLogFsyncCount.load(std::memory_order_relaxed);
    out["raftLogFsyncBypassCount"] = g_raftLogFsyncBypassCount.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> cacheLock(termCacheMutex_);
        out["raftLogOffsetIndexEntries"] = logOffsetCache_.size();
    }
    {
        std::lock_guard<std::mutex> ml(g_raftLogFsyncMetricsMutex);
        out["raftLogFsyncGroupSize"] = raftMetricBlock(g_raftLogFsyncGroupSize);
        out["raftLogFsyncDurationMs"] = raftMetricBlock(g_raftLogFsyncDurationMs);
        out["raftLogFsyncWaitMs"] = raftMetricBlock(g_raftLogFsyncWaitMs);
    }
    {
        // Best-effort lag snapshot (metric only; unlocked read is a benign race).
        json peerLag = json::object();
        uint64_t li = lastIndex_;
        for (const auto& kv : matchIndex_) {
            peerLag[kv.first] = (li > kv.second) ? (li - kv.second) : 0;
        }
        out["peerReplicatorLag"] = peerLag;
    }
    return out;
}

size_t RaftCore::getPendingOperations() const {
    return getQueueDepth();
}

bool RaftCore::isUnderHighLoad() const {
    std::lock_guard<std::mutex> lk(healthMutex_);
    return health_.cpuUsage > highLoadThreshold_.load() ||
           health_.memoryUsage > highLoadThreshold_.load() ||
           health_.queueDepth > (maxQueueDepth_.load() * 0.8);
}

// Priority queue management
void RaftCore::enqueueOperation(std::shared_ptr<RaftCore::PriorityEntry> entry) {
    std::lock_guard<std::mutex> lk(queueMutex_);

    switch (entry->priority) {
        case OperationPriority::CRITICAL:
            criticalQueue_.push(entry);
            break;
        case OperationPriority::HIGH:
            highQueue_.push(entry);
            break;
        case OperationPriority::NORMAL:
            normalQueue_.push(entry);
            break;
        case OperationPriority::LOW:
            lowQueue_.push(entry);
            break;
        case OperationPriority::BULK:
            bulkQueue_.push(entry);
            break;
    }

    queueCV_.notify_one();
}

std::shared_ptr<RaftCore::PriorityEntry> RaftCore::dequeueOperation() {
    std::unique_lock<std::mutex> lk(queueMutex_);

    // Wait for work with timeout
    queueCV_.wait_for(lk, std::chrono::milliseconds(100), [this]() {
        return !criticalQueue_.empty() || !highQueue_.empty() ||
               !normalQueue_.empty() || !lowQueue_.empty() || !bulkQueue_.empty() ||
               !running_.load();
    });

    if (!running_.load()) return nullptr;

    // Priority order: CRITICAL > HIGH > NORMAL > LOW > BULK
    if (!criticalQueue_.empty()) {
        auto entry = criticalQueue_.front();
        criticalQueue_.pop();
        return entry;
    }
    if (!highQueue_.empty()) {
        auto entry = highQueue_.front();
        highQueue_.pop();
        return entry;
    }
    if (!normalQueue_.empty()) {
        auto entry = normalQueue_.front();
        normalQueue_.pop();
        return entry;
    }
    if (!lowQueue_.empty()) {
        auto entry = lowQueue_.front();
        lowQueue_.pop();
        return entry;
    }
    if (!bulkQueue_.empty()) {
        auto entry = bulkQueue_.front();
        bulkQueue_.pop();
        return entry;
    }

    return nullptr;
}

size_t RaftCore::getQueueDepth() const {
    std::lock_guard<std::mutex> lk(queueMutex_);
    return criticalQueue_.size() + highQueue_.size() + normalQueue_.size() +
           lowQueue_.size() + bulkQueue_.size();
}

// Worker thread loop
void RaftCore::workerLoop() {
    activeWorkers_++;

    while (running_.load()) {
        auto entry = dequeueOperation();
        if (!entry) continue;
        RAFT_SCHEMA_LOG("[DLOG][BATCH][DEQUEUE]", entry->entry,
                        "priority=" << static_cast<int>(entry->priority)
                        << " queueDepthAfter=" << getQueueDepth());

        std::vector<std::shared_ptr<PriorityEntry>> batch;
        batch.push_back(entry);
        bool batched = false;

        if (raftBatchEnabled() && batchEligible(entry)) {
            ReplicationMode mode = determineReplicationMode(entry->entry, entry->priority);
            if (mode == ReplicationMode::SYNCHRONOUS) {
                collectBatch(batch);
                batched = batch.size() > 1 || raftGroupCommitEnabled();
            }
        }

        try {
            if (batched) {
                bool result = processBatch(batch);
                for (const auto& item : batch) {
                    RAFT_SCHEMA_LOG("[DLOG][BATCH][PROMISE_SET]", item->entry, "result=" << (result ? "true" : "false"));
                    item->result.set_value(result);
                }
            } else {
                bool result = processOperation(entry);
                RAFT_SCHEMA_LOG("[DLOG][BATCH][PROMISE_SET]", entry->entry, "result=" << (result ? "true" : "false"));
                entry->result.set_value(result);
            }
        } catch (const std::exception& e) {
            std::cerr << "[RAFTCORE] Worker exception: " << e.what() << std::endl;
            if (batched) {
                for (const auto& item : batch) {
                    item->result.set_value(false);
                }
            } else {
                entry->result.set_value(false);
            }
            failedRequests_++;
        }
    }

    activeWorkers_--;
}

// Main operation processing
bool RaftCore::processOperation(std::shared_ptr<RaftCore::PriorityEntry> entry) {
    auto startTime = std::chrono::steady_clock::now();

    try {
        // Determine replication mode based on operation and system state
        ReplicationMode mode = determineReplicationMode(entry->entry, entry->priority);

        bool success = false;
        if (mode == ReplicationMode::SYNCHRONOUS) {
            success = replicateSynchronous(entry->entry, entry->timeoutMs);
        } else {
            success = replicateAsynchronous(entry->entry);
        }

        // Log metrics
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);
        logOperationMetrics(entry->entry, success, duration);

        return success;

    } catch (const std::exception& e) {
        std::cerr << "[RAFTCORE] Operation failed: " << e.what() << std::endl;
        failedRequests_++;
        consecutiveFailures_++;
        handleCircuitBreaker();
        return false;
    }
}

bool RaftCore::batchEligible(const std::shared_ptr<RaftCore::PriorityEntry>& entry) const {
    if (!entry) return false;
    if (entry->priority == OperationPriority::CRITICAL) return false;
    return isWriteLikeEntry(entry->entry);
}

void RaftCore::collectBatch(std::vector<std::shared_ptr<RaftCore::PriorityEntry>>& batch) {
    if (batch.empty()) return;

    const size_t maxCount = raftBatchMaxCount();
    const size_t maxBytes = raftBatchMaxBytes();
    if (batch.size() >= maxCount) return;

    int windowMs = raftBatchWindowMs();
    // Under load the queue already is the batch. Waiting the full window again
    // only adds head-of-line latency; use the window solely to collect an idle
    // stream's next arrivals.
    if (windowMs > 0 && getQueueDepth() == 0) {
        int timeoutBound = batch.front()->timeoutMs > 0 ? std::max(0, batch.front()->timeoutMs / 4) : windowMs;
        int sleepMs = std::min(windowMs, timeoutBound);
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }

    size_t bytes = 0;
    for (const auto& item : batch) {
        bytes += encodeRaftPayload(item->entry).size();
    }

    std::lock_guard<std::mutex> lk(queueMutex_);
    std::queue<std::shared_ptr<PriorityEntry>>* queue = nullptr;
    switch (batch.front()->priority) {
        case OperationPriority::CRITICAL: queue = &criticalQueue_; break;
        case OperationPriority::HIGH: queue = &highQueue_; break;
        case OperationPriority::NORMAL: queue = &normalQueue_; break;
        case OperationPriority::LOW: queue = &lowQueue_; break;
        case OperationPriority::BULK: queue = &bulkQueue_; break;
    }

    if (!queue) return;

    while (!queue->empty() && batch.size() < maxCount) {
        auto next = queue->front();
        if (!batchEligible(next)) {
            break;
        }
        size_t nextBytes = encodeRaftPayload(next->entry).size();
        if (bytes + nextBytes > maxBytes) {
            break;
        }
        queue->pop();
        batch.push_back(next);
        bytes += nextBytes;
    }
}

bool RaftCore::processBatch(const std::vector<std::shared_ptr<RaftCore::PriorityEntry>>& batch) {
    if (batch.empty()) return true;

    auto startTime = std::chrono::steady_clock::now();
    auto oldestSubmitted = batch.front()->submitted;
    for (const auto& item : batch) {
        if (item && item->submitted < oldestSubmitted) oldestSubmitted = item->submitted;
    }
    const long long batchWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        startTime - oldestSubmitted).count();
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_raftBatchSize, static_cast<long long>(batch.size()));
        raftRecordMetric(g_raftBatchWaitMs, std::max<long long>(0, batchWaitMs));
    }
    bool success = false;

    try {
        ReplicationMode mode = determineReplicationMode(batch.front()->entry, batch.front()->priority);
        if (mode != ReplicationMode::SYNCHRONOUS) return false;

        int timeoutMs = batch.front()->timeoutMs > 0 ? batch.front()->timeoutMs : 30000;
        for (const auto& item : batch) {
            if (item->timeoutMs > 0) timeoutMs = std::min(timeoutMs, item->timeoutMs);
        }

        for (const auto& item : batch) {
            RAFT_SCHEMA_LOG("[DLOG][BATCH][PROCESS]", item->entry,
                            "batchSize=" << batch.size() << " timeoutMs=" << timeoutMs);
        }
        success = replicateSynchronousBatch(batch, timeoutMs);
    } catch (const std::exception& e) {
        std::cerr << "[RAFTCORE] Batch operation failed: " << e.what() << std::endl;
        failedRequests_++;
        consecutiveFailures_++;
        handleCircuitBreaker();
        return false;
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime);
    for (const auto& item : batch) {
        logOperationMetrics(item->entry, success, duration);
    }

    return success;
}

// Dynamic replication mode determination
ReplicationMode RaftCore::determineReplicationMode(const json& entry, OperationPriority priority) {
    ReplicationMode configured = replicationMode_.load();

    // Enforce quorum-commit for all write-like entries regardless of configured mode.
    if (isWriteLikeEntry(entry)) {
        return ReplicationMode::SYNCHRONOUS;
    }

    // If not adaptive, use configured mode
    if (configured != ReplicationMode::HYBRID_ADAPTIVE) {
        return configured;
    }

    // Adaptive logic based on priority and system state
    if (shouldUseSynchronous(entry, priority)) {
        return ReplicationMode::SYNCHRONOUS;
    }

    return ReplicationMode::ASYNCHRONOUS;
}

bool RaftCore::shouldUseSynchronous(const json& entry, OperationPriority priority) {
    // Critical operations always synchronous
    if (priority == OperationPriority::CRITICAL) {
        return true;
    }

    // High priority under high load becomes synchronous
    if (priority == OperationPriority::HIGH && isUnderHighLoad()) {
        return true;
    }

    // Check operation type (action for router-origin entries, op for engine-origin entries).
    if (isWriteLikeEntry(entry)) {
        return true; // Mutations must commit via quorum before local apply.
    }

    // Check system health
    if (!checkSystemHealth()) {
        return true; // Degrade to sync when unhealthy
    }

    // Check queue depth
    if (getQueueDepth() > maxQueueDepth_.load() * 0.5) {
        return true; // High queue depth forces sync
    }

    return false; // Default to async for performance
}

int RaftCore::reachedCountForIndex(uint64_t targetIndex, long long* replicaIndexMutexWaitUs) {
    int c = 1; // self
    auto waitStart = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
    if (replicaIndexMutexWaitUs) {
        *replicaIndexMutexWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - waitStart).count();
    }
    for (const auto& kv : matchIndex_) {
        if (kv.second >= targetIndex) c++;
    }
    return c;
}

uint64_t RaftCore::computeQuorumIndex(long long* replicaIndexMutexWaitUs) {
    std::vector<uint64_t> idxs;
    idxs.push_back(lastIndex_);  // leader holds everything it appended
    {
        auto waitStart = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
        if (replicaIndexMutexWaitUs) {
            *replicaIndexMutexWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - waitStart).count();
        }
        for (const auto& kv : matchIndex_) idxs.push_back(kv.second);
    }
    std::sort(idxs.begin(), idxs.end(), std::greater<uint64_t>());
    return idxs[(idxs.size() - 1) / 2];  // median = quorum-replicated index
}

// v5.5P-R4.5: compute the median (quorum) replicated index from self + peer matchIndex_
// and complete only waiters whose target is now satisfied. This is per RaftCore instance
// (per shard group), so writers from one shard cannot wake writers from another shard.
void RaftCore::publishQuorumIndex() {
    long long mutexWaitUs = 0;
    uint64_t q = computeQuorumIndex(&mutexWaitUs);
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_replicaIndexMutexWaitUs, mutexWaitUs);
    }

    uint64_t cur = quorumReachedIndex_.load(std::memory_order_relaxed);
    while (cur < q && !quorumReachedIndex_.compare_exchange_weak(cur, q, std::memory_order_acq_rel)) {}

    std::vector<std::shared_ptr<QuorumWaiter>> ready;
    {
        std::lock_guard<std::mutex> lk(quorumWaiterMutex_);
        auto it = quorumWaitersByTarget_.begin();
        while (it != quorumWaitersByTarget_.end() && it->first <= q) {
            for (auto& waiter : it->second) ready.push_back(waiter);
            it = quorumWaitersByTarget_.erase(it);
        }
    }

    for (auto& waiter : ready) {
        if (!waiter) continue;
        if (!waiter->completed.exchange(true, std::memory_order_acq_rel)) {
            quorumWaiterNotifyCount_.fetch_add(1, std::memory_order_relaxed);
            quorumWaiterWakeups_.fetch_add(1, std::memory_order_relaxed);
            quorumWaiterSatisfied_.fetch_add(1, std::memory_order_relaxed);
            waiter->promise.set_value(true);
        }
    }
}

bool RaftCore::waitForQuorumIndex(uint64_t targetIndex, int needed, int timeoutMs, int* observedAcks) {
    if (observedAcks) *observedAcks = 1;
    if (quorumReachedIndex_.load(std::memory_order_acquire) >= targetIndex) {
        if (observedAcks) *observedAcks = needed;
        return true;
    }

    long long mutexWaitUs = 0;
    int nowAcks = reachedCountForIndex(targetIndex, &mutexWaitUs);
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_replicaIndexMutexWaitUs, mutexWaitUs);
    }
    if (observedAcks) *observedAcks = nowAcks;
    if (nowAcks >= needed) {
        publishQuorumIndex();
        return true;
    }

    auto waiter = std::make_shared<QuorumWaiter>();
    waiter->targetIndex = targetIndex;
    auto fut = waiter->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(quorumWaiterMutex_);
        if (quorumReachedIndex_.load(std::memory_order_acquire) >= targetIndex) {
            waiter->completed.store(true, std::memory_order_release);
            waiter->promise.set_value(true);
        } else {
            quorumWaitersByTarget_[targetIndex].push_back(waiter);
        }
    }

    bool ok = false;
    int waitMs = raftQuorumWaitTimeoutMs(timeoutMs);
    if (fut.wait_for(std::chrono::milliseconds(waitMs)) == std::future_status::ready) {
        ok = fut.get();
    } else {
        if (!waiter->completed.exchange(true, std::memory_order_acq_rel)) {
            quorumWaiterTimeouts_.fetch_add(1, std::memory_order_relaxed);
            try { waiter->promise.set_value(false); } catch (...) {}
        }
        std::lock_guard<std::mutex> lk(quorumWaiterMutex_);
        auto it = quorumWaitersByTarget_.find(targetIndex);
        if (it != quorumWaitersByTarget_.end()) {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), waiter), vec.end());
            if (vec.empty()) quorumWaitersByTarget_.erase(it);
        }
    }

    long long afterWaitUs = 0;
    nowAcks = reachedCountForIndex(targetIndex, &afterWaitUs);
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_replicaIndexMutexWaitUs, afterWaitUs);
    }
    if (observedAcks) *observedAcks = nowAcks;
    return ok || nowAcks >= needed || quorumReachedIndex_.load(std::memory_order_acquire) >= targetIndex;
}

void RaftCore::publishAppliedIndex() {
    const uint64_t applied = lastApplied_.load(std::memory_order_acquire);
    std::vector<std::shared_ptr<ApplyWaiter>> ready;
    {
        std::lock_guard<std::mutex> lk(applyWaiterMutex_);
        auto it = applyWaitersByTarget_.begin();
        while (it != applyWaitersByTarget_.end() && it->first <= applied) {
            for (auto& waiter : it->second) ready.push_back(waiter);
            it = applyWaitersByTarget_.erase(it);
        }
    }

    for (auto& waiter : ready) {
        if (!waiter) continue;
        if (!waiter->completed.exchange(true, std::memory_order_acq_rel)) {
            applyWaiterNotifyCount_.fetch_add(1, std::memory_order_relaxed);
            applyWaiterWakeups_.fetch_add(1, std::memory_order_relaxed);
            applyWaiterSatisfied_.fetch_add(1, std::memory_order_relaxed);
            waiter->promise.set_value(true);
        }
    }
}

void RaftCore::publishApplyTarget(uint64_t upToIndex) {
    if (upToIndex == 0) return;
    uint64_t cur = applyWorkerTarget_.load(std::memory_order_relaxed);
    while (cur < upToIndex && !applyWorkerTarget_.compare_exchange_weak(cur, upToIndex, std::memory_order_acq_rel)) {}
    applyWorkerCv_.notify_one();
}

bool RaftCore::waitForAppliedIndex(uint64_t targetIndex, int timeoutMs) {
    if (targetIndex == 0) return true;
    if (lastApplied_.load(std::memory_order_acquire) >= targetIndex) return true;

    auto waiter = std::make_shared<ApplyWaiter>();
    waiter->targetIndex = targetIndex;
    auto fut = waiter->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(applyWaiterMutex_);
        if (lastApplied_.load(std::memory_order_acquire) >= targetIndex) {
            waiter->completed.store(true, std::memory_order_release);
            waiter->promise.set_value(true);
        } else {
            applyWaitersByTarget_[targetIndex].push_back(waiter);
        }
    }

    const auto start = std::chrono::steady_clock::now();
    int waitMs = timeoutMs > 0 ? timeoutMs : 1;
    bool ok = false;
    if (fut.wait_for(std::chrono::milliseconds(waitMs)) == std::future_status::ready) {
        ok = fut.get();
    } else {
        if (!waiter->completed.exchange(true, std::memory_order_acq_rel)) {
            applyWaiterTimeouts_.fetch_add(1, std::memory_order_relaxed);
            try { waiter->promise.set_value(false); } catch (...) {}
        }
        std::lock_guard<std::mutex> lk(applyWaiterMutex_);
        auto it = applyWaitersByTarget_.find(targetIndex);
        if (it != applyWaitersByTarget_.end()) {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), waiter), vec.end());
            if (vec.empty()) applyWaitersByTarget_.erase(it);
        }
    }

    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_applyWaitMs, std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    }

    return ok || lastApplied_.load(std::memory_order_acquire) >= targetIndex;
}

bool RaftCore::applyCommittedOrWait(uint64_t upToIndex,
                                    uint64_t waitForIndex,
                                    int timeoutMs,
                                    ApplySource source) {
    if (upToIndex == 0) return true;
    (void)source; // The mandatory worker records the actual mutation owner.
    publishApplyTarget(upToIndex);
    return waitForAppliedIndex(waitForIndex, timeoutMs);
}

// Follower RPC acknowledgement paths (heartbeat / appendEntries / wal_ship) must not
// perform a durability fsync before replying. applyCommittedOrWait() calls
// persistProgress(), which rewrites and fsyncs raft/consensus_state.json; on a spinning
// disk that costs 40-230ms per RPC, which both delays every ack past the strong-read
// barrier timeout and makes the node a permanent replication straggler.
//
// The acknowledgement does not need that record durable: appended entries are already
// fsynced by raftSyncOrDeferLog() before this point. commitIndex/lastApplied are volatile
// Raft state and consensus_state.json is only a clean-restart replay checkpoint.
void RaftCore::scheduleFollowerApply(uint64_t upToIndex) {
    if (upToIndex == 0) return;
    publishApplyTarget(upToIndex);
}

void RaftCore::applyWorkerLoop() {
    std::cerr << "[RAFTCORE] apply worker started node=" << nodeId_ << std::endl;
    while (running_.load()) {
        uint64_t target = 0;
        {
            std::unique_lock<std::mutex> lk(applyWorkerMutex_);
            applyWorkerCv_.wait_for(lk, std::chrono::milliseconds(10), [&]() {
                return !running_.load() ||
                       applyWorkerTarget_.load(std::memory_order_acquire) >
                           lastApplied_.load(std::memory_order_acquire);
            });
            if (!running_.load()) break;
            target = applyWorkerTarget_.load(std::memory_order_acquire);
        }
        if (target <= lastApplied_.load(std::memory_order_acquire)) continue;

        applyWorkerActive_.store(true, std::memory_order_release);
        try {
            applyCommittedEntries(target, ApplySource::WORKER);
        } catch (const std::exception& e) {
            std::cerr << "[RAFTCORE] apply worker exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[RAFTCORE] apply worker unknown exception" << std::endl;
        }
        applyWorkerActive_.store(false, std::memory_order_release);
    }
    std::cerr << "[RAFTCORE] apply worker stopped node=" << nodeId_ << std::endl;
}

// v5.5P-R4.2.2: ensure exactly one long-lived coalescing replicator per peer. Writers
// (single-write OR group-commit batch) only publish their target index and then wait on
// a per-RaftCore targeted waiter for quorum; this thread streams the contiguous log suffix to the follower
// in bounded chunks and advances matchIndex_. Threads are bounded by peer count, never by
// write/batch concurrency, and the per-peer mutex is NEVER held across network I/O.
void RaftCore::ensurePeerReplicator(const std::string& peer, uint64_t term, uint64_t targetIndex) {
    auto rs = raftPeerReplState(peer);
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lk(rs->m);
        if (targetIndex > rs->target) rs->target = targetIndex;
        uint64_t depth = 0;
        if (targetIndex > rs->lastObservedMatch) depth = targetIndex - rs->lastObservedMatch;
        rs->maxQueueDepth = std::max(rs->maxQueueDepth, depth);
        if (!rs->active) { rs->active = true; spawn = true; }
    }
    rs->cv.notify_one();
    if (!spawn) return;

    const std::string p = peer;
    std::thread([this, p, term, rs]() {
        const int peerRpcTimeoutMs = raftReplicatorRpcTimeoutMs();
        const uint64_t chunkLimit = raftReplicatorMaxChunkEntries();
        const int idleLingerMs = raftReplicatorIdleLingerMs();
        {
            std::lock_guard<std::mutex> lk(rs->m);
            rs->threadStarts++;
        }
        // v5.5P-R5.6: one persistent connection per peer, reused across AppendEntries rounds.
        // The guard closes it on every thread-exit path (step-down, stale term, exception).
        const bool reuseConn = raftReplicatorReuseConnection();
        int heldSock = -1;
        unsigned consecutiveTransportFailures = 0;
        struct HeldSockGuard {
            int& s;
            ~HeldSockGuard() {
                if (s >= 0) {
                    closeRaftNetworkSocket(s);
                    s = -1;
                }
            }
        } _heldGuard{heldSock};
        try {
            for (;;) {
                g_peerReplicatorWakeups.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(rs->m);
                    rs->wakeups++;
                }
                { std::lock_guard<std::mutex> lk(electionMutex_);
                  if (!leader_.load() || currentTerm_ != term) break; }

                uint64_t target;
                { std::lock_guard<std::mutex> lk(rs->m); target = rs->target; }
                uint64_t matchNow;
                { std::lock_guard<std::mutex> rlk(replicaIndexMutex_); matchNow = matchIndex_[p]; }
                {
                    std::lock_guard<std::mutex> lk(rs->m);
                    rs->lastObservedMatch = matchNow;
                    if (target > matchNow) rs->maxQueueDepth = std::max(rs->maxQueueDepth, target - matchNow);
                }
                if (matchNow >= target) {
                    std::unique_lock<std::mutex> lk(rs->m);
                    if (matchNow >= rs->target) {
                        publishQuorumIndex();
                        if (idleLingerMs <= 0) {
                            rs->active = false;
                            return;
                        }
                        const bool reused = rs->cv.wait_for(
                            lk,
                            std::chrono::milliseconds(idleLingerMs),
                            [&]() { return !running_.load(std::memory_order_acquire) || rs->target > matchNow; });
                        if (!running_.load(std::memory_order_acquire)) {
                            rs->active = false;
                            return;
                        }
                        if (!reused || rs->target <= matchNow) {
                            rs->active = false;
                            return;
                        }
                        rs->idleReuses++;
                    }
                    continue;
                }

                uint64_t prevIndex;
                {
                    std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                    auto it = nextIndex_.find(p);
                    if (it != nextIndex_.end() && it->second > 0) prevIndex = it->second - 1;
                    else prevIndex = (target > 0) ? (target - 1) : 0;
                    if (prevIndex > target) prevIndex = target;
                }

                // Bounded chunk: never ship a giant suffix in one RPC (a far-behind
                // follower streams chunkLimit entries per round).
                if (lastSnapshotIndex_ > 0 && prevIndex < lastSnapshotIndex_) {
                    bool installed = backfillCommittedEntriesToPeer(p, prevIndex, target, term, peerRpcTimeoutMs);
                    if (!installed) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                    continue;
                }
                uint64_t chunkTo = std::min(target, prevIndex + chunkLimit);
                std::vector<uint64_t> suffixTerms;
                auto suffixEntries = readRaftEntriesRange(prevIndex + 1, chunkTo, &suffixTerms);
                if (suffixEntries.empty()) {
                    bool installed = backfillCommittedEntriesToPeer(p, prevIndex, target, term, peerRpcTimeoutMs);
                    if (!installed && prevIndex > 0) {
                        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                        if (nextIndex_[p] > 1) nextIndex_[p] = std::max<uint64_t>(1, prevIndex);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                long long suffixBytes = 0;
                for (const auto& e : suffixEntries) {
                    suffixBytes += static_cast<long long>(encodeRaftPayload(e).size());
                }
                {
                    std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
                    raftRecordMetric(g_appendEntriesBatchEntries, static_cast<long long>(suffixEntries.size()));
                    raftRecordMetric(g_appendEntriesBatchBytes, suffixBytes);
                }

                uint64_t leaderCommitSnapshot;
                { std::lock_guard<std::mutex> lk(electionMutex_); leaderCommitSnapshot = commitIndex_; }

                json msg;
                msg["type"] = "append_entries";
                msg["term"] = term;
                msg["leader"] = nodeId_;
                msg["prevLogIndex"] = prevIndex;
                msg["prevLogTerm"] = getTermForIndex(prevIndex);
                msg["leaderCommit"] = leaderCommitSnapshot;
                msg["entries"] = suffixEntries;
                msg["entryTerms"] = suffixTerms;

                bool ack = false; std::string resp;
                auto peerStart = std::chrono::steady_clock::now();
                const int sockBefore = heldSock;
                bool ok = sendToPeer(p, msg, peerRpcTimeoutMs, ack, &resp, reuseConn ? &heldSock : nullptr);
                if (reuseConn && sockBefore >= 0 && heldSock < 0) {
                    std::lock_guard<std::mutex> lk(rs->m);
                    rs->reconnects++;
                }
                g_peerReplicatorChunksSent.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(rs->m);
                    rs->chunksSent++;
                    rs->bytesSent += suffixBytes;
                    rs->lastAckMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - peerStart).count();
                    raftRecordMetric(rs->ackLatencyMs, rs->lastAckMs);
                }
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - peerStart).count() >= peerRpcTimeoutMs) {
                    g_raftSlowFollowerCount.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lk(rs->m);
                    rs->timeouts++;
                }

                uint64_t remoteTerm = 0;
                if (parseStaleTermResponse(resp, remoteTerm)) {
                    observeHigherTerm(remoteTerm);
                    std::lock_guard<std::mutex> lk(rs->m); rs->active = false; publishQuorumIndex(); return;
                }
                uint64_t responseMatch = 0; std::string responseReason;
                try { auto jr = json::parse(resp); responseMatch = jr.value("matchIndex", (uint64_t)0); responseReason = jr.value("reason", std::string()); } catch (...) {}

                if (ok && ack) {
                    consecutiveTransportFailures = 0;
                    uint64_t remoteMatch = responseMatch > 0 ? responseMatch
                        : static_cast<uint64_t>(prevIndex + suffixEntries.size());
                    {
                        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                        matchIndex_[p] = std::max(matchIndex_[p], remoteMatch);
                        nextIndex_[p] = std::max(nextIndex_[p], remoteMatch + 1);
                    }
                    { std::lock_guard<std::mutex> hbLk(peerHeartbeatMutex_); peerLastHeartbeatMs_[p] = steadyNowMs(); }
                    publishQuorumIndex();
                    continue;
                }

                // A powered-off follower must not turn its dedicated replicator
                // into a tight connect/log loop. Quorum progress continues via
                // the healthy follower while this peer backs off independently;
                // the one-second ceiling still gives prompt catch-up on return.
                if (!ok && resp.empty()) {
                    consecutiveTransportFailures = std::min<unsigned>(consecutiveTransportFailures + 1, 32);
                    const unsigned shift = std::min<unsigned>(consecutiveTransportFailures - 1, 5);
                    const int backoffMs = std::min(1000, 50 * (1 << shift));
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                    continue;
                }
                consecutiveTransportFailures = 0;

                g_peerReplicatorConflictRetries.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(rs->m);
                    rs->conflictRetries++;
                    rs->rejectionCount++;
                }
                if (responseReason.find("conflict") != std::string::npos) {
                    std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                    uint64_t ni = nextIndex_[p];
                    if (responseMatch > 0 && responseMatch < ni) nextIndex_[p] = responseMatch + 1;
                    else if (ni > 1) nextIndex_[p] = ni - 1;
                    else nextIndex_[p] = 1;
                } else if (!resp.empty() && resp.rfind("conflict:", 0) == 0) {
                    try {
                        uint64_t peerLast = std::stoull(resp.substr(9));
                        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                        nextIndex_[p] = (peerLast > 0) ? peerLast + 1 : 1;
                    } catch (...) {
                        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                        if (nextIndex_[p] > 1) nextIndex_[p]--;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[RAFTCORE] peer replicator exception (" << p << "): " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[RAFTCORE] peer replicator unknown exception (" << p << ")" << std::endl;
        }
        { std::lock_guard<std::mutex> lk(rs->m); rs->active = false; }
        publishQuorumIndex();
    }).detach();
}

// Synchronous replication (wait for majority)
bool RaftCore::replicateSynchronous(const json& entry, int timeoutMs) {
    auto replicateStart = std::chrono::steady_clock::now();
    const int maxWriteInFlight = raftMaxWriteInFlight();
    int before = g_raftWriteInFlight.fetch_add(1, std::memory_order_relaxed);
    if (before >= maxWriteInFlight) {
        g_raftWriteInFlight.fetch_sub(1, std::memory_order_relaxed);
        g_raftWriteRejected.fetch_add(1, std::memory_order_relaxed);
        g_writeLifecycleRejectedBusy.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[RAFTCORE] Write admission rejected: in_flight="
                  << before << " max=" << maxWriteInFlight << std::endl;
        return false;
    }
    struct InFlightGuard {
        ~InFlightGuard() { g_raftWriteInFlight.fetch_sub(1, std::memory_order_relaxed); }
    } inFlightGuard;

    if (applyAdmissionEnabled()) {
        const uint64_t applied = lastApplied_.load(std::memory_order_acquire);
        const uint64_t lag = commitIndex_ > applied ? (commitIndex_ - applied) : 0;
        size_t waiters = 0;
        {
            std::lock_guard<std::mutex> alk(applyWaiterMutex_);
            for (const auto& kv : applyWaitersByTarget_) waiters += kv.second.size();
        }
        const uint64_t queueDepth = lag + static_cast<uint64_t>(waiters);
        long long applyWaitP99 = 0;
        {
            std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
            applyWaitP99 = metricP99Value(g_applyWaitMs);
        }
        const bool overloaded =
            lag > applyAdmissionMaxLagEntries() ||
            queueDepth > applyAdmissionMaxQueueDepth() ||
            applyWaitP99 > applyAdmissionMaxWaitP99Ms();
        g_applyOverloadActive.store(overloaded ? 1 : 0, std::memory_order_relaxed);
        g_applyRetryAfterMs.store(applyAdmissionRetryAfterMs(), std::memory_order_relaxed);
        if (lag > 0) {
            uint64_t zero = 0;
            g_applyOldestQueuedSinceMs.compare_exchange_strong(zero, steadyNowMs(), std::memory_order_acq_rel);
        } else {
            g_applyOldestQueuedSinceMs.store(0, std::memory_order_release);
        }
        if (overloaded) {
            g_applyAdmissionRejected.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "[RAFTCORE] Apply admission overloaded: lag=" << lag
                      << " queueDepth=" << queueDepth
                      << " waiters=" << waiters
                      << " applyWaitP99=" << applyWaitP99
                      << " retryAfterMs=" << applyAdmissionRetryAfterMs()
                      << std::endl;
            throw std::runtime_error("apply_overloaded");
        }
        g_applyAdmissionAccepted.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t index;
    uint64_t term;
    const std::string payload = encodeRaftLogPayload(entry);

    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (!leader_.load()) {
            std::cerr << "[RAFTCORE] Lost leadership before replication" << std::endl;
            g_writeLifecycleNotLeader.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        term = currentTerm_;
    }

    // === PHASE 1: Persist to Raft log under lock (fast — no fsync) ===
    g_writeLifecycleRaftSubmitted.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(logMutex_);

        std::string base = dataRoot();
        if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";

        index = lastIndex_ + 1;
        std::string bytes;
        bytes.reserve(sizeof(index) + sizeof(term) + sizeof(uint32_t) + payload.size());
        appendRaftRecordBytes(bytes, index, term, payload);
        uint64_t startOffset = 0;
        if (!appendRaftLog(logPath, bytes, &startOffset)) {
            std::cerr << "[RAFTCORE] Failed to open log: " << logPath << std::endl;
            return false;
        }

        lastIndex_ = index;
        cacheRaftRecordBlock(bytes, startOffset);
        lastEntryTerm_ = term;
    }
    // logMutex_ is released here — other writers can proceed in parallel
    raftLogAppendSeqBump(index);

    // === PHASE 2: durably sync the raft log. v5.5P-R4.4 group fsync coalesces the
    // fdatasync across concurrent single-writes (one fsync persists all appended entries).
    {
        std::string base = dataRoot();
        if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";
        raftGroupLogFsync(logPath, index);
    }

    // Wait for majority quorum
    int needed = quorumSize();
    uint64_t newIndex = index; // already assigned above
    const int peerRpcTimeoutMs = raftPeerRpcTimeoutMs(timeoutMs);
    RAFT_CT("replicateSynchronous index=" << newIndex << " term=" << term
            << " needed=" << needed << " peerRpcTimeoutMs=" << peerRpcTimeoutMs);

    auto quorumStart = std::chrono::steady_clock::now();
    std::vector<std::string> peerList;
    for (const auto& peer : peers_) {
        if (!isSelfPeer(peer)) peerList.push_back(peer);
    }

    // v5.5P-R4.2.2: publish our target to the shared per-peer coalescing replicators.
    (void)peerRpcTimeoutMs;
    for (const auto& p : peerList) ensurePeerReplicator(p, term, newIndex);

    // Wait until a quorum (self + peers) has matchIndex_ >= newIndex, bounded by the
    // quorum-wait deadline. No per-write threads are joined here — the replicators are
    // shared and long-lived.
    int observedAcks;
    bool quorumSatisfied = waitForQuorumIndex(newIndex, needed, timeoutMs, &observedAcks);

    auto quorumMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - quorumStart).count();
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_raftQuorumWaitMs, quorumMs);
    }

    RAFT_CT("replicateSynchronous quorum_ordered index=" << newIndex
            << " observedAcks=" << observedAcks << " needed=" << needed);

    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (!leader_.load() || currentTerm_ != term) {
            std::cerr << "[RAFTCORE] Leadership term changed during replication; aborting commit for index="
                      << newIndex << std::endl;
            return false;
        }
    }

    bool success = quorumSatisfied && observedAcks >= needed;
    if (success) {
        g_writeLifecycleQuorumReached.fetch_add(1, std::memory_order_relaxed);
        // v5.5P-R2C: heal commit-order cascades. The previous implementation gated
        // every commit behind a strict in-order CV (apply only when
        // lastApplied + 1 == newIndex). A single write that failed quorum left
        // lastApplied_ stuck, forcing every following writer to block the full
        // quorum-wait timeout and then fail — a cascade that produced the
        // 100-client 0.401% error rate and 15.3s write P99. We instead advance
        // commitIndex to the majority-replicated index and drive the canonical
        // in-order applier. Index assignment and the log append are atomic under
        // logMutex_, so log.bin is always a contiguous prefix; applyCommittedEntries()
        // therefore applies every entry up to commitIndex with no holes and never
        // skips a committed write (an entry stranded by a writer that gave up is
        // still on the quorum's contiguous log and gets applied here once a later
        // entry advances commitIndex past it). No CV wait, so no head-of-line stall.
        uint64_t majorityIndex = newIndex;
        {
            long long mutexWaitUs = 0;
            majorityIndex = computeQuorumIndex(&mutexWaitUs);
            std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
            raftRecordMetric(g_replicaIndexMutexWaitUs, mutexWaitUs);
        }
        // Quorum acked this entry at newIndex in the current term, so newIndex is
        // safely committable even if a slow follower's matchIndex hasn't posted yet.
        if (majorityIndex < newIndex) majorityIndex = newIndex;
        {
            std::lock_guard<std::mutex> lk(electionMutex_);
            if (majorityIndex > commitIndex_) {
                commitIndex_ = majorityIndex;
                RAFT_DLOG("[RAFTCORE] Advanced commitIndex to " << commitIndex_ << std::endl);
                RAFT_TLOG("commit_index_advanced index=" << commitIndex_
                          << " last_applied=" << lastApplied_.load() << "\n");
            }
        }

        // Apply all contiguous committed entries in order. In R4.6 the env-gated
        // apply worker owns heavy local apply work and writers wait on a targeted
        // applied-index waiter; legacy mode still applies inline.
        try {
            success = applyCommittedOrWait(
                commitIndex_, newIndex, timeoutMs, ApplySource::LEADER_ASYNC);
        } catch (const std::exception& e) {
            std::cerr << "[RAFTCORE] applyCommittedEntries failed for index=" << newIndex
                      << ": " << e.what() << std::endl;
            success = false;
        }
        // Wake legacy waiters (readers / strong-read barrier) that watch apply progress.
        g_raftCommitOrderCv.notify_all();
        if (success) {
            g_writeLifecycleApplied.fetch_add(1, std::memory_order_relaxed);
            RAFT_TLOG("last_applied_update index=" << newIndex
                      << " commit_index=" << commitIndex_ << "\n");
        } else {
            std::cerr << "[RAFTCORE] Quorum reached but local apply lagged for index="
                      << newIndex << " lastApplied=" << lastApplied_.load() << std::endl;
        }
    } else {
        g_writeLifecycleFailed.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[RAFTCORE] Quorum not met for entry index=" << newIndex << " acks=" << observedAcks << " needed=" << needed << std::endl;
    }
    {
        auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - replicateStart).count();
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_raftReplicationMs, totalMs);
    }
    return success;
}

bool RaftCore::replicateSynchronousBatch(const std::vector<std::shared_ptr<RaftCore::PriorityEntry>>& batch,
                                         int timeoutMs) {
    if (batch.empty()) return true;
    const auto replicateStart = std::chrono::steady_clock::now();

    // Serialize only term capture and log admission. The log mutex assigns contiguous
    // indexes; quorum/apply waiters already complete by target index in that order.
    // Keeping this lock across fsync, quorum, and apply serialized the whole pipeline.
    const auto serialWaitStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> serialGuard(replicationMutex_);
    {
        const auto serialWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - serialWaitStart).count();
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_raftReplicationSerialWaitMs, serialWaitMs);
    }

    uint64_t term;
    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (!leader_.load()) {
            std::cerr << "[RAFTCORE] Lost leadership before batch replication" << std::endl;
            g_writeLifecycleNotLeader.fetch_add(static_cast<long long>(batch.size()), std::memory_order_relaxed);
            return false;
        }
        term = currentTerm_;
    }

    std::vector<std::string> payloads;
    payloads.reserve(batch.size());
    for (const auto& item : batch) {
        RAFT_SCHEMA_LOG("[DLOG][BATCH][APPEND]", item->entry, "preparing batch entry");
        payloads.push_back(encodeRaftLogPayload(item->entry));
    }

    uint64_t firstIndex = 0;
    uint64_t lastIndex = 0;

    // === PHASE 1: Persist batch to Raft log under lock (fast — no fsync) ===
    g_writeLifecycleRaftSubmitted.fetch_add(static_cast<long long>(batch.size()), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(logMutex_);

        std::string base = dataRoot();
        if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";

        std::string bytes;
        for (const auto& payload : payloads) {
            uint64_t index = lastIndex_ + 1;
            if (firstIndex == 0) firstIndex = index;
            appendRaftRecordBytes(bytes, index, term, payload);
            lastIndex_ = index;
            lastIndex = index;
        }
        uint64_t startOffset = 0;
        if (!appendRaftLog(logPath, bytes, &startOffset)) {
            lastIndex_ = firstIndex - 1;
            std::cerr << "[RAFTCORE] Failed to append log: " << logPath << std::endl;
            return false;
        }
        cacheRaftRecordBlock(bytes, startOffset);
        lastEntryTerm_ = term;
    }
    raftLogAppendSeqBump(lastIndex);
    serialGuard.unlock();
    for (const auto& item : batch) {
        RAFT_SCHEMA_LOG("[DLOG][BATCH][APPEND_OK]", item->entry,
                        "firstIndex=" << firstIndex << " lastIndex=" << lastIndex);
    }
    // === PHASE 2: durably sync the raft log. v5.5P-R4.4 group fsync (shared with the
    // single-write path) coalesces the fdatasync across concurrent writers/batches.
    {
        std::string base = dataRoot();
        if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";
        for (const auto& item : batch) RAFT_SCHEMA_LOG("[DLOG][BATCH][FSYNC]", item->entry, "start");
        raftGroupLogFsync(logPath, lastIndex);
        for (const auto& item : batch) RAFT_SCHEMA_LOG("[DLOG][BATCH][FSYNC]", item->entry, "done");
    }

    int needed = quorumSize();
    std::vector<std::thread> workers;
    auto ackState = std::make_shared<RaftAckState>();
    ackState->acks = 1; // self
    ackState->needed = needed;
    uint64_t leaderCommitSnapshot = 0;

    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        leaderCommitSnapshot = commitIndex_;
    }

    auto entriesPtr = std::make_shared<std::vector<json>>();
    entriesPtr->reserve(batch.size());
    for (const auto& item : batch) {
        entriesPtr->push_back(item->entry);
    }


    const int peerRpcTimeoutMs = raftPeerRpcTimeoutMs(timeoutMs);
    for (const auto& item : batch) {
        RAFT_SCHEMA_LOG("[DLOG][BATCH][REPLICATE_START]", item->entry,
                        "needed=" << needed << " peerRpcTimeoutMs=" << peerRpcTimeoutMs);
    }
    RAFT_CT("replicateSynchronousBatch first=" << firstIndex << " last=" << lastIndex
            << " term=" << term << " needed=" << needed
            << " peerRpcTimeoutMs=" << peerRpcTimeoutMs);
    int observedAcks = 1;
    const auto quorumStart = std::chrono::steady_clock::now();
    if (raftBatchCoalescingReplicatorEnabled()) {
        // v5.5P-R4.2.2: publish the batch target to the shared per-peer coalescing
        // replicators and wait for quorum by matchIndex. No per-batch-per-peer worker
        // threads and no peer mutex held across network I/O — that was the R4.2.1
        // throughput collapse (write P99 ~30s). The replicators stream the contiguous
        // log suffix in bounded chunks, so correctness (no gaps/divergence) is preserved.
        std::vector<std::string> peerList;
        for (const auto& pp : peers_) if (!isSelfPeer(pp)) peerList.push_back(pp);
        for (const auto& pp : peerList) ensurePeerReplicator(pp, term, lastIndex);
        bool quorumSatisfied = waitForQuorumIndex(lastIndex, needed, timeoutMs, &observedAcks);
        (void)quorumSatisfied;
    } else {
    for (const auto& p : peers_) {
        if (isSelfPeer(p)) continue;  // Skip replicating to ourselves
        ackState->total++;
        workers.emplace_back([this, p, entriesPtr, term, firstIndex, lastIndex, leaderCommitSnapshot, peerRpcTimeoutMs, ackState]() {
            struct WorkerDone { std::shared_ptr<RaftAckState> st;
                ~WorkerDone() { { std::lock_guard<std::mutex> lk(st->m); st->finished++; } st->cv.notify_all(); } } _done{ackState};
            // v5.5P-R4.2.1 FIX: serialize replication to each peer. Under load the batch
            // path spawns one worker per batch per peer, so many workers raced on the same
            // follower (each reading nextIndex_[p] and shipping an overlapping suffix with
            // its own prevLogIndex). Interleaved AppendEntries corrupted the follower's log
            // (it fell permanently behind and never reconverged -> ackLoss under load). The
            // per-peer mutex makes replication to a given follower sequential and ordered,
            // exactly like the single-write coalescing replicator, while different peers
            // still replicate in parallel.
            auto peerLock = raftPeerMutex(p);
            std::lock_guard<std::mutex> peerGuard(*peerLock);
            bool ack = false;
            int attempts = 0;
            int maxAttempts = 6;
            uint64_t prevIndex = 0;
            {
                std::lock_guard<std::mutex> lk(replicaIndexMutex_);
                auto it = nextIndex_.find(p);
                if (it != nextIndex_.end() && it->second > 0) prevIndex = it->second - 1;
                else prevIndex = (firstIndex > 0) ? (firstIndex - 1) : 0;
                // If a concurrent batch already advanced this peer past our range, skip.
                if (matchIndex_[p] >= lastIndex) {
                    { std::lock_guard<std::mutex> lk2(ackState->m); ++ackState->acks; }
                    ackState->cv.notify_all();
                    return;
                }
            }

            while (attempts++ < maxAttempts) {
                if (lastSnapshotIndex_ > 0 && prevIndex < lastSnapshotIndex_) {
                    try {
                        std::string snapshotResponse;
                        const bool installed = sendSnapshotToPeer(
                            p, term, peerRpcTimeoutMs, snapshotResponse);
                        uint64_t remoteTerm = 0;
                        if (parseStaleTermResponse(snapshotResponse, remoteTerm)) {
                            observeHigherTerm(remoteTerm);
                            break;
                        }
                        if (installed) {
                            prevIndex = lastSnapshotIndex_;
                            {
                                std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                                matchIndex_[p] = prevIndex;
                                nextIndex_[p] = prevIndex + 1;
                            }
                            if (prevIndex >= lastIndex) {
                                { std::lock_guard<std::mutex> lk2(ackState->m); ++ackState->acks; }
                                ackState->cv.notify_all();
                                break;
                            }
                            continue;
                        }
                    } catch (...) {}
                }

                // v5.5P-R4.2.1 FIX: send the CONTIGUOUS log suffix [prevIndex+1 .. lastIndex],
                // not just this batch's own entries. A follower abandoned at quorum can be
                // behind firstIndex (it missed a previously-committed batch). The old code
                // always shipped *entriesPtr (firstIndex..lastIndex) while only decrementing
                // prevIndex on conflict, so for a lagging follower it sent e.g. [INSERT@4]
                // with prevLogIndex=2 — the follower then appended the INSERT right after
                // index 2, silently SKIPPING the committed createCollection@3 and diverging
                // its log permanently (INSERT overwrote the missing createCollection's slot
                // -> follower count/hash never converged). The whole batch is already durable
                // in log.bin (PHASE 1/2 above), so re-read the gap-free suffix here.
                std::vector<json> suffixEntries;
                std::vector<uint64_t> suffixTerms;
                if (prevIndex + 1 >= firstIndex) {
                    suffixEntries = *entriesPtr;  // follower is caught up to firstIndex-1
                    suffixTerms.assign(suffixEntries.size(), term);
                } else {
                    suffixEntries = readRaftEntriesRange(prevIndex + 1, lastIndex, &suffixTerms);
                    if (suffixEntries.empty()) {
                        // Never attach the current batch to an older prevLogIndex.
                        // Retry so a newer snapshot can establish the boundary.
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        continue;
                    }
                }

                json msg;
                msg["type"] = "append_entries";
                msg["term"] = term;
                msg["leader"] = nodeId_;
                msg["prevLogIndex"] = prevIndex;
                msg["prevLogTerm"] = getTermForIndex(prevIndex);
                msg["leaderCommit"] = leaderCommitSnapshot;
                msg["entries"] = suffixEntries;
                msg["entryTerms"] = suffixTerms;
                std::string resp;
                bool ok = sendToPeer(p, msg, peerRpcTimeoutMs, ack, &resp);
                RAFT_CT("batch_append peer=" << p << " prevIndex=" << prevIndex
                        << " first=" << firstIndex << " last=" << lastIndex
                        << " ok=" << ok << " ack=" << ack << " resp=" << resp.substr(0, 160));
                uint64_t remoteTerm = 0;
                if (parseStaleTermResponse(resp, remoteTerm)) {
                    observeHigherTerm(remoteTerm);
                    break;
                }
                if (ok && ack) {
                    {
                        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                        matchIndex_[p] = lastIndex;
                        nextIndex_[p] = lastIndex + 1;
                    }
                    { std::lock_guard<std::mutex> lk2(ackState->m); ++ackState->acks; }
                    ackState->cv.notify_all();
                    break;
                }

                if (!resp.empty()) {
                    if (resp.rfind("conflict:", 0) == 0) {
                        try {
                            uint64_t peerLast = std::stoull(resp.substr(9));
                            if (peerLast < prevIndex) {
                                prevIndex = peerLast;
                            } else if (peerLast > 0 && peerLast < lastIndex) {
                                prevIndex = peerLast;
                            } else if (prevIndex > 0) {
                                prevIndex = prevIndex > 1 ? prevIndex - 1 : 0;
                            } else {
                                break;
                            }
                        } catch (...) {
                            if (prevIndex > 0) prevIndex--;
                        }
                    } else {
                        if (prevIndex > 0) prevIndex--;
                        else break;
                    }
                } else {
                    if (prevIndex > 0) prevIndex--;
                    else break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Fallback path: ship the full batch as WAL payload when append retries fail.
            if (!ack) {
                json walShip;
                walShip["type"] = "wal_ship";
                walShip["term"] = term;
                walShip["leader"] = nodeId_;
                walShip["leaderCommit"] = leaderCommitSnapshot;
                walShip["entries"] = *entriesPtr;
                bool walAck = false;
                std::string walResp;
                bool okWal = sendToPeer(p, walShip, peerRpcTimeoutMs, walAck, &walResp);
                RAFT_CT("batch_wal_ship peer=" << p << " last=" << lastIndex
                        << " ok=" << okWal << " ack=" << walAck << " resp=" << walResp.substr(0, 160));
                uint64_t remoteTerm = 0;
                if (parseStaleTermResponse(walResp, remoteTerm)) {
                    observeHigherTerm(remoteTerm);
                } else if (okWal && walAck) {
                    {
                        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                        matchIndex_[p] = lastIndex;
                        nextIndex_[p] = lastIndex + 1;
                    }
                    { std::lock_guard<std::mutex> lk2(ackState->m); ++ackState->acks; }
                    ackState->cv.notify_all();
                }
            }
        });
    }

        observedAcks = raftWaitForQuorum(ackState, timeoutMs);
        for (auto& t : workers) {
            if (t.joinable()) t.detach();
        }
    } // end legacy per-batch-per-peer replication path
    const long long quorumMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - quorumStart).count();
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_raftBatchQuorumMs, quorumMs);
        raftRecordMetric(g_raftQuorumWaitMs, quorumMs);
    }
    RAFT_CT("replicateSynchronousBatch quorum_wait first=" << firstIndex
            << " last=" << lastIndex << " observedAcks=" << observedAcks
            << " needed=" << needed << " finished=" << ackState->finished
            << "/" << ackState->total);

    {
        std::lock_guard<std::mutex> lk(electionMutex_);
        if (!leader_.load() || currentTerm_ != term) {
            std::cerr << "[RAFTCORE] Leadership term changed during batch replication" << std::endl;
            return false;
        }
    }

    bool success = observedAcks >= needed;
    if (success) {
        g_writeLifecycleQuorumReached.fetch_add(static_cast<long long>(batch.size()), std::memory_order_relaxed);
        for (const auto& item : batch) {
            RAFT_SCHEMA_LOG("[DLOG][BATCH][QUORUM_OK]", item->entry,
                            "acks=" << observedAcks << " needed=" << needed);
        }
        const auto applyStart = std::chrono::steady_clock::now();
        long long mutexWaitUs = 0;
        uint64_t majorityIndex = computeQuorumIndex(&mutexWaitUs);
        {
            std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
            raftRecordMetric(g_replicaIndexMutexWaitUs, mutexWaitUs);
        }
        if (majorityIndex >= lastIndex) {
            std::lock_guard<std::mutex> lk(electionMutex_);
            if (majorityIndex > commitIndex_) {
                commitIndex_ = majorityIndex;
                RAFT_DLOG("[RAFTCORE] Advanced commitIndex to " << commitIndex_ << std::endl);
            }
        }
        for (const auto& item : batch) {
            RAFT_SCHEMA_LOG("[DLOG][BATCH][COMMIT]", item->entry, "commitIndex=" << commitIndex_);
        }

        try {
            success = applyCommittedOrWait(
                commitIndex_, lastIndex, timeoutMs, ApplySource::LEADER_ASYNC);
        } catch (const std::exception& e) {
            std::cerr << "[RAFTCORE] Quorum reached but batch apply failed: " << e.what() << std::endl;
            success = false;
        }
        const long long applyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - applyStart).count();
        {
            std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
            raftRecordMetric(g_raftBatchApplyMs, applyMs);
        }
        if (success) {
            g_writeLifecycleApplied.fetch_add(static_cast<long long>(batch.size()), std::memory_order_relaxed);
        } else {
            g_writeLifecycleFailed.fetch_add(static_cast<long long>(batch.size()), std::memory_order_relaxed);
        }
    } else {
        g_writeLifecycleFailed.fetch_add(static_cast<long long>(batch.size()), std::memory_order_relaxed);
        std::cerr << "[RAFTCORE] Quorum not met for batch lastIndex=" << lastIndex
                  << " acks=" << observedAcks << " needed=" << needed << std::endl;
    }

    {
        auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - replicateStart).count();
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_raftReplicationMs, totalMs);
    }

    return success;
}

// Kept for API compatibility. A leader may not expose a write locally before
// quorum commit, so this path uses the same ordered commit/apply pipeline as
// every acknowledged write.
bool RaftCore::replicateAsynchronous(const json& entry) {
    return replicateSynchronous(entry, 30000);
}

bool RaftCore::sendToPeer(const std::string& peer, const json& payload, int timeoutMs, bool& acked,
                          std::string* respStr, int* persistentSock) {
    acked = false;
    if (respStr) respStr->clear();
    const bool reuseSock = (persistentSock != nullptr);
    try {
        const std::string rpcType = payload.value("type", std::string());
        const bool isAppendRpc = rpcType == "append_entries" || rpcType == "appendEntries";
        json rpc = payload;
        static const std::string clusterId = [] {
            const char* value = std::getenv("RAFT_CLUSTER_ID");
            return value ? std::string(value) : std::string();
        }();
        if (!clusterId.empty() && rpc.is_object() && rpc.contains("type") &&
            !rpc.contains("clusterId")) {
            rpc["clusterId"] = clusterId;
        }
        const std::string wirePayload = encodeRaftPayload(rpc);
        static const int64_t sendDelayMs = raftEnvIntMs("RAFT_SEND_DELAY_MS");
        static const int64_t sendJitterMs = raftEnvIntMs("RAFT_SEND_JITTER_MS");
        int64_t baseDelay = sendDelayMs;
        int64_t jitterDelay = sendJitterMs;
        if (rpcType == "heartbeat") {
            static const int64_t heartbeatDelayMs = raftEnvIntMs("RAFT_HEARTBEAT_DELAY_MS", sendDelayMs);
            static const int64_t heartbeatJitterMs = raftEnvIntMs("RAFT_HEARTBEAT_JITTER_MS", sendJitterMs);
            baseDelay = heartbeatDelayMs;
            jitterDelay = heartbeatJitterMs;
        } else if (isAppendRpc) {
            static const int64_t appendDelayMs = raftEnvIntMs("RAFT_APPEND_DELAY_MS", sendDelayMs);
            static const int64_t appendJitterMs = raftEnvIntMs("RAFT_APPEND_JITTER_MS", sendJitterMs);
            baseDelay = appendDelayMs;
            jitterDelay = appendJitterMs;
        }
        raftMaybeDelayMs(baseDelay, jitterDelay);

        std::string endpoint = peerEndpoint(peer);
        std::string host = endpoint;
        int port = 9001;
        auto pos = endpoint.rfind(":");
        if (pos != std::string::npos) {
            host = endpoint.substr(0,pos);
            port = std::stoi(endpoint.substr(pos+1));
        }

    // v5.5P-R5.6: reuse a held connection when one was passed in and is still open.
    int sock = reuseSock ? *persistentSock : -1;
    const bool needConnect = (sock < 0);
    if (needConnect) {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    auto socketStart = std::chrono::steady_clock::now();
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (isAppendRpc) {
        long long socketUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - socketStart).count();
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_replicatorSocketCreateUs, socketUs);
        g_peerReplicatorSocketCreates.fetch_add(1, std::memory_order_relaxed);
    }
    if (sock < 0) {
        if (reuseSock) *persistentSock = -1;
#ifdef _WIN32
        std::cerr << "[RAFTCORE] socket() failed (WSA=" << WSAGetLastError() << ")" << std::endl;
#else
        std::cerr << "[RAFTCORE] socket() failed (errno=" << errno << ")" << std::endl;
#endif
        return false;
    }

        // set send/recv timeouts to avoid blocking indefinitely
    #ifdef _WIN32
        DWORD to = static_cast<DWORD>(timeoutMs);
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    #else
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        // v5.5P-R3: disable Nagle. Each RPC is a small request/response on a fresh
        // connection; with Nagle the 4-byte length prefix and the payload land in
        // separate segments and the second waits for the peer's delayed ACK (~40ms
        // stall). That was the dominant per-round cost capping writes at ~600/s.
        if (raftTcpNoDelayEnabled()) { int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    #endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        // Bounded connect wait. Under load some platforms surface an in-progress
        // nonblocking connect as EINPROGRESS/EALREADY; that is not a dead peer.
        // Wait for writability and then inspect SO_ERROR before declaring failure.
        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                timeval ctv;
                ctv.tv_sec = timeoutMs / 1000;
                ctv.tv_usec = (timeoutMs % 1000) * 1000;
                int sr = select(0, nullptr, &wfds, nullptr, &ctv);
                if (sr > 0 && FD_ISSET(sock, &wfds)) {
                    int soerr = 0;
                    int slen = sizeof(soerr);
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &slen) == 0) {
                        err = soerr;
                    }
                } else if (sr == 0) {
                    err = WSAETIMEDOUT;
                }
            }
            if (err != 0) {
                std::cerr << "[RAFTCORE] connect() to " << host << ":" << port << " failed (WSA=" << err << ")" << std::endl;
                if (isAppendRpc) g_peerReplicatorReconnects.fetch_add(1, std::memory_order_relaxed);
                closeRaftNetworkSocket(sock);
                if (reuseSock) *persistentSock = -1;
                return false;
            }
#else
            int err = errno;
            if (err == EINPROGRESS || err == EALREADY || err == EWOULDBLOCK) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                timeval ctv;
                ctv.tv_sec = timeoutMs / 1000;
                ctv.tv_usec = (timeoutMs % 1000) * 1000;
                int sr = select(sock + 1, nullptr, &wfds, nullptr, &ctv);
                if (sr > 0 && FD_ISSET(sock, &wfds)) {
                    int soerr = 0;
                    socklen_t slen = sizeof(soerr);
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0) {
                        err = soerr;
                    }
                } else if (sr == 0) {
                    err = ETIMEDOUT;
                }
            }
            if (err != 0) {
                std::cerr << "[RAFTCORE] connect() to " << host << ":" << port << " failed (errno=" << err << ")" << std::endl;
                if (isAppendRpc) g_peerReplicatorReconnects.fetch_add(1, std::memory_order_relaxed);
                closeRaftNetworkSocket(sock);
                if (reuseSock) *persistentSock = -1;
                return false;
            }
#endif
        }
        if (!attachRaftTls(sock, false, host)) {
            closeRaftNetworkSocket(sock);
            if (reuseSock) *persistentSock = -1;
            return false;
        }
        if (reuseSock) *persistentSock = sock;  // v5.5P-R5.6: store fresh connection for reuse
    } // end if (needConnect)

        raftThrottleBytes(wirePayload.size() + sizeof(uint32_t));

        uint32_t len = static_cast<uint32_t>(wirePayload.size());
        // Single coalesced write (length prefix + payload) so the request leaves in one
        // segment — avoids the Nagle/delayed-ACK interaction on the framed protocol.
        std::string framed;
        framed.reserve(sizeof(len) + wirePayload.size());
        framed.append(reinterpret_cast<const char*>(&len), sizeof(len));
        framed.append(wirePayload);
        auto sendStart = std::chrono::steady_clock::now();
        size_t off = 0;
        while (off < framed.size()) {
            int sn = raftSocketSend(sock, framed.data() + off,
                                    framed.size() - off, 0);
            if (sn <= 0) { std::cerr << "[RAFTCORE] send() incomplete to peer " << peer << " off=" << off << std::endl; break; }
            off += sn;
        }
        if (off < framed.size() && reuseSock) {
            // v5.5P-R5.6: a reused connection broke mid-send; drop it so we reconnect next round
            closeRaftNetworkSocket(sock); *persistentSock = -1; return false;
        }
        if (isAppendRpc) {
            long long sendUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - sendStart).count();
            std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
            raftRecordMetric(g_replicatorSendUs, sendUs);
        }

        // wait for ack (simple protocol)
        std::string resp;
        char buf[1024];
        auto ackStart = std::chrono::steady_clock::now();
        int r = raftSocketRecv(sock, buf, sizeof(buf)-1, 0);
        if (isAppendRpc) {
            long long ackMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - ackStart).count();
            std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
            raftRecordMetric(g_replicatorAckMs, ackMs);
            if (ackMs >= timeoutMs) g_peerReplicatorTimeouts.fetch_add(1, std::memory_order_relaxed);
        }
        if (r > 0) {
            resp.assign(buf, r);
            RAFT_DLOG("[RAFTCORE] Received ack from " << peer << " -> '" << resp << "'" << std::endl);
            uint64_t remoteTerm = 0;
            if (parseStaleTermResponse(resp, remoteTerm)) {
                observeHigherTerm(remoteTerm);
            }
            if (resp.find("ok") != std::string::npos) acked = true;
            if (resp.find("vote_granted") != std::string::npos) acked = true;
            try {
                auto jr = json::parse(resp);
                uint64_t jt = jr.value("term", (uint64_t)0);
                if (jt > 0 && jt > currentTerm_) observeHigherTerm(jt);
                if (jr.value("voteGranted", false) || jr.value("success", false) || jr.value("status", "") == "ok") {
                    acked = true;
                }
            } catch (...) {}
            if (respStr) *respStr = resp;
        } else {
            std::cerr << "[RAFTCORE] No ack received from " << peer << " (r=" << r << ")" << std::endl;
            if (reuseSock) {
                // v5.5P-R5.6: peer closed / recv failed on a reused connection -> drop it
                closeRaftNetworkSocket(sock); *persistentSock = -1; return false;
            }
        }

        if (!reuseSock) {
            closeRaftNetworkSocket(sock);
        }
        return true;
    } catch (...) {
        if (reuseSock && persistentSock && *persistentSock >= 0) {
            closeRaftNetworkSocket(*persistentSock);
            *persistentSock = -1;
        }
        return false;
    }
}

static size_t raftLogIndexCacheMaxEntries() {
    static const size_t cap = [] {
        if (const char* v = std::getenv("RAFT_TERM_CACHE_MAX")) {
            try { return std::max<size_t>(1024, std::stoull(v)); } catch (...) {}
        }
        return size_t{500000};
    }();
    return cap;
}

void RaftCore::cacheIndexTerm(uint64_t index, uint64_t term) {
    cacheIndexTermAndOffset(index, term, std::numeric_limits<uint64_t>::max());
}

void RaftCore::cacheIndexTermAndOffset(uint64_t index, uint64_t term, uint64_t offset) {
    if (index == 0) return;
    std::lock_guard<std::mutex> lk(termCacheMutex_);
    const size_t cap = raftLogIndexCacheMaxEntries();
    if (termCache_.size() >= cap || logOffsetCache_.size() >= cap) {
        uint64_t highest = 0;
        for (const auto& kv : termCache_) highest = std::max(highest, kv.first);
        for (const auto& kv : logOffsetCache_) highest = std::max(highest, kv.first);
        const uint64_t cutoff = highest > (cap / 2) ? highest - (cap / 2) : 0;
        for (auto it = termCache_.begin(); it != termCache_.end();) {
            if (it->first < cutoff) it = termCache_.erase(it); else ++it;
        }
        for (auto it = logOffsetCache_.begin(); it != logOffsetCache_.end();) {
            if (it->first < cutoff) it = logOffsetCache_.erase(it); else ++it;
        }
    }
    if (term != 0) termCache_[index] = term;
    if (offset != std::numeric_limits<uint64_t>::max()) logOffsetCache_[index] = offset;
}

void RaftCore::cacheRaftRecordBlock(const std::string& bytes, uint64_t startOffset) {
    std::size_t cursor = 0;
    while (bytes.size() - cursor >= sizeof(uint64_t) * 2 + sizeof(uint32_t)) {
        const uint64_t offset = startOffset + cursor;
        uint64_t index = 0;
        uint64_t term = 0;
        uint32_t size = 0;
        std::memcpy(&index, bytes.data() + cursor, sizeof(index));
        cursor += sizeof(index);
        std::memcpy(&term, bytes.data() + cursor, sizeof(term));
        cursor += sizeof(term);
        std::memcpy(&size, bytes.data() + cursor, sizeof(size));
        cursor += sizeof(size);
        if (size == 0 || size > kMaxRaftPayloadBytes || bytes.size() - cursor < size) return;
        cacheIndexTermAndOffset(index, term, offset);
        cursor += size;
    }
}

void RaftCore::invalidateTermCache() {
    std::lock_guard<std::mutex> lk(termCacheMutex_);
    termCache_.clear();
    logOffsetCache_.clear();
}

uint64_t RaftCore::getTermForIndex(uint64_t index) {
    if (index == 0) return 0;
    // v5.5P-R2B: entries up to lastSnapshotIndex_ are compacted out of log.bin.
    // Answer the compaction-boundary query from the snapshot metadata so prevLogTerm
    // stays correct after compaction; otherwise the scan below returns 0 and the
    // follower rejects the append with a phantom prev_term_conflict.
    if (lastSnapshotIndex_ > 0 && index == lastSnapshotIndex_ && lastSnapshotTerm_ > 0) {
        return lastSnapshotTerm_;
    }
    {
        std::lock_guard<std::mutex> lk(termCacheMutex_);
        auto it = termCache_.find(index);
        if (it != termCache_.end()) return it->second;
    }
    try {
        std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";
        std::ifstream in(logPath, std::ios::binary);
        if (!in.is_open()) return 0;
        while (!in.eof()) {
            const std::streamoff entryStart = in.tellg();
            uint64_t idx = 0;
            uint64_t term = 0;
            uint32_t sz = 0;
            in.read(reinterpret_cast<char*>(&idx), sizeof(idx));
            if (!in) break;
            in.read(reinterpret_cast<char*>(&term), sizeof(term));
            in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
            if (!in) break;
            // skip payload
            in.seekg(sz, std::ios::cur);
            // Cache every pair the scan passes over, so one scan warms the whole log
            // instead of each index costing its own traversal.
            if (entryStart >= 0) {
                cacheIndexTermAndOffset(idx, term, static_cast<uint64_t>(entryStart));
            } else {
                cacheIndexTerm(idx, term);
            }
            if (idx == index) {
                in.close();
                return term;
            }
        }
        in.close();
    } catch (...) {}
    return 0;
}

std::vector<json> RaftCore::readRaftEntriesRange(
    uint64_t fromIndex,
    uint64_t toIndex,
    std::vector<uint64_t>* entryTerms) {
    std::vector<json> entries;
    if (entryTerms) entryTerms->clear();
    if (fromIndex == 0 || toIndex < fromIndex) return entries;

    try {
        std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";
        // Compaction rewrites log.bin. Keep the requested range on one physical
        // log generation so a post-snapshot suffix cannot silently lose a prefix.
        //
        // Only the open needs the lock. Compaction replaces log.bin by rename, and an
        // already-open descriptor keeps referring to the original inode, so the read is
        // still pinned to a single generation once the handle exists. Holding logMutex_
        // across the whole read blocked the write-append path, which takes the same mutex:
        // the leader's 500 ms heartbeat fires a catch-up read for any lagging follower, so
        // every heartbeat stalled foreground writes for the duration of that disk read.
        std::ifstream in;
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            in.open(logPath, std::ios::binary);
            if (in.is_open()) {
                std::lock_guard<std::mutex> cacheLock(termCacheMutex_);
                const auto cached = logOffsetCache_.find(fromIndex);
                if (cached != logOffsetCache_.end()) {
                    in.seekg(static_cast<std::streamoff>(cached->second), std::ios::beg);
                }
            }
        }
        if (!in.is_open()) return entries;

        uint64_t expectedIndex = fromIndex;

        while (!in.eof()) {
            const std::streamoff entryStart = in.tellg();
            uint64_t idx = 0;
            uint64_t term = 0;
            uint32_t sz = 0;
            in.read(reinterpret_cast<char*>(&idx), sizeof(idx));
            if (!in) break;
            in.read(reinterpret_cast<char*>(&term), sizeof(term));
            in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
            if (!in) break;

            std::string payload(sz, '\0');
            in.read(&payload[0], sz);
            if (!in) break;

            if (entryStart >= 0) {
                cacheIndexTermAndOffset(idx, term, static_cast<uint64_t>(entryStart));
            }

            if (idx < fromIndex) continue;
            if (idx > toIndex) break;

            // AppendEntries assigns indexes relative to prevLogIndex. Returning
            // index N+K for a range beginning at N+1 would relabel the payload as
            // N+1 and permanently skip committed entries.
            if (idx != expectedIndex) {
                entries.clear();
                if (entryTerms) entryTerms->clear();
                return entries;
            }

            try {
                entries.push_back(decodeRaftPayloadOrThrow(payload));
                if (entryTerms) entryTerms->push_back(term);
                ++expectedIndex;
            } catch (...) {
                std::cerr << "[RAFTCORE] Failed to parse raft entry during catch-up index="
                          << idx << std::endl;
                break;
            }
        }

        if (expectedIndex != toIndex + 1) {
            entries.clear();
            if (entryTerms) entryTerms->clear();
        }
    } catch (...) {}

    return entries;
}

bool RaftCore::sendSnapshotToPeer(const std::string& peer,
                                  uint64_t term,
                                  int timeoutMs,
                                  std::string& response) {
    std::string base = dataRoot();
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";
    const std::string snapshotPath = base + ".snapshot";
    std::string checksum;
    if (!readTrustedSnapshotChecksum(snapshotPath, checksum)) return false;

    std::error_code sizeEc;
    const uint64_t fileBytes = std::filesystem::file_size(snapshotPath, sizeEc);
    if (sizeEc || fileBytes == 0 || lastSnapshotIndex_ == 0) return false;
    const size_t chunkBytes = raftSnapshotChunkBytes();
    const size_t totalChunks = static_cast<size_t>((fileBytes + chunkBytes - 1) / chunkBytes);
    uint64_t transferId = steadyNowMs() ^ fileBytes ^ (lastSnapshotIndex_ << 1U);
    if (transferId == 0) transferId = 1;

    std::ifstream input(snapshotPath, std::ios::binary);
    if (!input) return false;
    std::vector<char> buffer(chunkBytes);
    const int transferTimeoutMs = std::max(
        timeoutMs, static_cast<int>(raftEnvIntMs("RAFT_SNAPSHOT_TRANSFER_TIMEOUT_MS", 30000)));
    for (size_t chunkIndex = 0; chunkIndex < totalChunks; ++chunkIndex) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes = input.gcount();
        if (bytes <= 0) return false;
        json chunk;
        chunk["type"] = "install_snapshot_chunk";
        chunk["term"] = term;
        chunk["leader"] = nodeId_;
        chunk["transferId"] = transferId;
        chunk["chunkIndex"] = chunkIndex;
        chunk["totalChunks"] = totalChunks;
        chunk["lastIncludedIndex"] = lastSnapshotIndex_;
        chunk["lastIncludedTerm"] = lastSnapshotTerm_;
        chunk["payloadSha256"] = checksum;
        chunk["chunkData"] = std::string(buffer.data(), static_cast<size_t>(bytes));
        bool ack = false;
        response.clear();
        if (!sendToPeer(peer, chunk, transferTimeoutMs, ack, &response) ||
            response.find("chunk_ok") == std::string::npos) {
            return false;
        }
    }
    if (input.bad()) return false;

    json complete;
    complete["type"] = "install_snapshot_complete";
    complete["term"] = term;
    complete["leader"] = nodeId_;
    complete["transferId"] = transferId;
    complete["lastIncludedIndex"] = lastSnapshotIndex_;
    complete["lastIncludedTerm"] = lastSnapshotTerm_;
    complete["payloadSha256"] = checksum;
    bool ack = false;
    response.clear();
    return sendToPeer(peer, complete, transferTimeoutMs, ack, &response) &&
        response.find("installed") != std::string::npos;
}

bool RaftCore::backfillCommittedEntriesToPeer(const std::string& peer,
                                             uint64_t peerMatchIndex,
                                             uint64_t leaderCommitIndex,
                                             uint64_t term,
                                             int timeoutMs) {
    // Heartbeats and the foreground replicator can discover the same lagging
    // follower concurrently. Serialize the expensive recovery path per peer;
    // otherwise both paths can hold and transmit a multi-GB snapshot at once.
    auto peerLock = raftPeerMutex(peer);
    std::lock_guard<std::mutex> peerGuard(*peerLock);
    {
        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
        peerMatchIndex = std::max(peerMatchIndex, matchIndex_[peer]);
    }
    if (peerMatchIndex >= leaderCommitIndex) return true;

    uint64_t prevIndex = peerMatchIndex;
    static const uint64_t maxBatch = []() -> uint64_t {
        const char* e = std::getenv("RAFT_CATCHUP_MAX_ENTRIES");
        if (!e) return 512;
        try { return std::max<uint64_t>(1, std::stoull(e)); } catch (...) { return 512; }
    }();

    for (int attempt = 0; attempt < 16 && prevIndex < leaderCommitIndex; ++attempt) {
        uint64_t from = prevIndex + 1;
        uint64_t to = std::min<uint64_t>(leaderCommitIndex, prevIndex + maxBatch);
        std::vector<uint64_t> entryTerms;
        auto entries = readRaftEntriesRange(from, to, &entryTerms);
        // A non-empty read is not proof that the requested range is contiguous:
        // after compaction it can begin at snapshot+1 while `from` is still below
        // the snapshot boundary. Install the snapshot before replaying its suffix.
        if (lastSnapshotIndex_ > 0 && prevIndex < lastSnapshotIndex_) {
            entries.clear();
            entryTerms.clear();
        }
        if (entries.empty()) {
            if (lastSnapshotIndex_ > 0 && prevIndex < lastSnapshotIndex_) {
                // v5.5P-R4.9: heartbeat-triggered catch-up *is* the normal
                // replication path for a restarted follower. Returning false here
                // left a follower permanently parked behind a compacted prefix
                // (leader had snapshot@N, follower had log<N, no new foreground
                // write necessarily arrived to exercise the batch snapshot branch).
                // Send the latest snapshot directly, then continue with any suffix
                // after lastSnapshotIndex_.
                try {
                    std::string snapResp;
                    const bool snapOk = sendSnapshotToPeer(peer, term, timeoutMs, snapResp);
                    uint64_t remoteTerm = 0;
                    if (parseStaleTermResponse(snapResp, remoteTerm)) {
                        observeHigherTerm(remoteTerm);
                        return false;
                    }
                    if (!snapOk) {
                        RAFT_DLOG("[RAFTCORE] snapshot backfill failed peer=" << peer
                                  << " resp=" << snapResp.substr(0, 160) << std::endl);
                        return false;
                    }

                    const uint64_t included = lastSnapshotIndex_;
                    {
                        std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                        matchIndex_[peer] = std::max(matchIndex_[peer], included);
                        nextIndex_[peer] = std::max(nextIndex_[peer], included + 1);
                    }
                    {
                        std::lock_guard<std::mutex> hbLk(peerHeartbeatMutex_);
                        peerLastHeartbeatMs_[peer] = steadyNowMs();
                    }
                    prevIndex = std::max(prevIndex, included);
                    if (prevIndex >= leaderCommitIndex) return true;
                    continue;
                } catch (const std::exception& e) {
                    RAFT_DLOG("[RAFTCORE] snapshot backfill exception peer=" << peer
                              << " error=" << e.what() << std::endl);
                    return false;
                } catch (...) {
                    RAFT_DLOG("[RAFTCORE] snapshot backfill unknown exception peer=" << peer << std::endl);
                    return false;
                }
            }
            return false;
        }

        json msg;
        msg["type"] = "appendEntries";
        msg["rpcFormat"] = "json";
        msg["term"] = term;
        msg["leaderId"] = nodeId_;
        msg["leader"] = nodeId_;
        msg["prevLogIndex"] = prevIndex;
        msg["prevLogTerm"] = getTermForIndex(prevIndex);
        msg["leaderCommit"] = leaderCommitIndex;
        msg["entries"] = entries;
        msg["entryTerms"] = entryTerms;

        bool ack = false;
        std::string resp;
        bool ok = sendToPeer(peer, msg, timeoutMs, ack, &resp);
        uint64_t remoteTerm = 0;
        if (parseStaleTermResponse(resp, remoteTerm)) {
            observeHigherTerm(remoteTerm);
            return false;
        }

        if (ok && ack) {
            uint64_t remoteMatch = to;
            try {
                auto jr = json::parse(resp);
                remoteMatch = jr.value("matchIndex", remoteMatch);
            } catch (...) {}
            if (remoteMatch <= prevIndex) remoteMatch = to;

            {
                std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                matchIndex_[peer] = std::max(matchIndex_[peer], remoteMatch);
                nextIndex_[peer] = std::max(nextIndex_[peer], remoteMatch + 1);
            }
            {
                std::lock_guard<std::mutex> hbLk(peerHeartbeatMutex_);
                peerLastHeartbeatMs_[peer] = steadyNowMs();
            }
            prevIndex = remoteMatch;
            continue;
        }

        bool adjusted = false;
        try {
            auto jr = json::parse(resp);
            uint64_t remoteMatch = jr.value("matchIndex", prevIndex);
            std::string reason = jr.value("reason", "");
            if (reason.find("conflict") != std::string::npos) {
                prevIndex = remoteMatch > 0 ? remoteMatch : (prevIndex > 0 ? prevIndex - 1 : 0);
                adjusted = true;
            }
        } catch (...) {
            if (resp.rfind("conflict:", 0) == 0) {
                try {
                    uint64_t remoteMatch = std::stoull(resp.substr(9));
                    prevIndex = remoteMatch > 0 ? remoteMatch : (prevIndex > 0 ? prevIndex - 1 : 0);
                    adjusted = true;
                } catch (...) {}
            }
        }

        if (!adjusted) {
            if (prevIndex > 0) --prevIndex;
            else return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return prevIndex >= leaderCommitIndex;
}

bool RaftCore::createSnapshot(uint64_t lastIncludedIndex,
                              const std::filesystem::path& pinnedRoot) {
    try {
        if (lastIncludedIndex == 0) return false;
        pacificdb::test::hitFailpoint("FP_SHUTDOWN_DURING_SNAPSHOT", 1);
        auto snapshotStart = std::chrono::steady_clock::now();
        uint64_t lastIncludedTerm = getTermForIndex(lastIncludedIndex);
        json snap;
        snap["version"] = 1;
        snap["lastIncludedIndex"] = lastIncludedIndex;
        snap["lastIncludedTerm"] = lastIncludedTerm;
        snap["currentTerm"] = currentTerm_;
        snap["commitIndex"] = lastIncludedIndex;
        const auto snapshotNow = std::chrono::system_clock::now();
        snap["createdAt"] = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
            snapshotNow.time_since_epoch()).count();
        snap["createdAtMs"] = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            snapshotNow.time_since_epoch()).count();
        snap["complete"] = true;

        std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string snapshotPath = base + ".snapshot";
        std::string tmpSnapshotPath = snapshotPath + ".tmp";

        // A Raft snapshot is unusable without the exact state-machine image for
        // its advertised boundary. Export failures must abort snapshot creation;
        // a metadata-only snapshot would make a follower skip committed entries.
        pacificdb::test::hitFailpoint(
            "FP_SNAPSHOT_AFTER_PIN", lastIncludedIndex);
        std::vector<std::filesystem::path> artifactPaths;
        snap["lsm_payload"] = LSM::exportSnapshotBundleManifest(
            lastIncludedIndex, pinnedRoot, artifactPaths);
        snap["snapshot_format"] = "pdb-snapshot-bundle-v3";
        snap["snapshot_checksum_format"] = "bundle-v3";
        std::vector<pacificdb::snapshot_bundle::ArtifactSource> artifacts;
        artifacts.reserve(artifactPaths.size());
        for (const auto& path : artifactPaths) artifacts.push_back({path});
        pacificdb::snapshot_bundle::write(tmpSnapshotPath, snap, artifacts);
        snap = pacificdb::snapshot_bundle::read(tmpSnapshotPath).manifest;

        std::string validationReason;
        if (!validateRaftSnapshot(snap, validationReason)) {
            std::filesystem::remove(tmpSnapshotPath);
            std::cerr << "[RAFTCORE] Refusing invalid snapshot: " << validationReason << std::endl;
            return false;
        }

        // The durable snapshot must reach disk before compactRaftLog is allowed
        // to remove entries represented by it.
        durableAtomicReplaceFile(tmpSnapshotPath, snapshotPath, "Raft snapshot");
        const std::string fileChecksum =
            pacificdb::durability::ChecksumCalculator::sha256File(snapshotPath);
        persistRaftSnapshotChecksum(snapshotPath, fileChecksum);

        lastSnapshotIndex_ = lastIncludedIndex;
        lastSnapshotTerm_ = lastIncludedTerm;
        lastSnapshotTime_ = std::chrono::steady_clock::now();
        const auto snapshotMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - snapshotStart).count();
        MetricsExporter::recordCustomMetric("pacificdb_snapshot_create_duration_ms", static_cast<double>(snapshotMs));
        MetricsExporter::incrementCounter("pacificdb_snapshot_create_total", 1.0);
        MetricsExporter::recordCustomMetric("pacificdb_snapshot_last_index", static_cast<double>(lastIncludedIndex));
        std::cout << "[RAFTCORE] Snapshot created at index " << lastIncludedIndex << " term=" << lastIncludedTerm << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[RAFTCORE] Snapshot creation failed: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[RAFTCORE] Snapshot creation failed: unknown error" << std::endl;
        return false;
    }
}

bool RaftCore::compactRaftLog(uint64_t lastIncludedIndex) {
    try {
        // v5.5P-R2C: hold logMutex_ across the rewrite+rename so the physical removal of
        // committed entries is atomic w.r.t. log appends (phase 1 of replicateSynchronous)
        // and the in-order applier. Callers hold at most applyMutex_ (lock order
        // applyMutex_ > logMutex_, matching applyCommittedEntries), never logMutex_, so
        // this cannot self-deadlock.
        std::lock_guard<std::mutex> lk(logMutex_);
        std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";
        std::string tmpPath = base + "raft/log.bin.tmp";

        std::ifstream in(logPath, std::ios::binary);
        if (!in.is_open()) return false;
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) { in.close(); return false; }

        // copy entries with index > lastIncludedIndex
        while (!in.eof()) {
            uint64_t idx = 0; uint64_t term = 0; uint32_t sz = 0;
            in.read(reinterpret_cast<char*>(&idx), sizeof(idx));
            if (!in) break;
            in.read(reinterpret_cast<char*>(&term), sizeof(term));
            in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
            if (!in) break;
            std::string payload(sz, '\0');
            in.read(&payload[0], sz);
            if (!in) break;
            if (idx > lastIncludedIndex) {
                out.write(reinterpret_cast<char*>(&idx), sizeof(idx));
                out.write(reinterpret_cast<char*>(&term), sizeof(term));
                out.write(reinterpret_cast<char*>(&sz), sizeof(sz));
                out.write(payload.data(), sz);
            }
        }
        in.close(); out.flush(); out.close();

        // replace original log
        resetRaftLog(logPath);
        std::filesystem::rename(tmpPath, logPath);
        // log.bin was rewritten by compaction: index->term mappings may no longer hold.
        invalidateTermCache();

        // Rebuild JSONL raft_log.jsonl from remaining binary entries
        std::string jsonlPath = base + "raft/raft_log.jsonl";
        std::ofstream jsonlOut(jsonlPath, std::ios::trunc);
        if (jsonlOut.is_open()) {
            std::ifstream in2(logPath, std::ios::binary);
            while (!in2.eof()) {
                uint64_t idx = 0; uint64_t term = 0; uint32_t sz = 0;
                in2.read(reinterpret_cast<char*>(&idx), sizeof(idx));
                if (!in2) break;
                in2.read(reinterpret_cast<char*>(&term), sizeof(term));
                in2.read(reinterpret_cast<char*>(&sz), sizeof(sz));
                if (!in2) break;
                std::string payload(sz, '\0');
                in2.read(&payload[0], sz);
                if (!in2) break;
                // write payload as JSON line
                jsonlOut << payload << "\n";
            }
            in2.close(); jsonlOut.flush(); jsonlOut.close();
        }

        std::cout << "[RAFTCORE] Compacted raft log up to index " << lastIncludedIndex << std::endl;
        return true;
    } catch (...) {
        return false;
    }
}

bool RaftCore::trimRaftLog(uint64_t maxIndex) {
    try {
        std::string base = dataRoot();
        if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";
        std::string tmpPath = base + "raft/log.bin.trim.tmp";

        std::lock_guard<std::mutex> lk(logMutex_);

        if (!std::filesystem::exists(logPath)) return true;
        std::ifstream in(logPath, std::ios::binary);
        if (!in.is_open()) return false;
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) { in.close(); return false; }

        while (!in.eof()) {
            uint64_t idx = 0; uint64_t term = 0; uint32_t sz = 0;
            in.read(reinterpret_cast<char*>(&idx), sizeof(idx));
            if (!in) break;
            in.read(reinterpret_cast<char*>(&term), sizeof(term));
            in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
            if (!in) break;
            std::string payload(sz, '\0');
            in.read(&payload[0], sz);
            if (!in) break;

            if (idx > maxIndex) {
                break;
            }

            out.write(reinterpret_cast<char*>(&idx), sizeof(idx));
            out.write(reinterpret_cast<char*>(&term), sizeof(term));
            out.write(reinterpret_cast<char*>(&sz), sizeof(sz));
            out.write(payload.data(), sz);
        }

        in.close();
        out.flush();
        out.close();

        std::error_code ec;
        resetRaftLog(logPath);
        std::filesystem::rename(tmpPath, logPath, ec);
        if (ec) {
            std::cerr << "[RAFTCORE] Failed to trim log: " << ec.message() << std::endl;
            return false;
        }
        // log.bin was rewritten by trim: index->term mappings may no longer hold.
        invalidateTermCache();

        return true;
    } catch (...) {
        return false;
    }
}

void RaftCore::runListener() {
    std::cout << "[RAFTCORE] Listener starting on port " << listenPort_ << std::endl;
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { std::cerr << "[RAFTCORE] Listener socket failed" << std::endl; return; }
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listenPort_);
    std::string raftBindHost = "0.0.0.0";
    if (const char* bindEnv = std::getenv("RAFT_BIND_HOST")) {
        if (*bindEnv) raftBindHost = bindEnv;
    }
    if (raftBindHost == "0.0.0.0" || raftBindHost == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, raftBindHost.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[RAFTCORE] Listener invalid RAFT_BIND_HOST=" << raftBindHost
                  << "; falling back to 0.0.0.0" << std::endl;
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[RAFTCORE] Listener bind failed on " << raftBindHost << ":" << listenPort_
                  << " errno=" << errno << std::endl;
#ifdef _WIN32
        closesocket(server);
#else
        close(server);
#endif
        raftListenerRunning_.store(false);
        return;
    }
    if (listen(server, 1024) < 0) {
        std::cerr << "[RAFTCORE] Listener listen failed on port " << listenPort_ << " errno=" << errno << std::endl;
#ifdef _WIN32
        closesocket(server);
#else
        close(server);
#endif
        raftListenerRunning_.store(false);
        return;
    }
    raftListenerSocket_.store(
        static_cast<std::intptr_t>(server), std::memory_order_release);
    raftListenerRunning_.store(true);
    std::cout << "[RAFTCORE] Listener ready on " << raftBindHost << ":" << listenPort_ << std::endl;

    // Reusable inbound RPC workers. Previously every accepted heartbeat/append/vote
    // connection created and detached a new std::thread. A short V10 syscall profile
    // observed ~950 clone3 calls even though RF3 has only two peers. Keep the same
    // bounded concurrency, but queue sockets onto a fixed worker set.
    int inboundWorkerCount = std::min(32, raftMaxWorkers_);
    if (const char* workers = std::getenv("RAFT_INBOUND_WORKERS")) {
        try { inboundWorkerCount = std::max(2, std::min(raftMaxWorkers_, std::stoi(workers))); } catch (...) {}
    }
    size_t inboundQueueMax = static_cast<size_t>(std::max(64, raftMaxWorkers_ * 64));
    if (const char* queueMax = std::getenv("RAFT_INBOUND_QUEUE_MAX")) {
        try { inboundQueueMax = std::max<size_t>(16, std::stoull(queueMax)); } catch (...) {}
    }
    std::mutex inboundMutex;
    std::condition_variable inboundCv;
    std::deque<std::pair<int, std::chrono::steady_clock::time_point>> inboundQueue;
    std::atomic<size_t> pendingConnections{0};
    std::vector<std::thread> inboundWorkers;
    inboundWorkers.reserve(static_cast<size_t>(inboundWorkerCount));
    for (int i = 0; i < inboundWorkerCount; ++i) {
        inboundWorkers.emplace_back([this, &inboundMutex, &inboundCv, &inboundQueue, &pendingConnections]() {
            for (;;) {
                int client = -1;
                {
                    std::unique_lock<std::mutex> lk(inboundMutex);
                    inboundCv.wait(lk, [this, &inboundQueue]() {
                        return !running_.load(std::memory_order_acquire) || !inboundQueue.empty();
                    });
                    if (!running_.load(std::memory_order_acquire) && inboundQueue.empty()) return;
                    client = inboundQueue.front().first;
                    const auto queuedAt = inboundQueue.front().second;
                    inboundQueue.pop_front();
                    pendingConnections.fetch_sub(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
                    raftRecordMetric(g_inboundRaftQueueWaitUs,
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - queuedAt).count());
                }
                inboundCv.notify_all();
                {
                    std::lock_guard<std::mutex> wlk(raftWorkerMutex_);
                    ++raftActiveWorkers_;
                }
                try {
                    handleFollowerConn(client, pendingConnections); // closes socket via RAII guard
                } catch (...) {
                    std::cerr << "[RAFTCORE] Follower connection handler exception" << std::endl;
                }
                {
                    std::lock_guard<std::mutex> wlk(raftWorkerMutex_);
                    --raftActiveWorkers_;
                }
                raftWorkerCv_.notify_all();
            }
        });
    }
    std::cout << "[RAFTCORE] Inbound RPC pool workers=" << inboundWorkerCount
              << " queue=" << inboundQueueMax << std::endl;

    while (running_.load()) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int len = sizeof(clientAddr);
#else
        socklen_t len = sizeof(clientAddr);
#endif
        int client = accept(server, (sockaddr*)&clientAddr, &len);
        if (client < 0) {
            if (running_.load()) {
                int e = errno;
                raftAcceptErrors_.fetch_add(1, std::memory_order_relaxed);
                // v5.5P-R2B: never exit on transient accept errors. For EMFILE/ENFILE
                // (fd exhaustion) back off longer so in-flight handlers can release fds.
                long backoffMs = 25;
                if (e == EMFILE || e == ENFILE) {
                    raftEmfileErrors_.fetch_add(1, std::memory_order_relaxed);
                    static const long acceptBackoffMs = [] {
                        if (const char* value = std::getenv("RAFT_ACCEPT_BACKOFF_MS")) {
                            try { return static_cast<long>(std::max(50, std::stoi(value))); } catch (...) {}
                        }
                        return 200L;
                    }();
                    backoffMs = acceptBackoffMs;
                    static thread_local uint64_t lastLog = 0;
                    uint64_t now = steadyNowMs();
                    if (now - lastLog > 1000) { // rate-limit the log
                        std::cerr << "[RAFTCORE] accept EMFILE/ENFILE on " << listenPort_ << " (open=" << raftOpenConnections_.load() << ") backoff " << backoffMs << "ms" << std::endl;
                        lastLog = now;
                    }
                } else if (e == EINTR || e == EAGAIN || e == EWOULDBLOCK) {
                    backoffMs = 1;
                } else {
                    std::cerr << "[RAFTCORE] Listener accept failed on port " << listenPort_
                              << " errno=" << e << " (" << std::strerror(e) << ")" << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            }
            continue;
        }
        // v5.5P-R3: disable Nagle on the accepted socket so the follower's framed
        // response is not held back by delayed ACK (pairs with the client-side fix).
        if (raftTcpNoDelayEnabled()) {
#ifndef _WIN32
            int one = 1; setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#else
            int one = 1; setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
        }
        // Queue onto the reusable inbound worker set. Backpressure the accept loop at a
        // bounded queue depth instead of allocating another thread per connection.
        {
            std::unique_lock<std::mutex> qlk(inboundMutex);
            inboundCv.wait(qlk, [this, &inboundQueue, inboundQueueMax]() {
                return !running_.load(std::memory_order_acquire) || inboundQueue.size() < inboundQueueMax;
            });
            if (!running_.load()) {
#ifdef _WIN32
                closesocket(client);
#else
                close(client);
#endif
                break;
            }
            inboundQueue.push_back({client, std::chrono::steady_clock::now()});
            pendingConnections.fetch_add(1, std::memory_order_relaxed);
        }
        inboundCv.notify_one();
        char addrbuf[64]; inet_ntop(AF_INET, &clientAddr.sin_addr, addrbuf, sizeof(addrbuf));
        RAFT_DLOG("[RAFTCORE] Accepted connection from " << addrbuf << ":" << ntohs(clientAddr.sin_port) << std::endl);
    }

    raftListenerRunning_.store(false);
    inboundCv.notify_all();
    for (auto& worker : inboundWorkers) {
        if (worker.joinable()) worker.join();
    }
    std::cout << "[RAFTCORE] Listener exiting on port " << listenPort_ << std::endl;
#ifdef _WIN32
    std::intptr_t expectedSocket = static_cast<std::intptr_t>(server);
    if (raftListenerSocket_.compare_exchange_strong(expectedSocket, -1)) {
        closesocket(server);
    }
#else
    std::intptr_t expectedSocket = static_cast<std::intptr_t>(server);
    if (raftListenerSocket_.compare_exchange_strong(expectedSocket, -1)) {
        close(server);
    }
#endif
}

void RaftCore::loadPersistedState() {
    try {
        // V11.4-DIV-001 P0.6 regression hook (test builds only; production stub
        // returns false): force the restore-failure path so the fenced-node
        // contract (no votes, no candidacy, no appendEntries, no strong reads)
        // is provable against the real engine.
        if (pacificdb::test::injectApplyFailure("FP_RECOVERY_STATE_LOAD_FAILURE", 1)) {
            throw std::runtime_error("test-injected recovery state load failure");
        }
        std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
        const std::string consensusFile = base + "raft/consensus_state.json";
        std::ifstream consensusIn(consensusFile);
        if (consensusIn.is_open()) {
            json state = json::parse(consensusIn, nullptr, false);
            consensusIn.close();
            if (!state.is_discarded() && state.is_object()) {
                const std::string storedChecksum = state.value("checksum", std::string());
                json checksumPayload = state;
                checksumPayload.erase("checksum");
                const std::string encoded = checksumPayload.dump();
                const std::string actualChecksum =
                    pacificdb::durability::ChecksumCalculator::sha256(encoded.data(), encoded.size());
                const uint64_t version = state.value("version", static_cast<uint64_t>(0));
                const uint64_t commit = state.value("commitIndex", static_cast<uint64_t>(0));
                const uint64_t applied = state.value("lastApplied", static_cast<uint64_t>(0));
                if (version == 1 && !storedChecksum.empty() && storedChecksum == actualChecksum && applied <= commit) {
                    currentTerm_ = state.value("currentTerm", static_cast<uint64_t>(0));
                    votedFor_ = state.value("votedFor", std::string());
                    commitIndex_.store(commit, std::memory_order_release);
                    lastApplied_.store(applied, std::memory_order_release);
                    persistedConsensusTerm_ = currentTerm_;
                    persistedVotedFor_ = votedFor_;
                    persistedCommitIndex_ = commit;
                    persistedLastApplied_ = applied;
                    consensusStateLoaded_ = true;
                } else {
                    std::cerr << "[RAFTCORE] Ignoring invalid consensus state checksum/invariants" << std::endl;
                }
            }
        }

        // Legacy term/vote files are migration inputs only. New writes persist
        // the complete consensus record atomically so term, vote and progress
        // cannot be observed from different crash generations.
        std::string termFile = base + "raft/current_term.txt";
        std::string votedFile = base + "raft/voted_for.txt";
        if (!consensusStateLoaded_) {
            std::ifstream in(termFile);
            if (in.is_open()) {
                uint64_t t; in >> t; currentTerm_ = t; in.close();
            }
            std::ifstream iv(votedFile);
            if (iv.is_open()) {
                std::string v; std::getline(iv, v); votedFor_ = v; iv.close();
            }
            persistedConsensusTerm_ = currentTerm_;
            persistedVotedFor_ = votedFor_;
        }
        std::cout << "[RAFTCORE] Loaded persisted state term=" << currentTerm_
                  << " commit=" << commitIndex_.load()
                  << " applied=" << lastApplied_.load()
                  << " source=" << (consensusStateLoaded_ ? "checksummed" : "legacy") << std::endl;

        // Load and replay Raft log entries
        replayRaftLog();
    } catch (const std::exception& e) {
        // V11.4-DIV-001 P0.6: a swallowed restore failure previously let the node
        // start networking with lastIndex_/lastEntryTerm_ still zero and grant
        // votes to stale-log candidates (the term-3183 committed-entry rollback).
        // Fail closed: the node stays fenced (recoveryComplete_=false blocks
        // votes, candidacy, appendEntries, strong reads and readiness).
        std::cerr << "[RAFTCORE] FATAL: persisted state load failed: " << e.what()
                  << " — node fenced (no votes, no leadership, no strong reads)" << std::endl;
        recoveryComplete_.store(false);
        leader_.store(false);
        recordSafetyAudit("recovery_state_load_failed",
                          {{"error", e.what()}, {"nodeId", nodeId_}});
    } catch (...) {
        std::cerr << "[RAFTCORE] FATAL: persisted state load failed: unknown error"
                  << " — node fenced (no votes, no leadership, no strong reads)" << std::endl;
        recoveryComplete_.store(false);
        leader_.store(false);
        recordSafetyAudit("recovery_state_load_failed",
                          {{"error", "unknown"}, {"nodeId", nodeId_}});
    }
}

// V11.4-DIV-001 P0.6: durable safety audit trail. One fsynced JSON line per
// safety-relevant refusal/fencing event. Best-effort by contract (never throws,
// never blocks the caller on failure) but durable when it succeeds.
void RaftCore::recordSafetyAudit(const std::string& event,
                                 const nlohmann::json& detail) noexcept {
    try {
        std::lock_guard<std::mutex> lk(safetyAuditMutex_);
        std::string base = dataRoot();
        if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";
        std::filesystem::create_directories(base + "raft");
        const std::string path = base + "raft/safety_audit.jsonl";
        nlohmann::json rec = {
            {"ts_ms", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())},
            {"event", event},
            {"nodeId", nodeId_},
            {"term", currentTerm_},
            {"commitIndex", commitIndex_.load(std::memory_order_acquire)},
            {"lastApplied", lastApplied_.load(std::memory_order_acquire)},
            {"detail", detail},
        };
        const std::string line = rec.dump() + "\n";
#ifdef _WIN32
        HANDLE fd = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fd != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(fd, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            FlushFileBuffers(fd);
            CloseHandle(fd);
        }
#else
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) {
            ssize_t off = 0;
            while (off < static_cast<ssize_t>(line.size())) {
                ssize_t w = ::write(fd, line.data() + off, line.size() - off);
                if (w <= 0) break;
                off += w;
            }
            ::fsync(fd);
            ::close(fd);
        }
#endif
        MetricsExporter::incrementCounter("pacificdb_safety_audit_events_total", 1.0);
    } catch (...) {}
}

void RaftCore::persistConsensusState(uint64_t term,
                                     const std::string& votedFor,
                                     uint64_t commitIndex,
                                     uint64_t lastApplied,
                                     bool updateElectionState) {
    if (lastApplied > commitIndex) {
        throw std::logic_error("raft progress invariant violated: lastApplied > commitIndex");
    }

    std::lock_guard<std::mutex> persistLock(consensusStatePersistMutex_);
    // Progress writers race with term/vote writers. Preserve the newest
    // election state and monotonically merge commit/apply progress so a late
    // stale snapshot can never move the durable record backwards.
    if (updateElectionState && term >= persistedConsensusTerm_) {
        persistedConsensusTerm_ = term;
        persistedVotedFor_ = votedFor;
    }
    persistedCommitIndex_ = std::max(persistedCommitIndex_, commitIndex);
    persistedLastApplied_ = std::max(persistedLastApplied_, lastApplied);
    if (persistedLastApplied_ > persistedCommitIndex_) {
        throw std::logic_error("durable raft progress invariant violated: lastApplied > commitIndex");
    }

    std::string base = dataRoot();
    if (base.back() != '/' && base.back() != '\\') base += "/";
    const std::string raftDir = base + "raft";
    const std::string statePath = raftDir + "/consensus_state.json";
    const std::string tempPath = statePath + ".tmp";
    std::filesystem::create_directories(raftDir);

    json state = {
        {"version", 1},
        {"currentTerm", persistedConsensusTerm_},
        {"votedFor", persistedVotedFor_},
        {"commitIndex", persistedCommitIndex_},
        {"lastApplied", persistedLastApplied_}
    };
    const std::string checksumInput = state.dump();
    state["checksum"] = pacificdb::durability::ChecksumCalculator::sha256(
        checksumInput.data(), checksumInput.size());
    const std::string encoded = state.dump(2) + "\n";

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) throw std::runtime_error("cannot open raft consensus state temp file");
        out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        out.flush();
        if (!out.good()) throw std::runtime_error("cannot flush raft consensus state temp file");
    }

#ifdef _WIN32
    HANDLE tempHandle = CreateFileA(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (tempHandle == INVALID_HANDLE_VALUE || !FlushFileBuffers(tempHandle)) {
        if (tempHandle != INVALID_HANDLE_VALUE) CloseHandle(tempHandle);
        throw std::runtime_error("cannot fsync raft consensus state temp file");
    }
    CloseHandle(tempHandle);
    std::error_code removeEc;
    std::filesystem::remove(statePath, removeEc);
#else
    int tempFd = open(tempPath.c_str(), O_WRONLY);
    if (tempFd < 0 || fsync(tempFd) != 0) {
        if (tempFd >= 0) close(tempFd);
        throw std::runtime_error("cannot fsync raft consensus state temp file");
    }
    close(tempFd);
#endif

    std::error_code renameEc;
    std::filesystem::rename(tempPath, statePath, renameEc);
    if (renameEc) {
        std::filesystem::remove(tempPath);
        throw std::runtime_error("cannot atomically replace raft consensus state: " + renameEc.message());
    }
#ifndef _WIN32
    int dirFd = open(raftDir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirFd >= 0) {
        if (fsync(dirFd) != 0) {
            close(dirFd);
            throw std::runtime_error("cannot fsync raft consensus state directory");
        }
        close(dirFd);
    }
#endif
    consensusStateLoaded_ = true;
}

void RaftCore::persistProgress() {
    uint64_t term = 0;
    std::string votedFor;
    {
        std::lock_guard<std::mutex> lock(electionMutex_);
        term = currentTerm_;
        votedFor = votedFor_;
    }
    persistConsensusState(term, votedFor,
        commitIndex_.load(std::memory_order_acquire),
        lastApplied_.load(std::memory_order_acquire), false);
}

void RaftCore::persistAppliedProgress(uint64_t appliedIndex) {
    const uint64_t committed = commitIndex_.load(std::memory_order_acquire);
    if (appliedIndex > committed) {
        throw std::logic_error("cannot persist applied progress beyond commit index");
    }
    uint64_t term = 0;
    std::string votedFor;
    {
        std::lock_guard<std::mutex> lock(electionMutex_);
        term = currentTerm_;
        votedFor = votedFor_;
    }
    persistConsensusState(term, votedFor, committed, appliedIndex, false);
}

void RaftCore::persistCurrentTerm() {
    try {
        persistConsensusState(currentTerm_, votedFor_, commitIndex_.load(), lastApplied_.load(), true);
    } catch (const std::exception& e) {
        std::cerr << "[RAFTCORE] Failed to persist current term: " << e.what() << std::endl;
        throw;
    }
}

void RaftCore::adoptLeaderTermLocked(uint64_t term) {
    if (term < currentTerm_) return;
    leader_.store(false);
    if (term > currentTerm_) {
        currentTerm_ = term;
        votedFor_.clear();
        persistCurrentTerm();
    }
}

void RaftCore::persistVotedFor() {
    try {
        persistConsensusState(currentTerm_, votedFor_, commitIndex_.load(), lastApplied_.load(), true);
    } catch (const std::exception& e) {
        std::cerr << "[RAFTCORE] Failed to persist vote: " << e.what() << std::endl;
        throw;
    }
}

std::vector<json> RaftCore::loadRaftLog() {
    std::vector<json> entries;
    try {
        std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logFile = base + "raft/raft_log.jsonl";

        std::ifstream in(logFile);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty()) {
                    try {
                        auto entry = json::parse(line);
                        entries.push_back(entry);
                    } catch (...) {}
                }
            }
            in.close();
            std::cout << "[RAFTCORE] Loaded " << entries.size() << " Raft log entries from disk" << std::endl;
        }
    } catch (...) {}
    return entries;
}

static std::vector<PersistedRaftEntry> loadBinaryRaftLogEntries(const std::string& baseRoot) {
    std::vector<PersistedRaftEntry> entries;
    std::string base = baseRoot;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";
    const std::string logPath = base + "raft/log.bin";
    std::ifstream in(logPath, std::ios::binary);
    if (!in.is_open()) return entries;

    while (in.peek() != std::char_traits<char>::eof()) {
        const std::streamoff entryStart = in.tellg();
        uint64_t idx = 0;
        uint64_t term = 0;
        uint32_t sz = 0;
        if (!in.read(reinterpret_cast<char*>(&idx), sizeof(idx)) ||
            !in.read(reinterpret_cast<char*>(&term), sizeof(term)) ||
            !in.read(reinterpret_cast<char*>(&sz), sizeof(sz))) {
            throw std::runtime_error("truncated raft/log.bin record header");
        }
        if (sz == 0 || sz > kMaxRaftPayloadBytes) {
            throw std::runtime_error("invalid raft/log.bin payload size at index " +
                                     std::to_string(idx));
        }
        std::string payload(sz, '\0');
        if (!in.read(payload.data(), sz)) {
            throw std::runtime_error("truncated raft/log.bin payload at index " +
                                     std::to_string(idx));
        }
        const std::streamoff entryEnd = in.tellg();
        if (entryEnd < 0) throw std::runtime_error("invalid raft/log.bin offset");
        entries.push_back(PersistedRaftEntry{
            idx,
            term,
            decodeRaftPayloadOrThrow(payload),
            static_cast<uint64_t>(entryStart),
            static_cast<uint64_t>(entryEnd)
        });
    }
    return entries;
}

void RaftCore::replayRaftLog() {
    recoveryComplete_.store(false);
    try {
        uint64_t snapshotIndex = 0;
        uint64_t snapshotTerm = 0;
        try {
            std::string base = dataRoot();
            if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";
            const std::string snapshotPath = base + ".snapshot";
            if (std::filesystem::is_regular_file(snapshotPath)) {
                std::string metadataError;
                if (!readPersistedSnapshotMetadata(
                        snapshotPath, snapshotIndex, snapshotTerm, metadataError)) {
                    throw std::runtime_error(metadataError);
                }
                if (snapshotIndex > 0) {
                    if (snapshotIndex >= lastSnapshotIndex_) lastSnapshotTerm_ = snapshotTerm;
                    lastSnapshotIndex_ = std::max(lastSnapshotIndex_, snapshotIndex);
                    lastIndex_ = std::max(lastIndex_, snapshotIndex);
                    lastEntryTerm_ = std::max(lastEntryTerm_, snapshotTerm);
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        commitIndex_ = std::max<uint64_t>(commitIndex_.load(), snapshotIndex);
                    }
                    lastApplied_.store(std::max(lastApplied_.load(), snapshotIndex));
                    std::cout << "[RAFTCORE] Restored raft snapshot metadata index="
                              << snapshotIndex << " term=" << snapshotTerm << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[RAFTCORE] Snapshot metadata restore failed: " << e.what() << std::endl;
            throw;
        } catch (...) {
            std::cerr << "[RAFTCORE] Snapshot metadata restore failed: unknown error" << std::endl;
            throw;
        }

        std::vector<PersistedRaftEntry> entries = loadBinaryRaftLogEntries(dataRoot());
        bool usedBinaryLog = !entries.empty();
        if (!usedBinaryLog) {
            auto legacyEntries = loadRaftLog();
            uint64_t syntheticIndex = 0;
            entries.reserve(legacyEntries.size());
            for (const auto& e : legacyEntries) {
                uint64_t idx = 0;
                uint64_t term = currentTerm_;
                try {
                    if (e.contains("_raft_commit_index") &&
                        (e["_raft_commit_index"].is_number_integer() || e["_raft_commit_index"].is_number_unsigned())) {
                        idx = e["_raft_commit_index"].get<uint64_t>();
                    }
                    if (e.contains("_raft_term") &&
                        (e["_raft_term"].is_number_integer() || e["_raft_term"].is_number_unsigned())) {
                        term = e["_raft_term"].get<uint64_t>();
                    }
                } catch (...) {}
                if (idx == 0) idx = ++syntheticIndex;
                syntheticIndex = std::max(syntheticIndex, idx);
                entries.push_back(PersistedRaftEntry{idx, term, e, 0, 0});
            }
        }

        if (entries.empty()) {
            const uint64_t durableCommit = commitIndex_.load(std::memory_order_acquire);
            if (durableCommit > snapshotIndex) {
                std::cerr << "[RAFTCORE] Recovery refused: durable commit index " << durableCommit
                          << " is not covered by raft/log.bin or a snapshot" << std::endl;
                recoveryComplete_.store(false);
                leader_.store(false);
                return;
            }
            if (snapshotIndex > 0) {
                commitIndex_.store(snapshotIndex, std::memory_order_release);
                lastApplied_.store(snapshotIndex, std::memory_order_release);
                persistProgress();
            }
            std::cout << "[RAFTCORE] No Raft log entries to replay" << std::endl;
            recoveryComplete_.store(true);
            return;
        }

        uint64_t maxIndex = 0;
        uint64_t lastLogTerm = 0;
        for (const auto& entry : entries) {
            if (entry.index >= maxIndex) {
                maxIndex = entry.index;
                lastLogTerm = entry.term;
            }
        }

        // Only a durable commit marker proves that a log entry was committed.
        // The log can legally contain an uncommitted tail after a leader crash;
        // replaying maxIndex as committed can expose a write that never reached
        // quorum. Existing pre-V11 data has no marker, so retain the old all-log
        // migration behavior once, then immediately write the checksummed state.
        const uint64_t availableLastIndex = std::max(maxIndex, snapshotIndex);
        const uint64_t durableCommitIndex =
            consensusStateLoaded_ ? commitIndex_.load(std::memory_order_acquire) : 0;
        if (consensusStateLoaded_ && durableCommitIndex > availableLastIndex) {
            throw std::runtime_error("durable commit index " +
                std::to_string(durableCommitIndex) +
                " is not covered by raft/log.bin or the installed snapshot (available " +
                std::to_string(availableLastIndex) + ")");
        }
        uint64_t restoredCommitIndex = consensusStateLoaded_
            ? durableCommitIndex
            : availableLastIndex;
        const uint64_t restoredLastLogTerm = maxIndex > snapshotIndex
            ? lastLogTerm : snapshotTerm;
        {
            std::lock_guard<std::mutex> el(electionMutex_);
            if (commitIndex_ < restoredCommitIndex) commitIndex_ = restoredCommitIndex;
        }
        lastIndex_ = std::max(lastIndex_, maxIndex);
        // Election term and log-entry term are different Raft state. Using the
        // larger currentTerm_ here lets a short-log node advertise a term it never
        // appended and win an election over a longer log after restart.
        lastEntryTerm_ = restoredLastLogTerm;

        std::cout << "[RAFTCORE] Replaying " << entries.size()
                  << " Raft log entries from " << (usedBinaryLog ? "raft/log.bin" : "raft_log.jsonl")
                  << " maxIndex=" << maxIndex
                  << " committedPrefix=" << restoredCommitIndex
                  << " recoveryMode=" << (consensusStateLoaded_ ? "checksummed" : "legacy_migration")
                  << std::endl;
        const uint64_t persistedApplied =
            lastApplied_.load(std::memory_order_acquire);
        const bool cleanAppliedPrefix =
            cleanShutdownRecovery_.load(std::memory_order_acquire)
            && consensusStateLoaded_
            && persistedApplied == restoredCommitIndex;
        uint64_t replayedApplied = cleanAppliedPrefix
            ? persistedApplied
            : snapshotIndex;
        uint64_t replayedOffset = 0;
        uint64_t expectedIndex = replayedApplied + 1;
        uint64_t reappliedEntries = 0;
        if (cleanAppliedPrefix) {
            std::cout << "[RAFTCORE] Clean shutdown: preserving checksummed applied prefix "
                      << replayedApplied << "; validating Raft log without reapplying it"
                      << std::endl;
        }
        for (const auto& persisted : entries) {
            if (persisted.index > restoredCommitIndex) continue;
            if (persisted.index <= replayedApplied) {
                if (usedBinaryLog) replayedOffset = persisted.endOffset;
                continue;
            }
            if (persisted.index < expectedIndex) continue;
            if (persisted.index != expectedIndex) {
                throw std::runtime_error("committed raft log gap at index " +
                    std::to_string(expectedIndex) + ", next available index " +
                    std::to_string(persisted.index));
            }
            try {
                // consensus_state.json proves Raft progress, not that every historical
                // state-machine file and index survived independently. Reapply every
                // committed entry after the installed snapshot. Skipping this prefix
                // previously hid missing collection data on a restarted follower.
                json entry = markEntryCommittedVisible(persisted.payload, persisted.index, persisted.term);
                if (!isRaftNoopEntry(entry)) {
                    // V11.4-DIV-001: this result was previously DISCARDED and the progress
                    // markers below advanced unconditionally. Node A stepped over committed
                    // entry 138216 (INSERT_MANY, 100 items) during recovery replay and
                    // reported lastApplied 139191 while holding none of those documents.
                    // A failed committed entry must stop replay here: no progress marker
                    // advances, no later entry is applied, recovery never completes.
                    const bool applied = DatabaseEngine::applyReplicatedEntry(entry);
                    if (!applied) {
                        // lastApplied_ was restored from consensus_state.json before replay
                        // began, so it still carries the durable value recorded by an
                        // earlier run. Leaving it there would report progress the state
                        // machine does not actually have — the same optimistic-marker
                        // failure as DIV-001 itself. Clamp it down to the last entry that
                        // genuinely applied. commitIndex legitimately stays ahead: these
                        // entries are committed, they are simply not applied.
                        lastApplied_.store(replayedApplied, std::memory_order_release);
                        const uint64_t failedTerm = persisted.term != 0
                            ? persisted.term
                            : entry.value("_raft_term", static_cast<uint64_t>(0));
                        blockApply(persisted.index, failedTerm,
                                   entry.value("op", entry.value("type", std::string("unknown"))),
                                   entry.value("logicalWriteId", std::string("")),
                                   "recovery replay: applyReplicatedEntry returned false",
                                   /*mutationMayHaveOccurred=*/true);
                        g_applyStorageFailures.fetch_add(1, std::memory_order_relaxed);
                        g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                        throw std::runtime_error(
                            "committed entry " + std::to_string(persisted.index) +
                            " failed to apply during recovery replay; refusing to advance"
                            " apply progress past an unapplied committed entry");
                    }
                }
                replayedApplied = persisted.index;
                if (usedBinaryLog) replayedOffset = persisted.endOffset;
                expectedIndex = persisted.index + 1;
                ++reappliedEntries;
            } catch (const std::exception& ex) {
                throw std::runtime_error("failed to replay committed entry " +
                    std::to_string(persisted.index) + ": " + ex.what());
            }
        }
        if (replayedApplied < restoredCommitIndex) {
            throw std::runtime_error("committed raft log ends before durable commit index " +
                std::to_string(restoredCommitIndex));
        }
        lastApplied_.store(replayedApplied, std::memory_order_release);
        trimRaftLog(maxIndex);
        if (usedBinaryLog) {
            for (const auto& persisted : entries) {
                cacheIndexTermAndOffset(
                    persisted.index, persisted.term, persisted.startOffset);
            }
        }
        // The binary log can contain a legal uncommitted tail. Resume scanning
        // after the last entry actually replayed, not at end-of-file, otherwise
        // a later commit can become permanently invisible to the apply worker.
        lastAppliedOffset_.store(
            usedBinaryLog ? replayedOffset : 0,
            std::memory_order_release);
        persistProgress();
        std::cout << "[RAFTCORE] ✅ Raft log replay complete"
                  << " reapplied=" << reappliedEntries << std::endl;
        recoveryComplete_.store(true);
    } catch (const std::exception& e) {
        std::cerr << "[RAFTCORE] Raft recovery failed: " << e.what() << std::endl;
        recoveryComplete_.store(false);
        leader_.store(false);
    } catch (...) {
        std::cerr << "[RAFTCORE] Raft recovery failed: unknown error" << std::endl;
        recoveryComplete_.store(false);
        leader_.store(false);
    }
}

const char* RaftCore::applySourceName(ApplySource source) {
    switch (source) {
        case ApplySource::WORKER: return "worker";
        case ApplySource::APPEND_ENTRIES: return "append_entries";
        case ApplySource::LEADER_BYPASS: return "leader_bypass";
        case ApplySource::LEADER_ASYNC: return "leader_async";
        case ApplySource::RECOVERY: return "recovery";
        case ApplySource::STARTUP: return "startup";
        case ApplySource::STRONG_READ: return "strong_read";
        case ApplySource::SNAPSHOT: return "snapshot";
    }
    return "unknown";
}

bool RaftCore::advanceLastApplied(uint64_t newIndex,
                                  ApplySource source,
                                  bool allowSnapshotJump) {
    const uint64_t oldIndex = lastApplied_.load(std::memory_order_acquire);
    const uint64_t committed = commitIndex_.load(std::memory_order_acquire);

    if (newIndex < oldIndex) {
        g_backwardLastAppliedAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[RAFTCORE][APPLY] refused backward lastApplied source="
                  << applySourceName(source) << " old=" << oldIndex
                  << " new=" << newIndex << std::endl;
        return false;
    }
    if (newIndex == oldIndex) {
        g_duplicateApplyAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!allowSnapshotJump && newIndex != oldIndex + 1) {
        g_outOfOrderApplyAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
        g_skippedApplyIndexTotal.fetch_add(
            static_cast<long long>(newIndex > oldIndex + 1 ? newIndex - oldIndex - 1 : 0),
            std::memory_order_relaxed);
        std::cerr << "[RAFTCORE][APPLY] refused non-contiguous advancement source="
                  << applySourceName(source) << " expected=" << (oldIndex + 1)
                  << " new=" << newIndex << std::endl;
        return false;
    }
    if (newIndex > committed) {
        g_visibilityAheadOfAppliedTotal.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[RAFTCORE][APPLY] refused lastApplied beyond commit source="
                  << applySourceName(source) << " commit=" << committed
                  << " new=" << newIndex << std::endl;
        return false;
    }

    lastApplied_.store(newIndex, std::memory_order_release);
    switch (source) {
        case ApplySource::WORKER:
            g_applySourceWorkerTotal.fetch_add(1, std::memory_order_relaxed); break;
        case ApplySource::APPEND_ENTRIES:
            g_applySourceAppendEntriesTotal.fetch_add(1, std::memory_order_relaxed); break;
        case ApplySource::LEADER_BYPASS:
            g_applySourceLeaderBypassTotal.fetch_add(1, std::memory_order_relaxed); break;
        case ApplySource::LEADER_ASYNC:
            g_applySourceLeaderAsyncTotal.fetch_add(1, std::memory_order_relaxed); break;
        case ApplySource::RECOVERY:
            g_applySourceRecoveryTotal.fetch_add(1, std::memory_order_relaxed); break;
        case ApplySource::STARTUP:
            g_applySourceStartupTotal.fetch_add(1, std::memory_order_relaxed); break;
        case ApplySource::STRONG_READ:
            g_applySourceStrongReadTotal.fetch_add(1, std::memory_order_relaxed); break;
        case ApplySource::SNAPSHOT:
            g_applySourceSnapshotTotal.fetch_add(1, std::memory_order_relaxed); break;
    }
    return true;
}

bool RaftCore::applyCommittedEntries(uint64_t upToIndex, ApplySource source) {
    if (upToIndex == 0) return true;

    auto applyWaitStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> applyGuard(applyMutex_);
    auto applyLockAcquired = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_applyMutexWaitUs,
            std::chrono::duration_cast<std::chrono::microseconds>(applyLockAcquired - applyWaitStart).count());
    }
    auto applyTotalStart = applyLockAcquired;
    const long long priorOwners = g_activeApplyOwners.fetch_add(1, std::memory_order_acq_rel);
    struct ApplyOwnerGuard {
        ~ApplyOwnerGuard() {
            g_activeApplyOwners.fetch_sub(1, std::memory_order_acq_rel);
        }
    } applyOwnerGuard;
    if (priorOwners != 0) {
        g_concurrentApplyDetectedTotal.fetch_add(1, std::memory_order_relaxed);
        g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[RAFTCORE][APPLY] concurrent owner detected source="
                  << applySourceName(source) << " activeBefore=" << priorOwners << std::endl;
        return false;
    }

    const uint64_t committedAtStart = commitIndex_.load(std::memory_order_acquire);
    if (upToIndex > committedAtStart) {
        g_outOfOrderApplyAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
        g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[RAFTCORE][APPLY] refused target beyond commit source="
                  << applySourceName(source) << " target=" << upToIndex
                  << " commit=" << committedAtStart << std::endl;
        return false;
    }

    RAFT_TLOG("apply_committed_start up_to=" << upToIndex
              << " last_applied=" << lastApplied_.load()
              << " commit_index=" << commitIndex_ << "\n");

    uint64_t startIndex = lastApplied_.load() + 1;
    if (startIndex > upToIndex) return true;
    pacificdb::test::hitFailpoint("FP_APPLY_AFTER_RANGE_CLAIM", startIndex);

    struct LogEntry {
        uint64_t index;
        json payload;
        uint64_t endOffset;
    };

    std::vector<LogEntry> toApply;
    uint64_t newOffset = lastAppliedOffset_.load();
    uint64_t expectedIndex = startIndex;
    size_t batchBytes = 0;
    bool decodeFailed = false;
    bool gapDetected = false;
    const size_t maxEntries = raftApplyBatchMaxEntries();
    const size_t maxBytes = raftApplyBatchMaxBytes();
    const int maxBatchMs = raftApplyBatchMaxMs();

    {
        std::lock_guard<std::mutex> lk(logMutex_);
        std::string base = dataRoot();
        if (base.back() != '/' && base.back() != '\\') base += "/";
        std::string logPath = base + "raft/log.bin";

        std::ifstream in(logPath, std::ios::binary);
        if (!in.is_open()) return false;

        std::error_code ec;
        uint64_t fileSize = std::filesystem::file_size(logPath, ec);
        if (ec || newOffset > fileSize) newOffset = 0;

        auto scanFrom = [&](uint64_t offset, bool allowRestart) -> bool {
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            bool firstEntry = true;
            const auto batchStart = std::chrono::steady_clock::now();

            while (in) {
                if (!toApply.empty()) {
                    if (toApply.size() >= maxEntries) return true;
                    if (batchBytes >= maxBytes) return true;
                    if (maxBatchMs > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - batchStart).count() >= maxBatchMs) {
                        return true;
                    }
                }
                std::streamoff entryStart = in.tellg();
                if (entryStart < 0) break;

                uint64_t idx = 0;
                uint64_t term = 0;
                uint32_t sz = 0;

                if (!in.read(reinterpret_cast<char*>(&idx), sizeof(idx))) break;
                if (!in.read(reinterpret_cast<char*>(&term), sizeof(term))) break;
                if (!in.read(reinterpret_cast<char*>(&sz), sizeof(sz))) break;

                if (firstEntry) {
                    firstEntry = false;
                    if (offset != 0 && idx != startIndex && allowRestart) {
                        return false;
                    }
                }

                if (sz == 0 || sz > (64U * 1024U * 1024U)) {
                    if (offset != 0 && allowRestart) return false;
                    decodeFailed = true;
                    newOffset = static_cast<uint64_t>(entryStart);
                    g_applyDecodeFailures.fetch_add(1, std::memory_order_relaxed);
                    g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                    g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[RAFTCORE][APPLY] invalid payload size index=" << idx
                              << " offset=" << entryStart << " size=" << sz << std::endl;
                    return true;
                }

                std::string payload(sz, '\0');
                if (!in.read(&payload[0], sz)) break;

                std::streamoff entryEnd = in.tellg();
                if (idx < startIndex) {
                    newOffset = static_cast<uint64_t>(entryEnd);
                    continue;
                }
                if (idx > upToIndex) {
                    newOffset = static_cast<uint64_t>(entryStart);
                    return true;
                }

                if (idx != expectedIndex) {
                    if (allowRestart) return false;
                    gapDetected = true;
                    newOffset = static_cast<uint64_t>(entryStart);
                    g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                    g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[RAFTCORE][APPLY] committed log gap expected="
                              << expectedIndex << " next=" << idx
                              << " offset=" << entryStart << std::endl;
                    return true;
                }

                try {
                    auto parseStart = std::chrono::steady_clock::now();
                    toApply.push_back(LogEntry{
                        idx,
                        decodeRaftPayloadOrThrow(payload),
                        static_cast<uint64_t>(entryEnd)
                    });
                    batchBytes += payload.size();
                    std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
                    raftRecordMetric(g_applyJsonPrepareUs,
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - parseStart).count());
                    ++expectedIndex;
                } catch (const std::exception& ex) {
                    decodeFailed = true;
                    newOffset = static_cast<uint64_t>(entryStart);
                    g_applyDecodeFailures.fetch_add(1, std::memory_order_relaxed);
                    g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                    g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[RAFTCORE][APPLY] decode failed index=" << idx
                              << " offset=" << entryStart
                              << " error=" << ex.what() << std::endl;
                    return true;
                } catch (...) {
                    decodeFailed = true;
                    newOffset = static_cast<uint64_t>(entryStart);
                    g_applyDecodeFailures.fetch_add(1, std::memory_order_relaxed);
                    g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                    g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[RAFTCORE][APPLY] decode failed index=" << idx
                              << " offset=" << entryStart
                              << " error=unknown" << std::endl;
                    return true;
                }

                newOffset = static_cast<uint64_t>(entryEnd);
            }

            return true;
        };

        bool ok = scanFrom(newOffset, true);
        if (!ok) {
            toApply.clear();
            newOffset = 0;
            expectedIndex = startIndex;
            scanFrom(0, false);
        }
    }

    if (toApply.empty()) {
        lastAppliedOffset_.store(newOffset);
        return lastApplied_.load(std::memory_order_acquire) >= upToIndex;
    }
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_applyBatchEntries, static_cast<long long>(toApply.size()));
        raftRecordMetric(g_applyBatchBytes, static_cast<long long>(batchBytes));
    }

    std::size_t committedCount = 0;

    static const int64_t applyDelayMs = raftEnvIntMs("RAFT_APPLY_DELAY_MS");
    static const int64_t applyJitterMs = raftEnvIntMs("RAFT_APPLY_JITTER_MS");
    static const bool applyFollowerOnly = raftEnvIntMs("RAFT_APPLY_DELAY_FOLLOWER_ONLY", 1) != 0;
    const bool isLeaderSnapshot = leader_.load();

    for (const auto& entry : toApply) {
        try {
            const uint64_t expectedIndex =
                lastApplied_.load(std::memory_order_acquire) + 1;
            if (entry.index != expectedIndex) {
                g_outOfOrderApplyAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
                if (entry.index > expectedIndex) {
                    g_skippedApplyIndexTotal.fetch_add(
                        static_cast<long long>(entry.index - expectedIndex),
                        std::memory_order_relaxed);
                } else {
                    g_duplicateApplyAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
                }
                g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                std::cerr << "[RAFTCORE][APPLY] refused entry before mutation expected="
                          << expectedIndex << " actual=" << entry.index << std::endl;
                return false;
            }
            g_applyEntryStarted.fetch_add(1, std::memory_order_relaxed);
            auto entryStart = std::chrono::steady_clock::now();
            if (applyDelayMs > 0 || applyJitterMs > 0) {
                if (!applyFollowerOnly || !isLeaderSnapshot) {
                    raftMaybeDelayMs(applyDelayMs, applyJitterMs);
                }
            }
            json payload = markEntryCommittedVisible(entry.payload, entry.index, currentTerm_);
            if (!isRaftNoopEntry(payload)) {
                pacificdb::test::hitFailpoint(
                    "FP_APPLY_BEFORE_BASE_MUTATION", entry.index);
                auto storageStart = std::chrono::steady_clock::now();
                bool applied = DatabaseEngine::applyReplicatedEntry(payload);
                auto storageEnd = std::chrono::steady_clock::now();
                // v5.5P-R6.9: scope this metrics lock so it is released BEFORE
                // raftVerifyPayloadVisible() — that function records its own metric under the
                // same g_raftWriteMetricsMutex, so holding it here self-deadlocked every write
                // that needs a visibility check (inserts), while schema ops (which skip the
                // check) appeared to work. This deadlock blocked all inserts ("load not coming").
                {
                    std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
                    raftRecordMetric(g_applyStorageDurationUs,
                        std::chrono::duration_cast<std::chrono::microseconds>(storageEnd - storageStart).count());
                }
                if (!applied) {
                    g_applyStorageFailures.fetch_add(1, std::memory_order_relaxed);
                    g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                    g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[RAFTCORE][APPLY] storage apply failed index=" << entry.index
                              << " payload=" << payload.dump(512) << std::endl;
                    return false;
                }
                pacificdb::test::hitFailpoint(
                    "FP_APPLY_AFTER_INDEX_BEFORE_PROGRESS", entry.index);
                pacificdb::test::hitFailpoint(
                    "FP_APPLY_BEFORE_VISIBILITY_PUBLICATION", entry.index);
                std::string visibilityReason;
                if (!raftVerifyPayloadVisible(payload, visibilityReason)) {
                    g_applyVisibilityFailures.fetch_add(1, std::memory_order_relaxed);
                    g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                    g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "[RAFTCORE][APPLY] visibility check failed index=" << entry.index
                              << " reason=" << visibilityReason
                              << " payload=" << payload.dump(512) << std::endl;
                    return false;
                }
                pacificdb::test::hitFailpoint(
                    "FP_APPLY_AFTER_VISIBILITY_BEFORE_PROGRESS", entry.index);
            } else {
                g_applyEntrySkipped.fetch_add(1, std::memory_order_relaxed);
            }
            // Destructive exact-boundary builds retain per-entry persistence so their
            // crash failpoints keep their original meaning. Production relies on the
            // already-durable Raft log and checkpoints progress during clean shutdown.
#ifdef PACIFICDB_TEST_FAILPOINTS
            persistAppliedProgress(entry.index);
#endif
            pacificdb::test::hitFailpoint(
                "FP_APPLY_AFTER_PROGRESS_BEFORE_LAST_APPLIED", entry.index);
            if (!advanceLastApplied(entry.index, source)) {
                g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
                g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            pacificdb::test::hitFailpoint(
                "FP_APPLY_AFTER_LAST_APPLIED_BEFORE_NOTIFY", entry.index);
            lastAppliedOffset_.store(entry.endOffset, std::memory_order_release);
#ifdef PACIFICDB_TEST_FAILPOINTS
            publishAppliedIndex();
#endif
            g_applyEntrySucceeded.fetch_add(1, std::memory_order_relaxed);
            g_lastAppliedAdvanceCount.fetch_add(1, std::memory_order_relaxed);
            ++committedCount;
            auto entryEnd = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
                raftRecordMetric(g_applyEntryDurationUs,
                    std::chrono::duration_cast<std::chrono::microseconds>(entryEnd - entryStart).count());
            }
        } catch (const std::exception& ex) {
            std::cerr << "[RAFTCORE] Failed to apply committed entry: " << ex.what() << std::endl;
            g_applyEntryFailed.fetch_add(1, std::memory_order_relaxed);
            g_applyFatalStopCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

#ifndef PACIFICDB_TEST_FAILPOINTS
    if (committedCount != 0) publishAppliedIndex();
#endif
    if (committedCount != 0) {
        RAFT_TLOG("apply_committed_complete last_applied=" << toApply.back().index
                  << " commit_index=" << commitIndex_
                  << " entries=" << committedCount << "\n");
    }
    if (decodeFailed) {
        g_applyRetryCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (gapDetected) {
        g_applyRetryCount.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
        raftRecordMetric(g_raftBatchApplyMs,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - applyTotalStart).count());
    }
    return lastApplied_.load(std::memory_order_acquire) >= upToIndex;
}

void RaftCore::handleFollowerConn(int clientSock, const std::atomic<size_t>& pendingConnections) {
    // v5.5P-R2B FD-LEAK FIX: RAII guard closes the socket on EVERY return path.
    // Previously ~28 early-return branches (heartbeat/vote/append acks) returned
    // without closing, leaking one fd per Raft RPC (heartbeats fire every 500ms x
    // peers) -> EMFILE (errno=24) -> raft listener accept failures on 4-core nodes.
    struct SockGuard {
        int fd;
        ~SockGuard() {
            if (fd >= 0) {
                closeRaftNetworkSocket(fd);
            }
        }
    } _sockGuard{clientSock};
    // A persistent peer must not pin a bounded inbound worker forever while
    // idle (including during TLS handshake or a partial frame). The sender
    // already reconnects closed persistent sockets on the next RPC.
    static const int idleTimeoutMs = [] {
        if (const char* value = std::getenv("RAFT_INBOUND_IDLE_TIMEOUT_MS")) {
            try { return std::max(100, std::stoi(value)); } catch (...) {}
        }
        return 1000;
    }();
#ifdef _WIN32
    DWORD idleTimeout = static_cast<DWORD>(idleTimeoutMs);
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&idleTimeout), sizeof(idleTimeout));
    setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&idleTimeout), sizeof(idleTimeout));
#else
    timeval idleTimeout{idleTimeoutMs / 1000, (idleTimeoutMs % 1000) * 1000};
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &idleTimeout, sizeof(idleTimeout));
    setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, &idleTimeout, sizeof(idleTimeout));
#endif
    if (!attachRaftTls(clientSock, true)) {
        return;
    }
    raftOpenConnections_.fetch_add(1, std::memory_order_relaxed);
    raftTotalAccepted_.fetch_add(1, std::memory_order_relaxed);
    struct ConnCounter { std::atomic<long>& open; std::atomic<long>& closed;
        ~ConnCounter() { open.fetch_sub(1, std::memory_order_relaxed); closed.fetch_add(1, std::memory_order_relaxed); } }
        _connCounter{raftOpenConnections_, raftTotalClosed_};
    const bool keepAlive = raftReplicatorReuseConnection();
    try {
      for (;;) {  // v5.5P-R5.6: serve multiple framed RPCs per connection (persistent peer conn)
        uint32_t len = 0;
        int r = raftSocketRecv(clientSock, reinterpret_cast<char*>(&len), sizeof(len), 0);
        if (r != sizeof(len)) {
            break; }  // peer closed / framing end -> close connection
        std::string payload(len, '\0');
        int got = 0;
        while (got < (int)len) {
            int br = raftSocketRecv(clientSock, &payload[got], len - got, 0);
            if (br <= 0) break;
            got += br;
        }
        if (got < (int)len) break;  // incomplete frame -> close
        RAFT_DLOG("[RAFTCORE] Received replicated payload (" << got << " bytes)" << std::endl);

        static const int64_t ackDelayMs = raftEnvIntMs("RAFT_ACK_DELAY_MS");
        static const int64_t ackJitterMs = raftEnvIntMs("RAFT_ACK_JITTER_MS");

        // parse and apply: write to raft log and WAL, then apply
        // v5.5P-R5.6: per-request handler as a lambda; each `return;` below ends THIS request
        // (returns from the lambda), then the loop reads the next framed request on the same conn.
        auto serveOne = [&]() {
        try {
                auto j = decodeRaftPayloadOrThrow(payload);
            // handle control messages (heartbeat / vote requests)
            if (j.contains("type")) {
                static const std::string clusterId = [] {
                    const char* value = std::getenv("RAFT_CLUSTER_ID");
                    return value ? std::string(value) : std::string();
                }();
                if (!clusterId.empty()) {
                    std::string incomingCluster = j.value("clusterId", "");
                    if (incomingCluster != clusterId) {
                        std::string resp = json({
                            {"type", "raftError"},
                            {"term", currentTerm_},
                            {"success", false},
                            {"reason", "cluster_mismatch"}
                        }).dump();
                        raftSocketSend(clientSock, resp.c_str(), (int)resp.size(), 0);
                        return;
                    }
                }
                std::string t = j.value("type", "");
	                if (t == "heartbeat") {
	                    uint64_t term = j.value("term", (uint64_t)0);
	                    bool leaderReady = j.value("leaderReady", true);
	                    bool hasLeaderLogPosition = j.contains("leaderLastLogIndex") || j.contains("lastLogIndex");
	                    uint64_t leaderLastIndex = j.value("leaderLastLogIndex", j.value("lastLogIndex", (uint64_t)0));
	                    uint64_t leaderLastTerm = j.value("leaderLastLogTerm", j.value("lastLogTerm", (uint64_t)0));
	                    uint64_t localLastIndexSnapshot = std::max(lastIndex_, lastSnapshotIndex_);
	                    // getTermForIndex() opens and linearly scans raft/log.bin. Only the
	                    // behind-leader check needs it, and read-barrier heartbeats carry no log
	                    // position at all, so computing it unconditionally put a full log scan on
	                    // every heartbeat -- the dominant remaining ack cost on a spinning disk.
	                    uint64_t localLastTermSnapshot = 0;
	                    bool leaderLogBehind = false;
	                    bool leaderTailMatches = false;
	                    if (hasLeaderLogPosition) {
	                        localLastTermSnapshot = getTermForIndex(localLastIndexSnapshot);
	                        if (localLastTermSnapshot == 0) localLastTermSnapshot = lastEntryTerm_;
	                        leaderTailMatches = leaderLastIndex == localLastIndexSnapshot &&
	                            leaderLastTerm == localLastTermSnapshot;
	                        leaderLogBehind =
	                            (leaderLastTerm < localLastTermSnapshot) ||
	                            (leaderLastTerm == localLastTermSnapshot && leaderLastIndex < localLastIndexSnapshot);
	                    }
	                    RAFT_CT("heartbeat_rx node=" << nodeId_
	                            << " from=" << j.value("leader", std::string(""))
	                            << " term=" << term
	                            << " localTerm=" << currentTerm_
	                            << " leaderReady=" << (leaderReady ? "true" : "false")
	                            << " leaderLast=" << leaderLastIndex << "/" << leaderLastTerm
	                            << " localLast=" << localLastIndexSnapshot << "/" << localLastTermSnapshot
	                            << " leaderLogBehind=" << (leaderLogBehind ? "true" : "false")
	                            << " leaderTailMatches=" << (leaderTailMatches ? "true" : "false")
	                            << " isLeader=" << (leader_.load() ? "true" : "false"));
	                    uint64_t localTerm = 0;
	                    bool accepted = false;
	                    std::string rejectReason = "stale_term";
	                    {
	                        std::lock_guard<std::mutex> el(electionMutex_);
	                        localTerm = currentTerm_;
	                        if (term < currentTerm_) {
	                            accepted = false;
	                            rejectReason = "stale_term";
	                        } else if (leaderLogBehind &&
                                   !(term == currentTerm_ && localLastTermSnapshot == term &&
                                     j.value("leader", std::string()) == leaderId_)) {
                            // Appends and heartbeats use independent connections. A
                            // heartbeat from the established same-term leader can
                            // describe an older tail than an append already received.
                            // Accept contact in that case; leaderTailMatches remains
                            // false, so this does not advance commitIndex below.
	                            // V8.5.8: a node with a fresher log must not let a behind
	                            // leader's heartbeat reset its election deadline. B/C could
	                            // elect each other with quorum while A had newer committed
	                            // entries; accepting those stale heartbeats kept A asleep and
	                            // produced a no-leader term storm. We still learn the higher
	                            // term, but we do not install the behind leader or refresh
	                            // lastHeartbeat_.
	                            if (term > currentTerm_) {
	                                currentTerm_ = term;
	                                leader_.store(false);
	                                votedFor_.clear();
	                                persistCurrentTerm();
	                                localTerm = currentTerm_;
	                            }
	                            accepted = false;
	                            rejectReason = "leader_log_behind";
	                        } else {
	                            accepted = true;
	                        }
	                        if (accepted && term >= currentTerm_) {
                            adoptLeaderTermLocked(term);
                            leaderId_ = j.value("leader", "");
                        }
	                    }
	                    if (!accepted) {
	                        std::string stale = j.value("rpcFormat", "") == "json"
	                            ? json({{"type", "appendEntriesResponse"}, {"term", localTerm}, {"success", false}, {"matchIndex", localLastIndexSnapshot}, {"reason", rejectReason}}).dump()
	                            : ("stale_term:" + std::to_string(localTerm) + ":" + rejectReason);
	                        raftMaybeDelayMs(ackDelayMs, ackJitterMs);
	                        raftSocketSend(clientSock, stale.c_str(), (int)stale.size(), 0);
	                        return;
	                    }
                    lastHeartbeat_.store(steadyNowMs());
                    // v5.5P-R1: advance commit + apply on heartbeat so the last committed
                    // entry converges during quiescence (replica lag returns to 0).
                    // V11.4-DIV-001 P0.6: never advance progress while this node's own
                    // state restore is incomplete — that is progress beyond
                    // recoverability.
                    // This ad-hoc heartbeat is not AppendEntries: when the leader is
                    // ahead it carries no term for the follower's local tail. Only an
                    // exact tail match proves that leaderCommit covers the same log.
                    if (recoveryComplete_.load(std::memory_order_acquire) && leaderTailMatches) {
                        {
                            uint64_t leaderCommit = j.value("leaderCommit", (uint64_t)0);
                            std::lock_guard<std::mutex> el(electionMutex_);
                            uint64_t maxCommitted = std::min(leaderCommit, lastIndex_);
                            if (maxCommitted > commitIndex_) commitIndex_ = maxCommitted;
                        }
                        scheduleFollowerApply(commitIndex_);
                    }
                    std::string ack = j.value("rpcFormat", "") == "json"
                        ? json({{"type", "appendEntriesResponse"}, {"term", currentTerm_}, {"success", true},
                                {"matchIndex", leaderTailMatches ? localLastIndexSnapshot : 0},
                                {"logMatch", leaderTailMatches}, {"status", "ok"}}).dump()
                        : "ok";
                    raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                    raftSocketSend(clientSock, ack.c_str(), (int)ack.size(), 0);
                    return;
                }
                if (t == "vote_request" || t == "requestVote") {
                    uint64_t term = j.value("term", (uint64_t)0);
                    std::string candidate = j.value("candidate", j.value("candidateId", ""));
                    // Candidate's last log info (for up-to-date check)
                    uint64_t candidateLastIndex = j.value("lastLogIndex", (uint64_t)0);
                    uint64_t candidateLastTerm = j.value("lastLogTerm", (uint64_t)0);
                    bool grant = false;
                    uint64_t currentTermSnapshot = 0;
                    std::string rejectReason = "denied";
                    static const int voteFenceMs = [] {
                        int value = 3000;
                        if (const char* e = std::getenv("RAFT_ELECTION_TIMEOUT_MIN_MS")) { try { value = std::max(500, std::stoi(e)); } catch (...) {} }
                        if (const char* e = std::getenv("RAFT_ELECTION_TIMEOUT_MS")) { try { value = std::max(500, std::stoi(e)); } catch (...) {} }
                        return value;
                    }();
                    const bool leaderLeaseActive = leader_.load() && hasQuorum();
                    const bool freshLeaderHeartbeat = !leader_.load()
                        && !leaderId_.empty()
                        && millisSinceLastHeartbeat() < static_cast<uint64_t>(voteFenceMs);
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        currentTermSnapshot = currentTerm_;
                        if (leaderLeaseActive && candidate != nodeId_) {
                            // v5.5P-R1: a leader that still has quorum must not step down
                            // just because an out-of-date follower increments its term.
                            // This is a leader-lease/pre-vote style fence to stop term wars.
                            rejectReason = "leader_lease_active";
                        } else if (freshLeaderHeartbeat && candidate != leaderId_) {
                            // v5.5P-R1: followers that recently heard from a valid leader
                            // reject disruptive elections and keep the current term stable.
                            rejectReason = "fresh_leader_heartbeat";
                        } else {
                        // Step down if we see a newer term
                        if (term > currentTerm_) {
                            currentTerm_ = term;
                            leader_.store(false);
                            votedFor_.clear();
                            persistCurrentTerm();
                            currentTermSnapshot = currentTerm_;
                        }
                        // V11.4-DIV-001 P0.6: a node whose own state restore has not
                        // completed (or failed) must never grant a vote — its
                        // lastIndex_/lastEntryTerm_ may be unrestored zeros and the
                        // up-to-date check below would approve a stale-log candidate
                        // (the exact term-3183 committed-entry rollback vector).
                        if (!recoveryComplete_.load(std::memory_order_acquire)) {
                            rejectReason = "recovery_incomplete";
                        }
                        // Grant vote only once per term AND only if candidate's log is at least as up-to-date
                        else if (term == currentTerm_ && !candidate.empty() && (votedFor_.empty() || votedFor_ == candidate)) {
                            // Log up-to-date check (Raft: compare last term then last index)
                            bool candidateUpToDate = false;
                            if (candidateLastTerm > lastEntryTerm_) candidateUpToDate = true;
                            else if (candidateLastTerm == lastEntryTerm_ && candidateLastIndex >= lastIndex_) candidateUpToDate = true;
                            if (candidateUpToDate) {
                                votedFor_ = candidate;
                                persistVotedFor();
                                // A granted vote starts a fresh election timeout. Without
                                // this, an already-expired follower immediately campaigns
                                // against the candidate it just elected.
                                lastHeartbeat_.store(steadyNowMs(), std::memory_order_release);
                                grant = true;
                                rejectReason = "granted";
                            }
                        }
                        }
                    }
	                    std::string resp;
	                    if (t == "requestVote" || j.value("rpcFormat", "") == "json") {
	                        uint64_t responseLastIndex = std::max(lastIndex_, lastSnapshotIndex_);
	                        uint64_t responseLastTerm = getTermForIndex(responseLastIndex);
	                        uint64_t responseCommitIndex = commitIndex_;
	                        resp = json({
		                            {"type", "requestVoteResponse"},
		                            {"term", currentTermSnapshot},
		                            {"voteGranted", grant},
		                            {"reason", grant ? "granted" : rejectReason},
		                            {"lastLogIndex", responseLastIndex},
		                            {"lastLogTerm", responseLastTerm},
		                            {"commitIndex", responseCommitIndex}
		                        }).dump();
                    } else {
                        resp = grant ? "vote_granted" : ("vote_denied:" + std::to_string(currentTermSnapshot) + ":" + rejectReason);
                    }
                    raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                    raftSocketSend(clientSock, resp.c_str(), (int)resp.size(), 0);
                    return;
                }
                if (t == "append_entries" || t == "appendEntries") {
                    uint64_t term = j.value("term", (uint64_t)0);
                    RAFT_CT("append_entries_rx node=" << nodeId_
                            << " from=" << j.value("leader", j.value("leaderId", std::string("")))
                            << " term=" << term
                            << " localTerm=" << currentTerm_
                            << " entries=" << (j.contains("entries") && j["entries"].is_array() ? j["entries"].size() : 0)
                            << " isLeader=" << (leader_.load() ? "true" : "false"));
                    uint64_t localTerm = 0;
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        localTerm = currentTerm_;
                        if (term >= currentTerm_) {
                            adoptLeaderTermLocked(term);
                        leaderId_ = j.value("leader", j.value("leaderId", ""));
                            localTerm = currentTerm_;
                        }
                    }
                    if (term < localTerm) {
                        std::string stale = "stale_term:" + std::to_string(localTerm);
                        raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                        raftSocketSend(clientSock, stale.c_str(), (int)stale.size(), 0);
                        return;
                    }

                    // V11.4-DIV-001 P0.6: a node whose state restore is incomplete or
                    // failed must not mutate its log or advance progress from
                    // AppendEntries — its own base is untrustworthy. Repair happens
                    // through snapshot install, which replaces state wholesale.
                    if (!recoveryComplete_.load(std::memory_order_acquire)) {
                        std::string resp = (t == "appendEntries" || j.value("rpcFormat", "") == "json")
                            ? json({{"type", "appendEntriesResponse"}, {"term", localTerm},
                                    {"success", false}, {"matchIndex", (uint64_t)0},
                                    {"reason", "recovery_incomplete"}}).dump()
                            : std::string("recovery_incomplete");
                        raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                        raftSocketSend(clientSock, resp.c_str(), (int)resp.size(), 0);
                        return;
                    }

	                    // AppendEntries RPC: check prevLogIndex/prevLogTerm, truncate conflicts, append entries.
                    uint64_t prevIndex = j.value("prevLogIndex", (uint64_t)0);
                    uint64_t prevTerm = j.value("prevLogTerm", (uint64_t)0);
                    uint64_t leaderCommit = j.value("leaderCommit", (uint64_t)0);
                    auto entries = j.value("entries", json::array());
                    auto entryTerms = j.value("entryTerms", json::array());
                    long long followerAppendBytes = 0;
                    std::vector<std::string> entryPayloads;
                    entryPayloads.reserve(entries.size());
                    for (const auto& e : entries) {
                        entryPayloads.push_back(encodeRaftLogPayload(e));
                        followerAppendBytes += static_cast<long long>(entryPayloads.back().size());
                    }
                    auto followerAppendStart = std::chrono::steady_clock::now();

                    std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
                    std::string logPath = base + "raft/log.bin";

                    {
                        std::lock_guard<std::mutex> lk(logMutex_);

                        uint64_t effectiveLastIndex = std::max(lastIndex_, lastSnapshotIndex_);

	                        if (prevIndex > effectiveLastIndex) {
                            std::string resp = (t == "appendEntries" || j.value("rpcFormat", "") == "json")
                                ? json({{"type", "appendEntriesResponse"}, {"term", localTerm}, {"success", false}, {"matchIndex", effectiveLastIndex}, {"reason", "prev_index_conflict"}}).dump()
                                : ("conflict:" + std::to_string(effectiveLastIndex));
                            raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                            raftSocketSend(clientSock, resp.c_str(), (int)resp.size(), 0);
	                            return;
	                        }

                        if (!entries.empty() && lastSnapshotIndex_ > 0 && prevIndex < lastSnapshotIndex_) {
                            std::string resp = (t == "appendEntries" || j.value("rpcFormat", "") == "json")
                                ? json({{"type", "appendEntriesResponse"}, {"term", localTerm}, {"success", false}, {"matchIndex", lastSnapshotIndex_}, {"reason", "snapshot_floor_conflict"}}).dump()
                                : ("conflict:" + std::to_string(lastSnapshotIndex_));
                            raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                            raftSocketSend(clientSock, resp.c_str(), (int)resp.size(), 0);
                            return;
                        }

                        uint64_t localTermAtPrev = getTermForIndex(prevIndex);
	                        if (prevIndex > 0 && localTermAtPrev != prevTerm) {
                            std::string resp = (t == "appendEntries" || j.value("rpcFormat", "") == "json")
                                ? json({{"type", "appendEntriesResponse"}, {"term", localTerm}, {"success", false}, {"matchIndex", effectiveLastIndex}, {"reason", "prev_term_conflict"}}).dump()
                                : ("conflict:" + std::to_string(effectiveLastIndex));
                            raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                            raftSocketSend(clientSock, resp.c_str(), (int)resp.size(), 0);
	                            return;
	                        }

	                        // V8.5.8: only a log-compatible AppendEntries counts as a valid
	                        // leader heartbeat. Conflict responses above intentionally do not
	                        // refresh lastHeartbeat_, so a fresher follower can still elect.
	                        lastHeartbeat_.store(steadyNowMs());

                        // Skip an already-matching prefix and truncate only at the first
                        // genuine term conflict. AppendEntries batches are not complete-log
                        // declarations: a matching short batch does not authorize deleting
                        // the follower's later suffix.
                        std::size_t appendFrom = 0;
                        if (!entries.empty() && prevIndex < lastIndex_) {
                            // V11.4-DIV-001 P0.6: never truncate committed history. In the
                            // incident a stale-log leader's noop at 138216 destroyed a
                            // committed+applied entry on every follower. A correctly elected
                            // leader always contains all committed entries, so a truncation
                            // request below our commit floor proves an unsafe election or
                            // divergent history: fail closed, fence the node, audit durably.
                            const uint64_t commitFloor = std::max(
                                commitIndex_.load(std::memory_order_acquire),
                                lastApplied_.load(std::memory_order_acquire));

                            // Only a GENUINE conflict destroys committed history. A leader
                            // re-sending entries we already hold (ordinary backfill, where
                            // nextIndex was rewound after a follower restart) presents
                            // prevIndex < commitFloor but changes nothing — refusing that
                            // would fence healthy followers and stall replication.
                            //
                            // Find the first index where our log actually disagrees with the
                            // leader. Matching entries remain in place and are not appended
                            // again. If the local log ends, append from that first absent
                            // entry. Only a term mismatch authorizes suffix truncation.
                            uint64_t firstConflict = 0;
                            for (size_t k = 0; k < entries.size(); ++k) {
                                const uint64_t idx = prevIndex + 1 + k;
                                if (idx > lastIndex_) {
                                    appendFrom = k;
                                    break;
                                }
                                const uint64_t localTerm = getTermForIndex(idx);
                                uint64_t incomingTerm = term;
                                if (k < entryTerms.size() &&
                                    (entryTerms[k].is_number_unsigned() ||
                                     entryTerms[k].is_number_integer())) {
                                    incomingTerm = entryTerms[k].get<uint64_t>();
                                }
                                if (localTerm != incomingTerm) {
                                    firstConflict = idx;
                                    appendFrom = k;
                                    break;
                                }
                                appendFrom = k + 1;
                            }

                            // Negative-control failpoint (test builds only): re-enable the
                            // pre-P0.6 vulnerable truncation so the regression can prove the
                            // old behavior destroys committed history and is detected.
                            const bool vulnerableTruncation = firstConflict != 0
                                && pacificdb::test::injectApplyFailure(
                                    "FP_BYPASS_TRUNCATION_COMMIT_FLOOR", prevIndex);
                            if (firstConflict != 0 && firstConflict <= commitFloor
                                && !vulnerableTruncation) {
                                divergenceDetected_.store(true, std::memory_order_release);
                                recordSafetyAudit("append_entries_truncation_below_commit_floor", {
                                    {"leader", j.value("leader", j.value("leaderId", std::string()))},
                                    {"leaderTerm", term},
                                    {"prevIndex", prevIndex},
                                    {"prevTerm", prevTerm},
                                    {"firstConflictIndex", firstConflict},
                                    {"commitFloor", commitFloor},
                                    {"localLastIndex", lastIndex_},
                                    {"localLastEntryTerm", lastEntryTerm_},
                                    {"refused", true},
                                });
                                std::cerr << "[RAFTCORE] REFUSED committed-history truncation:"
                                          << " firstConflict=" << firstConflict
                                          << " <= commitFloor=" << commitFloor
                                          << " (prevIndex=" << prevIndex << ")"
                                          << " — node fenced as divergent" << std::endl;
                                std::string resp = (t == "appendEntries" || j.value("rpcFormat", "") == "json")
                                    ? json({{"type", "appendEntriesResponse"}, {"term", localTerm},
                                            {"success", false}, {"matchIndex", commitFloor},
                                            {"reason", "truncation_below_commit_floor"}}).dump()
                                    : ("conflict_committed:" + std::to_string(commitFloor));
                                raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                                raftSocketSend(clientSock, resp.c_str(), (int)resp.size(), 0);
                                return;
                            }
                            if (firstConflict != 0) {
                                const uint64_t truncateAfter = firstConflict - 1;
                                std::string tmpPath = logPath + ".truncate.tmp";
                                std::ifstream in(logPath, std::ios::binary);
                                std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
                                if (!in.is_open() || !out.is_open()) {
                                    std::string nack = "no";
                                    raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                                    raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                                    return;
                                }

                                uint64_t keepLastIndex = 0;
                                uint64_t keepLastTerm = 0;
                                while (!in.eof()) {
                                    uint64_t idx = 0;
                                    uint64_t et = 0;
                                    uint32_t sz = 0;
                                    in.read(reinterpret_cast<char*>(&idx), sizeof(idx));
                                    if (!in) break;
                                    in.read(reinterpret_cast<char*>(&et), sizeof(et));
                                    in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
                                    if (!in) break;
                                    std::string p(sz, '\0');
                                    in.read(&p[0], sz);
                                    if (!in) break;

                                    if (idx <= truncateAfter) {
                                        out.write(reinterpret_cast<char*>(&idx), sizeof(idx));
                                        out.write(reinterpret_cast<char*>(&et), sizeof(et));
                                        out.write(reinterpret_cast<char*>(&sz), sizeof(sz));
                                        out.write(p.data(), sz);
                                        keepLastIndex = idx;
                                        keepLastTerm = et;
                                    }
                                }
                                in.close();
                                out.flush();
                                out.close();

                                std::error_code ec;
                                resetRaftLog(logPath);
                                std::filesystem::remove(logPath, ec);
                                std::filesystem::rename(tmpPath, logPath, ec);
                                // Conflicting suffix removed: cached index->term pairs for the
                                // truncated range are now wrong and must be dropped.
                                if (!ec) invalidateTermCache();
                                if (ec) {
                                    std::cerr << "[RAFTCORE] Failed to truncate log: "
                                              << ec.message() << std::endl;
                                    std::string nack = "no";
                                    raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                                    return;
                                }

                                if (keepLastIndex >= lastSnapshotIndex_) {
                                    lastIndex_ = keepLastIndex;
                                    lastEntryTerm_ = keepLastIndex == 0 ? 0 : keepLastTerm;
                                } else {
                                    lastIndex_ = lastSnapshotIndex_;
                                    lastEntryTerm_ = lastSnapshotTerm_;
                                }
                            }
                        }

                        if (appendFrom < entries.size()) {
                            std::string bytes;
                            std::vector<std::pair<uint64_t, uint64_t>> appendedTerms;
                            appendedTerms.reserve(entries.size() - appendFrom);
                            uint64_t appendedLastTerm = lastEntryTerm_;
                            for (std::size_t k = appendFrom; k < entries.size(); ++k) {
                                // Preserve the leader's Raft indexes. Followers previously
                                // assigned local 1..N indexes after snapshot install, so a
                                // suffix for leader indexes 184..288 was stored as 1..105.
                                // commitIndex/lastApplied then advanced past the local log
                                // and the committed suffix was never applied.
                                uint64_t index = prevIndex + 1 + k;
                                uint64_t entryTerm = term;
                                if (k < entryTerms.size() &&
                                    (entryTerms[k].is_number_unsigned() ||
                                     entryTerms[k].is_number_integer())) {
                                    entryTerm = entryTerms[k].get<uint64_t>();
                                }
                                appendRaftRecordBytes(bytes, index, entryTerm, entryPayloads[k]);
                                appendedTerms.push_back({index, entryTerm});
                                appendedLastTerm = entryTerm;
                            }
                            uint64_t startOffset = 0;
                            if (!appendRaftLog(logPath, bytes, &startOffset)) {
                                std::string nack = "no";
                                raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                                return;
                            }
                            cacheRaftRecordBlock(bytes, startOffset);
                            for (const auto& [index, entryTerm] : appendedTerms) {
                                lastIndex_ = std::max(lastIndex_, index);
                            }
                            lastEntryTerm_ = appendedLastTerm;
                        }
                    }
                    {
                        long long appendUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - followerAppendStart).count();
                        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
                        raftRecordMetric(g_followerAppendEntries, static_cast<long long>(entries.size()));
                        raftRecordMetric(g_followerAppendBytes, followerAppendBytes);
                        raftRecordMetric(g_followerAppendUs, appendUs);
                    }

                    // CRITICAL: durably sync appended entries (deferred in async mode)
                    auto durableStart = std::chrono::steady_clock::now();
                    raftSyncOrDeferLog(logPath);
                    {
                        long long durableMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - durableStart).count();
                        std::lock_guard<std::mutex> ml(g_raftWriteMetricsMutex);
                        raftRecordMetric(g_followerDurableAppendMs, durableMs);
                    }

                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        uint64_t maxCommitted = std::min(leaderCommit, lastIndex_);
                        if (maxCommitted > commitIndex_) {
                            commitIndex_ = maxCommitted;
                        }
                    }

                    scheduleFollowerApply(commitIndex_);

                    std::string ack = (t == "appendEntries" || j.value("rpcFormat", "") == "json")
                        ? json({{"type", "appendEntriesResponse"}, {"term", localTerm}, {"success", true}, {"matchIndex", std::max(lastIndex_, lastSnapshotIndex_)}, {"status", "ok"}}).dump()
                        : "ok";
                    raftMaybeDelayMs(ackDelayMs, ackJitterMs);
                    raftSocketSend(clientSock, ack.c_str(), (int)ack.size(), 0);
                    return;
                }
                if (t == "wal_ship") {
                    uint64_t term = j.value("term", (uint64_t)0);
                    uint64_t localTerm = 0;
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        localTerm = currentTerm_;
                        if (term >= currentTerm_) {
                            adoptLeaderTermLocked(term);
                            localTerm = currentTerm_;
                        }
                    }
                    if (term < localTerm) {
                        std::string stale = "stale_term:" + std::to_string(localTerm);
                        raftSocketSend(clientSock, stale.c_str(), (int)stale.size(), 0);
                        return;
                    }

                    uint64_t leaderCommit = j.value("leaderCommit", (uint64_t)0);
                    auto entries = j.value("entries", json::array());
                    std::vector<std::string> entryPayloads;
                    entryPayloads.reserve(entries.size());
                    for (const auto& entry : entries) entryPayloads.push_back(encodeRaftLogPayload(entry));

                    std::string base = dataRoot(); if (base.back() != '/' && base.back() != '\\') base += "/";
                    std::string logPath = base + "raft/log.bin";

                    {
                        std::lock_guard<std::mutex> lk(logMutex_);
                        std::string bytes;
                        uint64_t nextIndex = lastIndex_;
                        for (const auto& payload : entryPayloads) {
                            uint64_t index = ++nextIndex;
                            uint64_t entryTerm = term;
                            appendRaftRecordBytes(bytes, index, entryTerm, payload);
                        }
                        uint64_t startOffset = 0;
                        if (!appendRaftLog(logPath, bytes, &startOffset)) {
                            std::string nack = "no";
                            raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                            return;
                        }
                        cacheRaftRecordBlock(bytes, startOffset);
                        lastIndex_ = nextIndex;
                        lastEntryTerm_ = term;
                    }

                    // wal_ship: durably sync appended entries (deferred in async mode)
                    raftSyncOrDeferLog(logPath);

                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        uint64_t maxCommitted = std::min(leaderCommit, lastIndex_);
                        if (maxCommitted > commitIndex_) {
                            commitIndex_ = maxCommitted;
                        }
                    }
                    scheduleFollowerApply(commitIndex_);

                    std::string ack = "ok";
                    raftSocketSend(clientSock, ack.c_str(), (int)ack.size(), 0);
                    return;
                }
                if (t == "install_snapshot_chunk") {
                    uint64_t term = j.value("term", (uint64_t)0);
                    std::string leaderId = j.value("leader", "");
                    uint64_t transferId = j.value("transferId", (uint64_t)0);
                    size_t chunkIndex = j.value("chunkIndex", static_cast<size_t>(0));
                    size_t totalChunks = j.value("totalChunks", static_cast<size_t>(0));
                    std::string chunkData = j.value("chunkData", "");
                    uint64_t includedIdx = j.value("lastIncludedIndex", (uint64_t)0);
                    uint64_t includedTerm = j.value("lastIncludedTerm", (uint64_t)0);
                    std::string payloadSha256 = j.value("payloadSha256", "");

                    if (leaderId.empty() || transferId == 0 || totalChunks == 0 ||
                        chunkIndex >= totalChunks || chunkData.empty() ||
                        chunkData.size() > raftSnapshotChunkBytes() ||
                        !isSha256Hex(payloadSha256) || includedIdx == 0) {
                        std::string nack = "install_failed";
                        raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                        return;
                    }

                    uint64_t localTerm = 0;
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        localTerm = currentTerm_;
                        if (term > currentTerm_) {
                            currentTerm_ = term;
                            leader_.store(false);
                            votedFor_.clear();
                            persistCurrentTerm();
                            localTerm = currentTerm_;
                        }
                    }
                    if (term < localTerm) {
                        std::string stale = "stale_term:" + std::to_string(localTerm);
                        raftSocketSend(clientSock, stale.c_str(), (int)stale.size(), 0);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        leaderId_ = leaderId;
                    }
                    // InstallSnapshot is valid leader contact. Without this refresh a
                    // follower can start an election in the middle of a large transfer,
                    // advance the term, and force the copy to restart indefinitely.
                    lastHeartbeat_.store(steadyNowMs());

                    std::string base = dataRoot();
                    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";
                    bool stored = false;
                    {
                        std::lock_guard<std::mutex> chunkLock(g_snapshotChunkMu);
                        const std::string key = snapshotTransferKey(leaderId, transferId);
                        auto it = g_snapshotChunkTransfers.find(key);
                        if (it == g_snapshotChunkTransfers.end()) {
                            if (chunkIndex != 0) {
                                stored = false;
                            } else {
                                SnapshotChunkTransfer transfer;
                                transfer.term = term;
                                transfer.lastIncludedIndex = includedIdx;
                                transfer.lastIncludedTerm = includedTerm;
                                transfer.totalChunks = totalChunks;
                                transfer.tempPath = base + ".snapshot.transfer." +
                                    std::to_string(transferId) + ".tmp";
                                transfer.payloadSha256 = payloadSha256;
                                transfer.updatedAt = std::chrono::steady_clock::now();
                                std::error_code removeEc;
                                std::filesystem::remove(transfer.tempPath, removeEc);
                                std::ofstream create(transfer.tempPath,
                                    std::ios::binary | std::ios::trunc);
                                create.close();
                                if (create) {
                                    it = g_snapshotChunkTransfers.emplace(
                                        key, std::move(transfer)).first;
                                }
                            }
                        }
                        if (it != g_snapshotChunkTransfers.end()) {
                            auto& transfer = it->second;
                            const bool matches = transfer.term == term &&
                                transfer.totalChunks == totalChunks &&
                                transfer.nextChunk == chunkIndex &&
                                transfer.lastIncludedIndex == includedIdx &&
                                transfer.lastIncludedTerm == includedTerm &&
                                transfer.payloadSha256 == payloadSha256;
                            if (matches) {
                                std::ofstream append(transfer.tempPath,
                                    std::ios::binary | std::ios::app);
                                append.write(chunkData.data(),
                                    static_cast<std::streamsize>(chunkData.size()));
                                append.flush();
                                append.close();
                                stored = static_cast<bool>(append);
                                if (stored) {
                                    ++transfer.nextChunk;
                                    transfer.updatedAt = std::chrono::steady_clock::now();
                                }
                            }
                            if (!stored) {
                                std::error_code removeEc;
                                std::filesystem::remove(transfer.tempPath, removeEc);
                                g_snapshotChunkTransfers.erase(it);
                            }
                        }
                    }

                    if (!stored) {
                        std::string nack = "install_failed";
                        raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                        return;
                    }
                    pruneSnapshotChunkTransfers();

                    std::string ack = "chunk_ok:" + std::to_string(chunkIndex);
                    raftSocketSend(clientSock, ack.c_str(), (int)ack.size(), 0);
                    return;
                }
                if (t == "install_snapshot_complete") {
                    uint64_t term = j.value("term", (uint64_t)0);
                    std::string leaderId = j.value("leader", "");
                    uint64_t transferId = j.value("transferId", (uint64_t)0);
                    std::string finalChecksum = j.value("payloadSha256", "");
                    if (leaderId.empty() || transferId == 0 || !isSha256Hex(finalChecksum)) {
                        std::string nack = "install_failed";
                        raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                        return;
                    }

                    uint64_t localTerm = 0;
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        localTerm = currentTerm_;
                        if (term > currentTerm_) {
                            currentTerm_ = term;
                            leader_.store(false);
                            votedFor_.clear();
                            persistCurrentTerm();
                            localTerm = currentTerm_;
                        }
                    }
                    if (term < localTerm) {
                        std::string stale = "stale_term:" + std::to_string(localTerm);
                        raftSocketSend(clientSock, stale.c_str(), (int)stale.size(), 0);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> el(electionMutex_);
                        leaderId_ = leaderId;
                    }
                    lastHeartbeat_.store(steadyNowMs());

                    // Snapshot checksum verification and state-machine installation can
                    // exceed an election timeout. Reuse the existing recovery fence so
                    // this node cannot campaign or serve strong reads mid-install.
                    const bool recoveryWasComplete =
                        recoveryComplete_.exchange(false, std::memory_order_acq_rel);
                    struct SnapshotRecoveryFence {
                        std::atomic<bool>& recoveryComplete;
                        bool restore;
                        ~SnapshotRecoveryFence() {
                            recoveryComplete.store(restore, std::memory_order_release);
                        }
                    } snapshotRecoveryFence{recoveryComplete_, recoveryWasComplete};

                    uint64_t includedIdx = j.value("lastIncludedIndex", (uint64_t)0);
                    uint64_t includedTerm = j.value("lastIncludedTerm", (uint64_t)0);
                    SnapshotChunkTransfer transfer;
                    {
                        std::lock_guard<std::mutex> chunkLock(g_snapshotChunkMu);
                        auto it = g_snapshotChunkTransfers.find(snapshotTransferKey(leaderId, transferId));
                        if (it == g_snapshotChunkTransfers.end() || it->second.totalChunks == 0 ||
                            it->second.nextChunk != it->second.totalChunks ||
                            it->second.payloadSha256 != finalChecksum) {
                            std::string nack = "install_failed";
                            raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                            return;
                        }
                        if (includedIdx == 0) includedIdx = it->second.lastIncludedIndex;
                        if (includedTerm == 0) includedTerm = it->second.lastIncludedTerm;
                        transfer = std::move(it->second);
                        g_snapshotChunkTransfers.erase(it);
                    }

                    const auto rejectTransfer = [&]() {
                        std::error_code removeEc;
                        std::filesystem::remove(transfer.tempPath, removeEc);
                        std::string nack = "install_failed";
                        raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                    };
                    if (includedIdx != transfer.lastIncludedIndex ||
                        includedTerm != transfer.lastIncludedTerm ||
                        pacificdb::durability::ChecksumCalculator::sha256File(transfer.tempPath) !=
                            transfer.payloadSha256) {
                        rejectTransfer();
                        return;
                    }

                    json snap;
                    bool bundledSnapshot = false;
                    try {
                        bundledSnapshot = pacificdb::snapshot_bundle::isBundle(transfer.tempPath);
                        if (bundledSnapshot) {
                            snap = pacificdb::snapshot_bundle::read(transfer.tempPath).manifest;
                        } else {
                            std::ifstream snapshotInput(transfer.tempPath, std::ios::binary);
                            if (!snapshotInput) {
                                throw std::runtime_error("cannot open transferred snapshot");
                            }
                            snapshotInput >> snap;
                        }
                    } catch (...) {
                        rejectTransfer();
                        return;
                    }
                    std::string validationReason;
                    if (!validateTransferredRaftSnapshot(
                            snap, includedIdx, includedTerm, validationReason)) {
                        std::cerr << "[RAFTCORE] Rejected chunked snapshot: "
                                  << validationReason
                                  << std::endl;
                        rejectTransfer();
                        return;
                    }

                    // Snapshot publication and installation share one ordering.
                    // Otherwise an older local capture could overwrite a newer
                    // transferred snapshot after apply was deliberately released.
                    std::lock_guard<std::mutex> snapshotGuard(snapshotMutex_);

                    bool snapshotApplied = false;
                    std::string installedSnapshotPath;
                    {
                        std::lock_guard<std::mutex> applyGuard(applyMutex_);
                        if (includedIdx <= lastApplied_.load(std::memory_order_acquire)) {
                            std::error_code removeEc;
                            std::filesystem::remove(transfer.tempPath, removeEc);
                            snapshotApplied = true;
                        } else {
                            try {
                                std::string base = dataRoot();
                                if (!base.empty() && base.back() != '/' && base.back() != '\\') base += "/";
                                const std::string snapshotPath = base + ".snapshot";
                                durableAtomicReplaceFile(
                                    transfer.tempPath, snapshotPath, "transferred Raft snapshot");
                                persistRaftSnapshotChecksum(snapshotPath, transfer.payloadSha256);
                                installedSnapshotPath = snapshotPath;
                            } catch (const std::exception& e) {
                                std::cerr << "[RAFTCORE] Cannot persist transferred snapshot: "
                                          << e.what() << std::endl;
                                rejectTransfer();
                                return;
                            }
                            try {
                                snapshotApplied = DatabaseEngine::applySnapshot(
                                    snap["lsm_payload"],
                                    bundledSnapshot ? installedSnapshotPath : std::string());
                            } catch (...) {
                                snapshotApplied = false;
                            }
                            if (snapshotApplied) {
                                divergenceDetected_.store(false, std::memory_order_release);
                                if (includedIdx >= lastSnapshotIndex_) lastSnapshotTerm_ = includedTerm;
                                lastSnapshotIndex_ = std::max(lastSnapshotIndex_, includedIdx);
                                lastIndex_ = std::max(lastIndex_, includedIdx);
                                lastEntryTerm_ = std::max(lastEntryTerm_, includedTerm);
                                {
                                    std::lock_guard<std::mutex> el(electionMutex_);
                                    commitIndex_ = std::max<uint64_t>(commitIndex_.load(), includedIdx);
                                }
                                persistAppliedProgress(includedIdx);
                                snapshotApplied = advanceLastApplied(
                                    includedIdx, ApplySource::SNAPSHOT, true);
                                if (snapshotApplied) {
                                    lastAppliedOffset_.store(0);
                                    publishAppliedIndex();
                                    if (includedIdx > 0) compactRaftLog(includedIdx);
                                }
                            }
                        }
                    }

                    if (!snapshotApplied) {
                        std::string nack = "install_failed";
                        raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                        return;
                    }

                    std::string ok = "installed";
                    g_snapshotTransfersCompleted.fetch_add(1, std::memory_order_relaxed);
                    raftSocketSend(clientSock, ok.c_str(), (int)ok.size(), 0);
                    return;
                }
                if (t == "install_snapshot") {
                    // Leader is sending a full snapshot to bring follower up-to-date
                    try {
                        uint64_t term = j.value("term", (uint64_t)0);
                        uint64_t localTerm = 0;
                        {
                            std::lock_guard<std::mutex> el(electionMutex_);
                            localTerm = currentTerm_;
                            if (term >= currentTerm_) {
                                adoptLeaderTermLocked(term);
                                localTerm = currentTerm_;
                            }
                        }
                        if (term < localTerm) {
                            std::string stale = "stale_term:" + std::to_string(localTerm);
                            raftSocketSend(clientSock, stale.c_str(), (int)stale.size(), 0);
                            return;
                        }

                        json snap = j.value("snapshot", json::object());
                        std::string validationReason;
                        if (!validateRaftSnapshot(snap, validationReason)) {
                            std::cerr << "[RAFTCORE] Rejected snapshot: " << validationReason << std::endl;
                            std::string nack = "install_failed";
                            raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                            return;
                        }

                        std::lock_guard<std::mutex> snapshotGuard(snapshotMutex_);

                        const uint64_t included = snap.value("lastIncludedIndex", static_cast<uint64_t>(0));
                        const uint64_t includedTerm = snap.value("lastIncludedTerm", static_cast<uint64_t>(0));
                        bool snapshotApplied = false;
                        {
                            // Keep snapshot apply + state advancement serialized with committed-entry apply.
                            std::lock_guard<std::mutex> applyGuard(applyMutex_);
                            if (included <= lastApplied_.load(std::memory_order_acquire)) {
                                snapshotApplied = true;
                            } else {
                                try {
                                    snapshotApplied = DatabaseEngine::applySnapshot(snap["lsm_payload"]);
                                } catch (...) {
                                    snapshotApplied = false;
                                }
                                if (snapshotApplied) {
                                    snapshotApplied = persistRaftSnapshotFile(dataRoot(), snap);
                                }
                                if (snapshotApplied) {
                                    if (included >= lastSnapshotIndex_) lastSnapshotTerm_ = includedTerm;
                                    lastSnapshotIndex_ = std::max(lastSnapshotIndex_, included);
                                    lastIndex_ = std::max(lastIndex_, included);
                                    lastEntryTerm_ = std::max(lastEntryTerm_, includedTerm);
                                    {
                                        std::lock_guard<std::mutex> el(electionMutex_);
                                        commitIndex_ = std::max<uint64_t>(commitIndex_.load(), included);
                                    }
                                    persistAppliedProgress(included);
                                    snapshotApplied = advanceLastApplied(
                                        included, ApplySource::SNAPSHOT, true);
                                    if (snapshotApplied) {
                                        lastAppliedOffset_.store(0);
                                        publishAppliedIndex();
                                        if (included > 0) compactRaftLog(included);
                                    }
                                }
                            }
                        }

                        if (!snapshotApplied) {
                            std::string nack = "install_failed";
                            raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                            return;
                        }

                        std::string ok = "installed";
                        g_snapshotTransfersCompleted.fetch_add(1, std::memory_order_relaxed);
                        raftSocketSend(clientSock, ok.c_str(), (int)ok.size(), 0);
                        return;
                    } catch (...) {}
                    std::string nack = "install_failed";
                    raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
                    return;
                }
            }

            // Untyped replication has no term, previous-log proof, leader
            // identity, or committed-index boundary. It cannot safely append or
            // apply a state-machine entry, so legacy peers fail closed.
            std::cerr << "[RAFTCORE] Rejected unsafe untyped replication payload" << std::endl;
            std::string nack = "unsupported_legacy_replication_format";
            raftSocketSend(clientSock, nack.c_str(), static_cast<int>(nack.size()), 0);
        } catch (const std::exception& ex) {
            std::cerr << "[RAFTCORE] Failed to apply replicated payload: " << ex.what() << std::endl;
            std::string nack = "no";
            raftSocketSend(clientSock, nack.c_str(), (int)nack.size(), 0);
        }
        };  // v5.5P-R5.6: end serveOne lambda
        serveOne();
        // Busy persistent sessions never hit the idle timeout. Yield after a
        // complete reply when another connection needs a worker (e.g. heartbeat).
        // The per-peer replicator already reconnects and retries closed sessions.
        if (!keepAlive || pendingConnections.load(std::memory_order_relaxed) > 0) break;
      }  // v5.5P-R5.6: end per-connection RPC loop
        // socket closed by _sockGuard (RAII) on return
    } catch (...) {
        std::cerr << "[RAFTCORE] Exception in follower handler" << std::endl;
    }
    // _sockGuard closes clientSock here on all paths.
}

void RaftCore::snapshotLoop() {
    if (waitForShutdown(std::chrono::seconds(10))) return;

    static const std::optional<long long> snapshotIntervalSeconds = []() -> std::optional<long long> {
        if (const char* value = std::getenv("RAFT_SNAPSHOT_INTERVAL_SEC")) {
            try { return std::stoll(value); } catch (...) {}
        }
        return std::nullopt;
    }();
    static const std::optional<uint64_t> snapshotThresholdEntries = []() -> std::optional<uint64_t> {
        if (const char* value = std::getenv("RAFT_SNAPSHOT_THRESHOLD_ENTRIES")) {
            try { return std::stoull(value); } catch (...) {}
        }
        return std::nullopt;
    }();
    if (snapshotIntervalSeconds) snapshotIntervalSec_ = std::chrono::seconds(*snapshotIntervalSeconds);
    if (snapshotThresholdEntries) snapshotThresholdEntries_ = *snapshotThresholdEntries;

    static const uint64_t minSnapshotGap = [] {
        if (const char* value = std::getenv("RAFT_SNAPSHOT_MIN_ENTRIES")) {
            try { return std::max<uint64_t>(1, std::stoull(value)); } catch (...) {}
        }
        return uint64_t{1024};
    }();

    while (running_.load()) {
        // Snapshot every role so follower logs stay bounded, but never run storage
        // export on the heartbeat/election thread: slow disks must not create terms.
        try {
            const uint64_t appliedSnapshot = lastApplied_.load(std::memory_order_acquire);
            const uint64_t reclaimable = (appliedSnapshot > lastSnapshotIndex_)
                ? (appliedSnapshot - lastSnapshotIndex_) : 0;
            const bool intervalElapsed = lastSnapshotTime_.time_since_epoch().count() == 0 ||
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - lastSnapshotTime_).count() >= snapshotIntervalSec_.count();
            const bool needSnapshot = reclaimable >= snapshotThresholdEntries_ ||
                (intervalElapsed && reclaimable >= minSnapshotGap);

            if (needSnapshot && appliedSnapshot > 0) {
                std::lock_guard<std::mutex> snapshotGuard(snapshotMutex_);
                uint64_t stableApplied = 0;
                std::filesystem::path pinnedRoot;
                try {
                    {
                        // Freeze only long enough to drain and pin immutable SSTs.
                        std::unique_lock<std::mutex> applyGuard(applyMutex_);
                        stableApplied = lastApplied_.load(std::memory_order_acquire);
                        const uint64_t stableReclaimable =
                            (stableApplied > lastSnapshotIndex_)
                            ? (stableApplied - lastSnapshotIndex_) : 0;
                        if (stableReclaimable >= minSnapshotGap && stableApplied > 0) {
                            pinnedRoot = LSM::pinSnapshotFiles(stableApplied);
                        }
                    }
                    if (!pinnedRoot.empty() &&
                        createSnapshot(stableApplied, pinnedRoot)) {
                        compactRaftLog(stableApplied);
                    }
                } catch (const std::exception& error) {
                    std::cerr << "[RAFTCORE] Snapshot capture failed: "
                              << error.what() << std::endl;
                }
                LSM::discardSnapshotFiles(pinnedRoot);
            }
        } catch (...) {}

        if (waitForShutdown(std::chrono::milliseconds(500))) break;
    }
}

void RaftCore::monitorLoop() {
    // Wait 10 seconds to allow ALL nodes in cluster to initialize their listeners before ANY election
    // This prevents split-brain during simultaneous cluster startup
    std::cout << "[RAFTCORE] Monitor loop waiting 10 seconds for full cluster initialization..." << std::endl;
    if (waitForShutdown(std::chrono::seconds(10))) return;
    std::cout << "[RAFTCORE] Monitor loop starting with peers=" << peers_.size() << " quorum=" << quorumSize() << std::endl;

    static const std::pair<int, int> electionTimeoutRange = [] {
        int minMs = 3000;
        int maxMs = 6000;
        if (const char* e = std::getenv("RAFT_ELECTION_TIMEOUT_MIN_MS")) { try { minMs = std::stoi(e); } catch (...) {} }
        if (const char* e = std::getenv("RAFT_ELECTION_TIMEOUT_MAX_MS")) { try { maxMs = std::stoi(e); } catch (...) {} }
        if (const char* e = std::getenv("RAFT_ELECTION_TIMEOUT_MS")) { try { int base = std::stoi(e); minMs = base; maxMs = base * 2; } catch (...) {} }
        minMs = std::max(500, minMs);
        if (maxMs <= minMs) maxMs = minMs + 1000;
        return std::make_pair(minMs, maxMs);
    }();
    auto electionTimeoutMs = [this]() {
        std::uniform_int_distribution<int> dist(electionTimeoutRange.first, electionTimeoutRange.second);
        return dist(rng_);
    };

    // v5.5P-R1: after the startup barrier, begin each node's first election
    // deadline from "now" with independent jitter. Previously the startup wait
    // made lastHeartbeat_ stale on every node, so all nodes entered election
    // immediately and synchronized into a term storm.
    uint64_t observedHeartbeatForDeadline = steadyNowMs();
    lastHeartbeat_.store(observedHeartbeatForDeadline);
    uint64_t nextElectionDeadlineMs = observedHeartbeatForDeadline + static_cast<uint64_t>(electionTimeoutMs());

    while (running_.load()) {
        if (leader_.load()) {
            // The election no-op waiter may time out before its durable entry
            // applies. Readiness follows current-term committed apply, not the
            // lifetime of that waiter. Never bless an older term after a step-down.
            if (!leadershipReady_.load(std::memory_order_acquire)) {
                const uint64_t applied = lastApplied_.load(std::memory_order_acquire);
                const uint64_t appliedTerm = applied ? getTermForIndex(applied) : 0;
                std::lock_guard<std::mutex> lk(electionMutex_);
                if (leader_.load() && applied > 0 && applied <= commitIndex_.load()
                    && appliedTerm == currentTerm_) {
                    leadershipReady_.store(true, std::memory_order_release);
                }
            }
            // v5.5P-R1: send heartbeats to all peers IN PARALLEL so one slow/down peer
            // cannot delay heartbeats to healthy peers (sequential sends previously
            // caused followers to time out and trigger needless re-elections / term churn).
            uint64_t hbTerm = currentTerm_;
            uint64_t hbCommit = commitIndex_;
            std::string hbLeader = nodeId_;
            std::vector<std::thread> hbWorkers;
            for (const auto& p : peers_) {
                if (isSelfPeer(p)) continue;
                hbWorkers.emplace_back([this, p, hbTerm, hbCommit, hbLeader]() {
                    try {
                        json hb;
                        hb["type"] = "heartbeat";
                        hb["rpcFormat"] = "json";
                        hb["term"] = hbTerm;
                        hb["leader"] = hbLeader;
	                        hb["leaderId"] = hbLeader;
	                        hb["leaderCommit"] = hbCommit;
	                        hb["leaderReady"] = leadershipReady_.load();
	                        hb["leaderLastLogIndex"] = std::max(lastIndex_, lastSnapshotIndex_);
	                        hb["leaderLastLogTerm"] = getTermForIndex(std::max(lastIndex_, lastSnapshotIndex_));
	                        bool ack = false; int tm = 800;
                        std::string resp;
                        bool ok = sendToPeer(p, hb, tm, ack, &resp);
                        uint64_t remoteTerm = 0;
                        if (parseStaleTermResponse(resp, remoteTerm)) {
                            observeHigherTerm(remoteTerm);
                        } else if (ok && ack) {
                            uint64_t peerMatch = 0;
                            try {
                                auto jr = json::parse(resp);
                                peerMatch = jr.value("matchIndex", static_cast<uint64_t>(0));
                            } catch (...) {}
                            if (peerMatch > 0) {
                                std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                                matchIndex_[p] = std::max(matchIndex_[p], peerMatch);
                                nextIndex_[p] = std::max(nextIndex_[p], peerMatch + 1);
                            }
                            {
                                std::lock_guard<std::mutex> hbLk(peerHeartbeatMutex_);
                                peerLastHeartbeatMs_[p] = steadyNowMs();
                            }
                            if (peerMatch < hbCommit) {
                                // Keep heartbeats crisp. A lagging follower may need a multi-RPC
                                // catch-up, but doing that work inline here can block the leader
                                // monitor loop long enough for healthy followers to miss heartbeats
                                // and start elections. Schedule at most one catch-up per peer and
                                // let the next heartbeat continue immediately.
                                auto active = raftHeartbeatBackfillFlag(p);
                                bool expected = false;
                                if (active->compare_exchange_strong(expected, true)) {
                                    std::thread([this, p, peerMatch, hbCommit, hbTerm, active]() {
                                        try {
                                            backfillCommittedEntriesToPeer(p, peerMatch, hbCommit, hbTerm, 1000);
                                        } catch (...) {}
                                        active->store(false);
                                    }).detach();
                                }
                            }
                        }
                    } catch (...) {}
                });
            }
            for (auto& w : hbWorkers) if (w.joinable()) w.join();
            // v5.5P-R1: heartbeat interval (default 500ms) must be well below the
            // election timeout (3000-6000ms) to keep followers from electing.
            static const int hbIntervalMs = [] {
                if (const char* value = std::getenv("RAFT_HEARTBEAT_INTERVAL_MS")) {
                    try { return std::max(50, std::stoi(value)); } catch (...) {}
                }
                return 500;
            }();
            if (waitForShutdown(std::chrono::milliseconds(hbIntervalMs))) break;
        } else {
            // follower: check last heartbeat, run election if the persisted randomized
            // deadline expires. Deadline resets when a valid heartbeat arrives.
            uint64_t now = steadyNowMs();
            uint64_t last = lastHeartbeat_.load();
            if (last != observedHeartbeatForDeadline) {
                observedHeartbeatForDeadline = last;
                nextElectionDeadlineMs = (last ? last : now) + static_cast<uint64_t>(electionTimeoutMs());
                RAFT_CT("election_deadline_reset node=" << nodeId_
                        << " now=" << now << " last=" << last
                        << " deadline=" << nextElectionDeadlineMs
                        << " leaderId=" << leaderId_);
            }
            if (last == 0 || now >= nextElectionDeadlineMs) {
                RAFT_CT("election_deadline_fire node=" << nodeId_
                        << " now=" << now << " last=" << last
                        << " deadline=" << nextElectionDeadlineMs
                        << " leaderId=" << leaderId_);
                if (!leaderEligible_) {
                    nextElectionDeadlineMs = now + static_cast<uint64_t>(electionTimeoutMs());
                    if (waitForShutdown(std::chrono::milliseconds(200))) break;
                    continue;
                }
                bool won = attemptElection();
                if (won) {
                    std::cout << "[RAFTCORE] Won election, becoming leader" << std::endl;
                    {
                        std::lock_guard<std::mutex> lk(replicaIndexMutex_);
                        const uint64_t startNext = lastIndex_ + 1;
                        for (const auto& peer : peers_) {
                            if (isSelfPeer(peer)) continue;
                            nextIndex_[peer] = startNext;
                            matchIndex_[peer] = 0;
                        }
                    }
                    leadershipReady_.store(false);
                    leader_.store(true);
                    MetricsExporter::incrementCounter("pacificdb_leader_elections_total", 1.0);
                    MetricsExporter::recordCustomMetric(
                        "pacificdb_last_leader_election_unix_ms",
                        static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count())
                    );

                    // Raft best practice: commit a no-op in the new term so prior committed
                    // entries become visible consistently after leadership changes.
                    try {
                        // V8.5.8: send an immediate election-stabilizing heartbeat before
                        // doing the synchronous no-op commit. Without this, the new leader
                        // can spend the first ~1s in replicateSynchronous(); on small/loaded
                        // follower nodes that window was enough for another candidate to
                        // advance the term, making the no-op fail and causing a B/C term
                        // ping-pong with no stable leader.
                        {
                            uint64_t hbTerm = getCurrentTerm();
                            uint64_t hbCommit = getCommitIndex();
                            std::string hbLeader = nodeId_;
                            std::atomic<int> heartbeatAcks{1}; // self
                            std::vector<std::thread> immediateHb;
                            for (const auto& p : peers_) {
                                if (isSelfPeer(p)) continue;
                                immediateHb.emplace_back([this, p, hbTerm, hbCommit, hbLeader, &heartbeatAcks]() {
                                    try {
                                        json hb;
                                        hb["type"] = "heartbeat";
                                        hb["rpcFormat"] = "json";
                                        hb["term"] = hbTerm;
                                        hb["leader"] = hbLeader;
	                                        hb["leaderId"] = hbLeader;
	                                        hb["leaderCommit"] = hbCommit;
	                                        hb["leaderReady"] = false;
	                                        hb["leaderLastLogIndex"] = std::max(lastIndex_, lastSnapshotIndex_);
	                                        hb["leaderLastLogTerm"] = getTermForIndex(std::max(lastIndex_, lastSnapshotIndex_));
	                                        bool ack = false;
                                        std::string resp;
                                        bool ok = sendToPeer(p, hb, 800, ack, &resp);
                                        uint64_t remoteTerm = 0;
                                        if (parseStaleTermResponse(resp, remoteTerm)) {
                                            observeHigherTerm(remoteTerm);
                                        } else if (ok && ack) {
                                            heartbeatAcks.fetch_add(1, std::memory_order_relaxed);
                                            try {
                                                auto jr = json::parse(resp);
                                                uint64_t peerMatch = jr.value("matchIndex", static_cast<uint64_t>(0));
                                                if (peerMatch > 0) {
                                                    std::lock_guard<std::mutex> rlk(replicaIndexMutex_);
                                                    matchIndex_[p] = std::max(matchIndex_[p], peerMatch);
                                                    nextIndex_[p] = std::max(nextIndex_[p], peerMatch + 1);
                                                }
                                            } catch (...) {}
                                            std::lock_guard<std::mutex> hbLk(peerHeartbeatMutex_);
                                            peerLastHeartbeatMs_[p] = steadyNowMs();
                                        }
                                    } catch (...) {}
                                });
                            }
                            for (auto& w : immediateHb) if (w.joinable()) w.join();
                            RAFT_CT("post_election_heartbeat node=" << nodeId_
                                    << " term=" << hbTerm
                                    << " acks=" << heartbeatAcks.load()
                                    << " needed=" << quorumSize());
                        }
                        json noop;
                        noop["type"] = "raft_noop";
                        noop["leader"] = nodeId_;
                        noop["elected_term"] = getCurrentTerm();
                        static const int noopTimeoutMs = [] {
                            if (const char* value = std::getenv("RAFT_LEADER_NOOP_TIMEOUT_MS")) {
                                try { return std::max(500, std::stoi(value)); } catch (...) {}
                            }
                            return 3000;
                        }();
                        bool noopCommitted = replicateSynchronous(noop, noopTimeoutMs);
                        if (!noopCommitted) {
                            std::cerr << "[RAFTCORE] Warning: leader no-op commit failed after election" << std::endl;
                        }
                    } catch (...) {
                        std::cerr << "[RAFTCORE] Warning: leader no-op commit threw exception" << std::endl;
                    }
                } else {
                    // Unsynchronized candidate retry; do not hammer terms at a fixed 500ms cadence.
                    nextElectionDeadlineMs = steadyNowMs() + static_cast<uint64_t>(electionTimeoutMs());
                    if (waitForShutdown(std::chrono::milliseconds(200))) break;
                }
            } else {
                if (waitForShutdown(std::chrono::milliseconds(200))) break;
            }
        }
    }
}

bool RaftCore::attemptElection() {
    // v5.5P-R1 FIX: do NOT hold electionMutex_ during peer network I/O. Previously
    // this function held the lock across all blocking sendToPeer() calls, while the
    // listener's vote_request handler ALSO needs electionMutex_ to respond. With
    // simultaneous elections every candidate held its own lock doing I/O, so no
    // listener could grab the lock to answer — every vote RPC timed out (r=-1),
    // causing perpetual election storms and split brain. We now snapshot candidate
    // state under the lock, release it for all RPCs, then re-acquire to finalize.
    uint64_t electionTerm;
    std::string myId;
    uint64_t myLastIndex, myLastTerm;
    // V11.4-DIV-001 P0.6: a node whose state restore is incomplete or failed has an
    // untrustworthy (possibly empty) view of its own log and must not seek
    // leadership. Fail closed instead of campaigning with lastIndex_=0.
    if (!recoveryComplete_.load(std::memory_order_acquire)) {
        std::cerr << "[RAFTCORE] Election refused: recovery incomplete on " << nodeId_ << std::endl;
        return false;
    }

    std::vector<std::string> peersCopy;
    int needed;
    {
        std::lock_guard<std::mutex> el(electionMutex_);
        ++currentTerm_;
        persistCurrentTerm();
        votedFor_ = nodeId_;          // vote for self in the new term
        persistVotedFor();
        leader_.store(false);
        electionTerm = currentTerm_;
        myId = nodeId_;
        myLastIndex = lastIndex_;
        myLastTerm = lastEntryTerm_;
        peersCopy = peers_;
        needed = quorumSize();
    }

    std::atomic<int> votes{1}; // self
    std::atomic<bool> contactedAnyPeer{false};
    std::cout << "[RAFTCORE] ==== ELECTION ATTEMPT ==== Term=" << electionTerm
              << " NodeID=" << myId << " Peers=" << peersCopy.size() << " Needed=" << needed << std::endl;

    // Contact peers concurrently. A sequential RequestVote pass lets one
    // unreachable peer consume the election jitter window, so the healthy
    // candidates self-vote before they can hear from each other and livelock.
    std::vector<std::thread> voteWorkers;
    for (const auto& p : peersCopy) {
        if (isSelfPeer(p)) continue;
        voteWorkers.emplace_back([this, p, electionTerm, myId, myLastIndex,
                                  myLastTerm, &votes, &contactedAnyPeer]() {
        try {
            json req;
            req["type"] = "vote_request";
            // V11.4-DIV-001 P0.6: request the structured response so the grant can
            // be correlated to this exact election term (legacy "vote_granted"
            // text carries no term and could be a stale buffered response).
            req["rpcFormat"] = "json";
            req["term"] = electionTerm;
            req["candidate"] = myId;
            req["lastLogIndex"] = myLastIndex;
            req["lastLogTerm"] = myLastTerm;
            bool ack = false; int tm = 1500;
            std::string resp;
            bool ok = sendToPeer(p, req, tm, ack, &resp);   // NO LOCK HELD
            if (ok) {
                contactedAnyPeer.store(true, std::memory_order_relaxed);
                uint64_t remoteTerm = 0;
                if (parseStaleTermResponse(resp, remoteTerm) && remoteTerm > electionTerm) {
                    observeHigherTerm(remoteTerm); // step down; election lost
                    std::cout << "[RAFTCORE] ✗ Higher term " << remoteTerm << " from " << p << "; aborting election" << std::endl;
                    return;
                }
                // V11.4-DIV-001 P0.6: count only an explicit vote grant for THIS
                // election. sendToPeer's generic `ack` accepts any response
                // containing "ok"/success (e.g. a buffered appendEntries ack on a
                // reused connection) and previously manufactured phantom votes.
                (void)ack;
                bool granted = false;
                {
                    std::string trimmed = resp;
                    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                                                trimmed.back() == ' '))
                        trimmed.pop_back();
                    try {
                        auto jr = json::parse(trimmed);
                        granted = jr.value("type", "") == "requestVoteResponse" &&
                                  jr.value("voteGranted", false) &&
                                  jr.value("term", (uint64_t)0) == electionTerm;
                    } catch (...) { granted = false; }
                }
                if (granted) { votes.fetch_add(1, std::memory_order_relaxed); std::cout << "[RAFTCORE] ✓ vote from " << p << std::endl; }
                else std::cout << "[RAFTCORE] ✗ vote denied: " << p << std::endl;
            } else {
                std::cout << "[RAFTCORE] ✗ no contact: " << p << std::endl;
            }
        } catch (...) {
            std::cout << "[RAFTCORE] ✗ exception contacting peer" << std::endl;
        }
        });
    }
    for (auto& worker : voteWorkers) if (worker.joinable()) worker.join();

    const int voteCount = votes.load(std::memory_order_relaxed);
    const bool contacted = contactedAnyPeer.load(std::memory_order_relaxed);

    if (contacted && firstSuccessfulPeerContact_.load() == 0) {
        firstSuccessfulPeerContact_.store((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Finalize under lock: only win if our term is still current (no higher term
    // observed and we did not step down for another leader during the RPCs).
    {
        std::lock_guard<std::mutex> el(electionMutex_);
        if (currentTerm_ != electionTerm || votedFor_ != myId) {
            std::cout << "[RAFTCORE] Election aborted: term moved " << electionTerm << "->" << currentTerm_ << std::endl;
            return false;
        }
        if (voteCount >= needed) leaderId_ = myId;
    }

    bool won = voteCount >= needed;
    std::cout << "[RAFTCORE] Election result: votes=" << voteCount << " needed=" << needed
              << " WON=" << (won ? "YES" : "NO") << " contactedPeers=" << contacted << std::endl;
    if (won && !contacted && clusterNodeCount() > 1) {
        std::cout << "[RAFTCORE] WARNING: isolated node; rejecting leadership for safety." << std::endl;
        return false;
    }
    return won;
}

// Health monitoring loop
void RaftCore::healthMonitorLoop() {
    while (running_.load()) {
        updateHealthMetrics();
        checkSystemHealth();

        if (waitForShutdown(std::chrono::seconds(5))) break;
    }
}

// Update system health metrics
void RaftCore::updateHealthMetrics() {
    std::lock_guard<std::mutex> lk(healthMutex_);

    // CPU usage (simplified - in real production system, use platform-specific APIs)
    health_.cpuUsage = 0.0; // Placeholder - would use GetSystemTimes on Windows

    // Memory usage
    health_.memoryUsage = 0.0; // Placeholder - would use GlobalMemoryStatusEx

    // Queue depths
    health_.queueDepth = getQueueDepth();
    health_.criticalQueueDepth = criticalQueue_.size();
    health_.highQueueDepth = highQueue_.size();
    health_.normalQueueDepth = normalQueue_.size();
    health_.lowQueueDepth = lowQueue_.size();
    health_.bulkQueueDepth = bulkQueue_.size();

    // Active workers
    health_.activeWorkers = activeWorkers_.load();

    // Request rates (per second)
    //
    // V8.5.5: use interval deltas, not process-lifetime counters. The previous
    // calculation divided cumulative failedRequests_ by the most recent sample
    // interval after resetting lastMetricsUpdate_, so one overload run poisoned
    // health forever (e.g. 50K old failures became 50K errors/sec every tick).
    // That made quiet single-write/schema probes fail with "System unhealthy"
    // even when queue depth, quorum, and apply lag were healthy.
    static size_t lastTotalRequestsSample = 0;
    static size_t lastFailedRequestsSample = 0;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastMetricsUpdate_);
    if (elapsed.count() > 0) {
        const size_t totalNow = totalRequests_.load(std::memory_order_relaxed);
        const size_t failures = failedRequests_.load(std::memory_order_relaxed);
        const size_t healthRejections = healthRejectedRequests_.load(std::memory_order_relaxed);
        // Keep every rejection in client/lifecycle failure accounting, but do
        // not let this gate's own rejections renew its error-rate alarm forever.
        const size_t failedNow = failures > healthRejections ? failures - healthRejections : 0;
        const size_t totalDelta = totalNow >= lastTotalRequestsSample ? (totalNow - lastTotalRequestsSample) : totalNow;
        const size_t failedDelta = failedNow >= lastFailedRequestsSample ? (failedNow - lastFailedRequestsSample) : failedNow;
        health_.requestsPerSecond = static_cast<double>(totalDelta) / static_cast<double>(elapsed.count());
        health_.errorsPerSecond = static_cast<double>(failedDelta) / static_cast<double>(elapsed.count());
        lastTotalRequestsSample = totalNow;
        lastFailedRequestsSample = failedNow;
    }

    // Circuit breaker state
    health_.circuitBreakerState = circuitState_.load();

    // Replication mode
    health_.currentReplicationMode = replicationMode_.load();

    // System health score (0-100)
    health_.healthScore = calculateHealthScore();

    lastMetricsUpdate_ = now;
}

// Calculate overall health score
int RaftCore::calculateHealthScore() {
    int score = 100;

    // Penalize high queue depth
    if (health_.queueDepth > maxQueueDepth_.load() * 0.8) {
        score -= 20;
    } else if (health_.queueDepth > maxQueueDepth_.load() * 0.5) {
        score -= 10;
    }

    // Penalize high error rate
    if (health_.errorsPerSecond > 10) {
        score -= 30;
    } else if (health_.errorsPerSecond > 5) {
        score -= 15;
    }

    // Penalize circuit breaker open
    if (circuitState_.load() == CircuitState::OPEN) {
        score -= 50;
    } else if (circuitState_.load() == CircuitState::HALF_OPEN) {
        score -= 25;
    }

    // Penalize low worker utilization
    if (activeWorkers_.load() < workerThreads_.size() * 0.5) {
        score -= 10;
    }

    return std::max(0, score);
}

// Check system health and trigger actions
bool RaftCore::checkSystemHealth() {
    std::lock_guard<std::mutex> lk(healthMutex_);

    bool healthy = true;

    // Check queue depth thresholds
    if (health_.queueDepth > maxQueueDepth_.load()) {
        std::cerr << "[RAFTCORE] Queue depth exceeded threshold: " << health_.queueDepth << std::endl;
        healthy = false;
    }

    // Check error rate
    if (health_.errorsPerSecond > errorRateThreshold_.load()) {
        std::cerr << "[RAFTCORE] Error rate exceeded threshold: " << health_.errorsPerSecond << std::endl;
        healthy = false;
    }

    // activeWorkers_ tracks currently busy workers, not live worker threads.
    // A quiet Raft group should have zero active workers between writes; treating
    // that as worker death makes the first post-idle quorum write fail.
    if (running_.load() && workerThreads_.empty()) {
        std::cerr << "[RAFTCORE] Worker pool missing - restarting" << std::endl;
        restartWorkers();
        healthy = false;
    }

    // Update health status
    health_.isHealthy = healthy;
    return healthy;
}

// Adaptive control loop
void RaftCore::adaptiveControllerLoop() {
    while (running_.load()) {
        adaptReplicationMode();
        adaptWorkerPool();
        adaptCircuitBreaker();

        if (waitForShutdown(std::chrono::seconds(10))) break;
    }
}

// Dynamic replication mode adaptation
void RaftCore::adaptReplicationMode() {
    if (replicationMode_.load() != ReplicationMode::HYBRID_ADAPTIVE) {
        return; // Only adapt if in adaptive mode
    }

    std::lock_guard<std::mutex> lk(healthMutex_);

    // High load conditions - force more synchronous
    if (health_.queueDepth > maxQueueDepth_.load() * 0.7 ||
        health_.errorsPerSecond > errorRateThreshold_.load() * 0.5 ||
        health_.healthScore < 70) {

        if (adaptiveMode_ != ReplicationMode::SYNCHRONOUS) {
            std::cout << "[RAFTCORE] Adapting to SYNCHRONOUS mode due to high load" << std::endl;
            adaptiveMode_ = ReplicationMode::SYNCHRONOUS;
        }
    }
    // Normal conditions - allow asynchronous
    else if (health_.queueDepth < maxQueueDepth_.load() * 0.3 &&
             health_.errorsPerSecond < errorRateThreshold_.load() * 0.2 &&
             health_.healthScore > 85) {

        if (adaptiveMode_ != ReplicationMode::ASYNCHRONOUS) {
            std::cout << "[RAFTCORE] Adapting to ASYNCHRONOUS mode for performance" << std::endl;
            adaptiveMode_ = ReplicationMode::ASYNCHRONOUS;
        }
    }
}

// Dynamic worker pool adaptation
void RaftCore::adaptWorkerPool() {
    std::lock_guard<std::mutex> lk(healthMutex_);

    size_t currentWorkers = workerThreads_.size();
    size_t targetWorkers = currentWorkers; // Don't shrink by default

    // Scale up aggressively under any queue pressure
    if (health_.queueDepth > 10) {
        targetWorkers = std::min(maxWorkers_.load(), currentWorkers + 8);
    } else if (health_.queueDepth > maxQueueDepth_.load() * 0.3) {
        targetWorkers = std::min(maxWorkers_.load(), currentWorkers + 4);
    }
    // Only scale down when truly idle for a while (never below minWorkers)
    else if (health_.queueDepth == 0 &&
             health_.activeWorkers < currentWorkers * 0.25 &&
             currentWorkers > minWorkers_.load()) {
        targetWorkers = std::max(minWorkers_.load(), currentWorkers - 1);
    }

    if (targetWorkers != currentWorkers) {
        std::cout << "[RAFTCORE] Adapting worker pool: " << currentWorkers << " -> " << targetWorkers << std::endl;
        resizeWorkerPool(targetWorkers);
    }
}

// Circuit breaker logic
void RaftCore::handleCircuitBreaker() {
    if (consecutiveFailures_ >= circuitBreakerThreshold_.load()) {
        if (circuitState_.load() == CircuitState::CLOSED) {
            std::cout << "[RAFTCORE] Circuit breaker OPEN due to " << consecutiveFailures_ << " failures" << std::endl;
            circuitState_.store(CircuitState::OPEN);
            lastFailureTime_ = std::chrono::steady_clock::now();
        }
    } else {
        consecutiveFailures_ = 0;
    }
}

void RaftCore::adaptCircuitBreaker() {
    auto now = std::chrono::steady_clock::now();
    auto timeSinceFailure = std::chrono::duration_cast<std::chrono::seconds>(
        now - lastFailureTime_);

    // Half-open after timeout
    if (circuitState_.load() == CircuitState::OPEN &&
        timeSinceFailure >= circuitRecoveryTimeout_) {

        std::cout << "[RAFTCORE] Circuit breaker HALF_OPEN - testing recovery" << std::endl;
        circuitState_.store(CircuitState::HALF_OPEN);
        consecutiveFailures_ = 0;
    }

    // Close if successful in half-open

    if (circuitState_.load() == CircuitState::HALF_OPEN &&
        consecutiveFailures_ == 0 &&
        timeSinceFailure >= std::chrono::seconds(successThreshold_.load())) {

        std::cout << "[RAFTCORE] Circuit breaker CLOSED - recovered" << std::endl;
        circuitState_.store(CircuitState::CLOSED);
    }
}

// Worker pool management
void RaftCore::resizeWorkerPool(size_t newSize) {
    size_t currentSize = workerThreads_.size();

    if (newSize > currentSize) {
        // Add workers
        for (size_t i = currentSize; i < newSize; ++i) {
            workerThreads_.emplace_back(&RaftCore::workerLoop, this);
        }
    } else if (newSize < currentSize) {
        // Remove workers (they will exit naturally)
        // Note: In production, you'd want more sophisticated worker management
    }
}

void RaftCore::restartWorkers() {
    // v5.5P-R2B CRITICAL FIX: do NOT toggle the global running_ here. running_ also
    // governs the raft LISTENER, the monitorLoop (heartbeats/elections), and the
    // health loop — none of which are recreated by this function. The previous
    // implementation set running_=false to recycle worker threads, which
    // PERMANENTLY killed the raft listener ("Listener exiting on port ...") the
    // first time the health check fired (often a false positive when workers are
    // merely idle), leaving the node unable to receive any Raft RPC. We now only
    // (non-destructively) top up the worker pool, leaving running_ untouched.
    std::cout << "[RAFTCORE] Ensuring worker pool (non-destructive)" << std::endl;
    if (!running_.load()) return; // node is genuinely shutting down
    size_t target = std::max(minWorkers_.load(), static_cast<size_t>(4));
    resizeWorkerPool(target);
    queueCV_.notify_all();
}

// Load detection
// (duplicate below earlier — removed duplicate to avoid redefinition)

// Metrics logging
void RaftCore::logOperationMetrics(const json& entry, bool success, std::chrono::milliseconds duration) {
    totalRequests_++;
    if (!success) failedRequests_++;

    // Log to metrics file
    std::string base = dataRoot();
    if (base.back() != '/' && base.back() != '\\') base += "/";
    std::string metricsPath = base + "raft/metrics.csv";

    std::ofstream out(metricsPath, std::ios::app);
    if (out.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        std::string action = entry.value("action", entry.value("op", "unknown"));
        out << epochMs << ","
            << action << ","
            << success << ","
            << duration.count() << ","
            << health_.queueDepth << ","
            << health_.healthScore << ","
            << entry.value("requestId", std::string("")) << ","
            << entry.value("trace_id", std::string("")) << ","
            << entry.value("consistency", std::string("")) << std::endl;
    }
}

// Helper: data root for storage
std::string RaftCore::dataRoot() {
    static const std::string resolved = [] {
        const char* dataDir = std::getenv("DATA_DIR");
        const char* env = std::getenv("DATA_ROOT");
        if (!env || !*env) {
            throw std::runtime_error("DATA_ROOT is required; refusing cwd-relative Raft storage");
        }
        const std::filesystem::path root(env);
        if (!root.is_absolute()) {
            throw std::runtime_error("DATA_ROOT must be absolute; refusing cwd-relative Raft storage");
        }
        std::error_code ec;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(root, ec);
        const std::string value = (ec ? root.lexically_normal() : canonical).string();
        if (dataDir && *dataDir) {
            const std::filesystem::path alias(dataDir);
            if (!alias.is_absolute()) throw std::runtime_error("DATA_DIR must be absolute");
            std::error_code aliasEc;
            const auto canonicalAlias = std::filesystem::weakly_canonical(alias, aliasEc);
            const std::string aliasResolved = (aliasEc ? alias.lexically_normal() : canonicalAlias).string();
            if (aliasResolved != value) {
                throw std::runtime_error("DATA_DIR conflicts with canonical DATA_ROOT");
            }
        }
        return value;
    }();
    return resolved;
}
