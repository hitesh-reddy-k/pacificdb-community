#include "wal.hpp"
#include "database_engine.hpp"
#include "raft_core.hpp"
#include "request_timing.hpp"
#include "data_durability.hpp"
#include "wal_integrity.hpp"

#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>

static WalStats g_walStats;
static std::atomic<uint64_t> g_walSeq{0};

#ifdef _WIN32
#include <windows.h>
#undef DELETE
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static bool isTruthyEnv(const char* v) {
    if (!v) return false;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on";
}

static const char* firstEnv(const char* primary, const char* fallback = nullptr, const char* third = nullptr) {
    const char* v = std::getenv(primary);
    if (v && *v) return v;
    if (fallback) {
        v = std::getenv(fallback);
        if (v && *v) return v;
    }
    if (third) {
        v = std::getenv(third);
        if (v && *v) return v;
    }
    return nullptr;
}

static bool boolEnvAny(const char* primary, const char* fallback, bool defaultValue) {
    const char* v = firstEnv(primary, fallback);
    if (!v) return defaultValue;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

static int intEnvAny(const char* primary, const char* fallback, const char* third, int defaultValue) {
    const char* v = firstEnv(primary, fallback, third);
    if (!v) return defaultValue;
    try { return std::stoi(v); } catch (...) { return defaultValue; }
}

static uint64_t uint64Env(const char* key, uint64_t defaultValue) {
    const char* value = std::getenv(key);
    if (!value || !*value) return defaultValue;
    try { return std::stoull(value); } catch (...) { return defaultValue; }
}

static bool inMemoryOltpModeEnabled() {
    return isTruthyEnv(std::getenv("ENGINE_IN_MEMORY_OLTP")) ||
           isTruthyEnv(std::getenv("IN_MEMORY_OLTP"));
}

static bool walBinaryEncodingEnabled() {
    const char* v = std::getenv("WAL_BINARY_ENCODING");
    if (!v) return true;
    std::string s(v);
    if (s == "0" || s == "false" || s == "FALSE" || s == "off") return false;
    return true;
}

static bool walCrcEnabled() {
    const char* v = std::getenv("WAL_CRC_ENABLED");
    if (!v) return true;
    std::string s(v);
    return !(s == "0" || s == "false" || s == "FALSE" || s == "off");
}

static uint64_t stampWalIntegrityFields(nlohmann::json& entry, uint64_t lsn) {
    if (!entry.is_object()) return 0;
    entry["_wal_seq"] = lsn;
    entry["_wal_lsn"] = lsn;
    entry["_wal_ts_ms"] = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (!walCrcEnabled()) return lsn;
    entry["_wal_crc_encoding"] = "msgpack-v1";
    nlohmann::json forCrc = entry;
    forCrc.erase("_wal_crc32");
    const auto packed = nlohmann::json::to_msgpack(forCrc);
    const uint32_t crc = pacificdb::durability::ChecksumCalculator::crc32(
        std::string(reinterpret_cast<const char*>(packed.data()), packed.size()));
    entry["_wal_crc32"] = crc;
    return lsn;
}

static bool verifyWalPayloadCrc(const nlohmann::json& entry) {
    if (!entry.is_object() || !entry.contains("_wal_crc32")) return entry.is_object();
    try {
        const auto& value = entry["_wal_crc32"];
        if (!value.is_number_unsigned() && !value.is_number_integer()) return false;
        nlohmann::json forCrc = entry;
        forCrc.erase("_wal_crc32");
        if (forCrc.value("_wal_crc_encoding", "") == "msgpack-v1") {
            const auto packed = nlohmann::json::to_msgpack(forCrc);
            return value.get<uint32_t>() ==
                   pacificdb::durability::ChecksumCalculator::crc32(
                       std::string(reinterpret_cast<const char*>(packed.data()), packed.size()));
        }
        return value.get<uint32_t>() ==
               pacificdb::durability::ChecksumCalculator::crc32(forCrc.dump());
    } catch (...) {
        return false;
    }
}

static std::string encodeWalPayload(const nlohmann::json& entry) {
    if (walBinaryEncodingEnabled()) {
        auto packed = nlohmann::json::to_msgpack(entry);
        return std::string(reinterpret_cast<const char*>(packed.data()), packed.size());
    }
    return entry.dump();
}

static bool looksLikeJsonText(const std::string& payload) {
    for (unsigned char ch : payload) {
        if (std::isspace(ch)) continue;
        return ch == '{' || ch == '[' || ch == '"';
    }
    return false;
}

static bool decodeWalPayload(const std::string& payload, nlohmann::json& out) {
    try {
        if (looksLikeJsonText(payload)) {
            out = nlohmann::json::parse(payload);
            return true;
        }
        std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
        out = nlohmann::json::from_msgpack(bytes);
        return true;
    } catch (...) {
        try {
            out = nlohmann::json::parse(payload);
            return true;
        } catch (...) {
            return false;
        }
    }
}

// Durable by default; in-memory mode remains the only implicit opt-out.
static bool walFsyncEnabled() {
    const char* v = firstEnv("WAL_FSYNC_ENABLED", "WAL_FSYNC");
    if (!v) {
        if (inMemoryOltpModeEnabled()) return false;
        return true;
    }
    std::string s(v);
    return (s == "true" || s == "1");
}

namespace {
    struct WalCompletion {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
        bool success{false};
        std::string error;
        WalAppendResult result;
    };

    struct PendingWalEntry {
        WalOp op;
        nlohmann::json entry;
        std::shared_ptr<WalCompletion> completion;
    };

    std::unordered_map<std::string, std::vector<PendingWalEntry>> buffers;
    std::mutex bufMutex;
    std::condition_variable bufCv;
    bool flusherRunning = false;
    std::thread flusherThread;

    struct WalFileHandle {
        std::mutex mutex;
#ifdef _WIN32
        HANDLE native = INVALID_HANDLE_VALUE;
        ~WalFileHandle() { if (native != INVALID_HANDLE_VALUE) CloseHandle(native); }
#else
        int native = -1;
        ~WalFileHandle() { if (native >= 0) close(native); }
#endif
    };

    std::mutex walFilesMutex;
    std::unordered_map<std::string, std::shared_ptr<WalFileHandle>> walFiles;

    std::shared_ptr<WalFileHandle> walFileHandle(const std::string& file) {
        std::lock_guard<std::mutex> lock(walFilesMutex);
        auto& handle = walFiles[file];
        if (!handle) handle = std::make_shared<WalFileHandle>();
        return handle;
    }

    bool openWalFile(const std::string& file, WalFileHandle& handle, bool create) {
#ifdef _WIN32
        if (handle.native != INVALID_HANDLE_VALUE) return true;
        handle.native = CreateFileA(
            file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle.native != INVALID_HANDLE_VALUE;
#else
        if (handle.native >= 0) return true;
        const bool existed = std::filesystem::exists(file);
        handle.native = open(
            file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
        if (handle.native < 0) return false;
        if (create && !existed) {
            const std::string directory = std::filesystem::path(file).parent_path().string();
            const int dirFd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dirFd >= 0) { fsync(dirFd); close(dirFd); }
        }
        return true;
#endif
    }

    bool appendWalBytes(const std::string& file, const std::string& bytes) {
        auto handle = walFileHandle(file);
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!openWalFile(file, *handle, true)) return false;
#ifdef _WIN32
        size_t written = 0;
        while (written < bytes.size()) {
            DWORD count = 0;
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(bytes.size() - written, std::numeric_limits<DWORD>::max()));
            if (!WriteFile(handle->native, bytes.data() + written, chunk, &count, nullptr) || count == 0) {
                return false;
            }
            written += count;
        }
        return true;
#else
        size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = write(handle->native, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            written += static_cast<size_t>(count);
        }
        return true;
#endif
    }

    bool syncWalFile(const std::string& file) {
        auto handle = walFileHandle(file);
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (!openWalFile(file, *handle, false)) return false;
#ifdef _WIN32
        return FlushFileBuffers(handle->native);
#else
        return fdatasync(handle->native) == 0;
#endif
    }

    void closeWalFile(const std::string& file) {
        std::shared_ptr<WalFileHandle> handle;
        {
            std::lock_guard<std::mutex> lock(walFilesMutex);
            auto found = walFiles.find(file);
            if (found == walFiles.end()) return;
            handle = std::move(found->second);
            walFiles.erase(found);
        }
        std::lock_guard<std::mutex> lock(handle->mutex);
#ifdef _WIN32
        if (handle->native != INVALID_HANDLE_VALUE) CloseHandle(handle->native);
        handle->native = INVALID_HANDLE_VALUE;
#else
        if (handle->native >= 0) close(handle->native);
        handle->native = -1;
#endif
    }

    void closeWalFiles() {
        std::vector<std::string> files;
        {
            std::lock_guard<std::mutex> lock(walFilesMutex);
            files.reserve(walFiles.size());
            for (const auto& [file, handle] : walFiles) { (void)handle; files.push_back(file); }
        }
        for (const auto& file : files) closeWalFile(file);
    }

    constexpr size_t kSegmentHeaderBytes = 32;
    constexpr uint8_t kSegmentVersion = 3;

    struct ScannedWalFile {
        std::filesystem::path path;
        bool legacy{false};
        uint64_t segmentId{0};
        uint64_t firstLsn{0};
        uint64_t endLsn{0};
        uint64_t validBytes{0};
        uint64_t identityHash{0};
    };

    struct InternalScanResult {
        WalScanResult publicResult;
        std::vector<ScannedWalFile> files;
    };

    struct LogicalWalState {
        std::mutex mutex;
        bool clearing{false};
        bool initialized{false};
        bool poisoned{false};
        std::string error;
        uint64_t nextLsn{1};
        uint64_t activeSegment{0};
        uint64_t activeBytes{0};
        uint64_t identityHash{0};
    };

    std::mutex logicalWalStatesMutex;
    std::unordered_map<std::string, std::shared_ptr<LogicalWalState>> logicalWalStates;

    std::shared_ptr<LogicalWalState> logicalWalState(const std::string& logicalWal) {
        std::lock_guard<std::mutex> lock(logicalWalStatesMutex);
        auto& state = logicalWalStates[logicalWal];
        if (!state) state = std::make_shared<LogicalWalState>();
        return state;
    }

    std::filesystem::path segmentDirectory(const std::string& logicalWal) {
        return logicalWal + ".segments";
    }

    std::filesystem::path segmentPath(const std::string& logicalWal, uint64_t segmentId) {
        std::ostringstream name;
        name << std::setw(20) << std::setfill('0') << segmentId << ".wal";
        return segmentDirectory(logicalWal) / name.str();
    }

    uint64_t segmentMaxBytes() {
        return std::max<uint64_t>(kSegmentHeaderBytes + 1024,
                                  uint64Env("WAL_SEGMENT_MAX_BYTES", 64ULL * 1024 * 1024));
    }

    uint64_t maxRecordBytes() {
        return std::max<uint64_t>(1024,
                                  uint64Env("WAL_MAX_RECORD_BYTES", 64ULL * 1024 * 1024));
    }

    void appendU16(std::string& bytes, uint16_t value) {
        bytes.push_back(static_cast<char>(value & 0xff));
        bytes.push_back(static_cast<char>((value >> 8) & 0xff));
    }

    void appendU64(std::string& bytes, uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 0xff));
        }
    }

    uint16_t readU16(const char* bytes) {
        return static_cast<uint16_t>(static_cast<unsigned char>(bytes[0])) |
               static_cast<uint16_t>(static_cast<unsigned char>(bytes[1]) << 8);
    }

    uint64_t readU64(const char* bytes) {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[shift / 8])) << shift;
        }
        return value;
    }

    uint64_t walIdentityHash(const nlohmann::json& entry) {
        if (!entry.is_object()) return 0;
        std::string identity;
        try {
            identity = entry.value("userId", std::string()) + "\n" +
                       entry.value("db", std::string()) + "\n" +
                       entry.value("collection", std::string());
        } catch (...) {
            return 0;
        }
        if (identity == "\n\n") return 0;
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char byte : identity) {
            hash = (hash ^ byte) * 1099511628211ULL;
        }
        return hash;
    }

    bool readEntryLsn(const nlohmann::json& entry, bool legacy,
                      uint64_t fallback, uint64_t& lsn) {
        try {
            if (!entry.is_object()) return false;
            const char* field = entry.contains("_wal_lsn") ? "_wal_lsn" : "_wal_seq";
            if (!legacy && !entry.contains("_wal_lsn")) return false;
            if (!entry.contains(field)) {
                lsn = fallback;
                return legacy;
            }
            const auto& value = entry[field];
            if (!value.is_number_unsigned() && !value.is_number_integer()) return false;
            lsn = value.get<uint64_t>();
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string segmentHeader(uint64_t segmentId, uint64_t firstLsn, uint64_t identityHash) {
        std::string bytes("PDBW3", 5);
        bytes.push_back(static_cast<char>(kSegmentVersion));
        appendU16(bytes, static_cast<uint16_t>(kSegmentHeaderBytes));
        appendU64(bytes, segmentId);
        appendU64(bytes, firstLsn);
        appendU64(bytes, identityHash);
        return bytes;
    }

    bool isValidWalOp(WalOp op) {
        switch (op) {
            case WalOp::INSERT:
            case WalOp::UPDATE:
            case WalOp::DELETE:
            case WalOp::BATCH_COMPRESSED:
            case WalOp::COMPRESSED_ZLIB:
            case WalOp::SEGMENT_HEADER:
                return true;
        }
        return false;
    }

    std::vector<std::filesystem::path> sortedSegmentFiles(
        const std::string& logicalWal, std::error_code* scanError = nullptr) {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        const auto directory = segmentDirectory(logicalWal);
        if (!std::filesystem::exists(directory, ec)) {
            if (scanError) *scanError = ec;
            return files;
        }
        std::filesystem::directory_iterator it(directory, ec), end;
        while (!ec && it != end) {
            std::error_code typeError;
            const bool regular = it->is_regular_file(typeError);
            if (typeError) {
                ec = typeError;
                break;
            }
            if (regular && it->path().extension() == ".wal") {
                files.push_back(it->path());
            }
            it.increment(ec);
        }
        if (scanError) *scanError = ec;
        std::sort(files.begin(), files.end());
        return files;
    }

    bool looksLikeJsonLines(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        char byte = 0;
        while (input.get(byte)) {
            if (std::isspace(static_cast<unsigned char>(byte))) continue;
            return byte == '{' || byte == '[';
        }
        return false;
    }

    void setScanFailure(InternalScanResult& scan, WalScanStatus status,
                        const std::string& error) {
        scan.publicResult.status = status;
        scan.publicResult.error = error;
    }

    void setFileScanFailure(InternalScanResult& scan, ScannedWalFile file,
                            WalScanStatus status, const std::string& error) {
        if (status == WalScanStatus::PARTIAL_TAIL) {
            scan.publicResult.validBytes += file.validBytes;
            scan.files.push_back(std::move(file));
        }
        setScanFailure(scan, status, error);
    }

    InternalScanResult scanWalFiles(const std::string& logicalWal, const WalVisitor& visitor) {
        InternalScanResult scan;
        std::vector<std::pair<std::filesystem::path, bool>> physicalFiles;
        std::error_code ec;
        const bool legacyExists = std::filesystem::exists(logicalWal, ec);
        if (ec) {
            setScanFailure(scan, WalScanStatus::IO_ERROR,
                           "cannot inspect legacy WAL: " + ec.message());
            return scan;
        }
        if (legacyExists && std::filesystem::is_regular_file(logicalWal, ec)) {
            physicalFiles.push_back({logicalWal, true});
        }
        if (ec) {
            setScanFailure(scan, WalScanStatus::IO_ERROR,
                           "cannot inspect legacy WAL type: " + ec.message());
            return scan;
        }
        std::error_code segmentError;
        for (const auto& path : sortedSegmentFiles(logicalWal, &segmentError)) {
            physicalFiles.push_back({path, false});
        }
        if (segmentError) {
            setScanFailure(scan, WalScanStatus::IO_ERROR,
                           "cannot enumerate WAL segments: " + segmentError.message());
            return scan;
        }

        uint64_t segmentIdentity = 0;
        uint64_t previousSegmentId = 0;
        for (size_t fileIndex = 0; fileIndex < physicalFiles.size(); ++fileIndex) {
            const auto& [path, legacy] = physicalFiles[fileIndex];
            const bool finalFile = fileIndex + 1 == physicalFiles.size();
            ScannedWalFile fileMeta;
            fileMeta.path = path;
            fileMeta.legacy = legacy;
            std::ifstream input(path, std::ios::binary);
            if (!input) {
                setScanFailure(scan, WalScanStatus::IO_ERROR,
                               "cannot open WAL file: " + path.string());
                return scan;
            }

            if (legacy && looksLikeJsonLines(path)) {
                std::string line;
                while (std::getline(input, line)) {
                    if (line.empty()) continue;
                    nlohmann::json wrapper;
                    try { wrapper = nlohmann::json::parse(line); }
                    catch (...) {
                        setFileScanFailure(scan, fileMeta, finalFile && input.eof()
                            ? WalScanStatus::PARTIAL_TAIL : WalScanStatus::CORRUPT,
                            "invalid JSON WAL record: " + path.string());
                        return scan;
                    }
                    if (!wrapper.is_object()) {
                        setScanFailure(scan, WalScanStatus::CORRUPT,
                                       "legacy WAL record is not an object");
                        return scan;
                    }
                    if (wrapper.contains("checksum")) {
                        try {
                            const auto record = WALRecord::fromJson(wrapper);
                            if (record.checksum != 0 && !record.verifyChecksum()) {
                                setScanFailure(scan, WalScanStatus::CORRUPT,
                                               "legacy JSON WAL checksum mismatch");
                                return scan;
                            }
                        } catch (...) {
                            setScanFailure(scan, WalScanStatus::CORRUPT,
                                           "invalid legacy JSON WAL fields");
                            return scan;
                        }
                    }
                    const nlohmann::json entry = wrapper.contains("payload")
                        ? wrapper["payload"] : wrapper;
                    const uint64_t entryIdentity = walIdentityHash(entry);
                    if (entryIdentity != 0) {
                        if (fileMeta.identityHash == 0) fileMeta.identityHash = entryIdentity;
                        if (fileMeta.identityHash != entryIdentity) {
                            setScanFailure(scan, WalScanStatus::CORRUPT,
                                           "legacy WAL namespace identity changed");
                            return scan;
                        }
                    }
                    uint64_t lsn = 0;
                    if (wrapper.contains("lsn")) {
                        try { lsn = wrapper["lsn"].get<uint64_t>(); }
                        catch (...) { lsn = 0; }
                    } else if (!readEntryLsn(entry, true, scan.publicResult.lastLsn + 1, lsn)) {
                        lsn = 0;
                    }
                    if (lsn == 0 || lsn <= scan.publicResult.lastLsn ||
                        !verifyWalPayloadCrc(entry)) {
                        setScanFailure(scan, WalScanStatus::CORRUPT,
                                       "invalid legacy WAL payload or LSN");
                        return scan;
                    }
                    fileMeta.firstLsn = fileMeta.firstLsn == 0 ? lsn : fileMeta.firstLsn;
                    fileMeta.endLsn = lsn;
                    scan.publicResult.lastLsn = lsn;
                    ++scan.publicResult.records;
                    const auto afterLine = input.tellg();
                    fileMeta.validBytes = afterLine < 0
                        ? std::filesystem::file_size(path, ec)
                        : static_cast<uint64_t>(afterLine);
                    if (visitor && !visitor({WalOp::INSERT, lsn, entry})) return scan;
                }
                scan.publicResult.validBytes += fileMeta.validBytes;
                scan.files.push_back(fileMeta);
                continue;
            }

            if (!legacy) {
                char header[kSegmentHeaderBytes]{};
                input.read(header, sizeof(header));
                if (input.gcount() != static_cast<std::streamsize>(sizeof(header))) {
                    setFileScanFailure(scan, fileMeta,
                                       finalFile ? WalScanStatus::PARTIAL_TAIL
                                                 : WalScanStatus::CORRUPT,
                                       "partial WAL segment header: " + path.string());
                    return scan;
                }
                if (std::memcmp(header, "PDBW3", 5) != 0 ||
                    static_cast<uint8_t>(header[5]) != kSegmentVersion ||
                    readU16(header + 6) != kSegmentHeaderBytes) {
                    setScanFailure(scan, WalScanStatus::CORRUPT,
                                   "invalid WAL segment header: " + path.string());
                    return scan;
                }
                fileMeta.segmentId = readU64(header + 8);
                fileMeta.firstLsn = readU64(header + 16);
                fileMeta.identityHash = readU64(header + 24);
                uint64_t namedSegmentId = 0;
                try { namedSegmentId = std::stoull(path.stem().string()); }
                catch (...) { namedSegmentId = 0; }
                if (fileMeta.segmentId == 0 || fileMeta.segmentId != namedSegmentId ||
                    fileMeta.segmentId <= previousSegmentId ||
                    fileMeta.firstLsn == 0 ||
                    (segmentIdentity != 0 && fileMeta.identityHash != segmentIdentity)) {
                    setScanFailure(scan, WalScanStatus::CORRUPT,
                                   "inconsistent WAL segment identity or range");
                    return scan;
                }
                previousSegmentId = fileMeta.segmentId;
                segmentIdentity = fileMeta.identityHash;
                fileMeta.validBytes = kSegmentHeaderBytes;
            }

            bool firstSegmentRecord = !legacy;
            while (true) {
                const auto frameStart = input.tellg();
                WalOp op{};
                input.read(reinterpret_cast<char*>(&op), sizeof(op));
                if (input.gcount() == 0 && input.eof()) break;
                if (input.gcount() != static_cast<std::streamsize>(sizeof(op))) {
                    setFileScanFailure(scan, fileMeta,
                                       finalFile ? WalScanStatus::PARTIAL_TAIL
                                                 : WalScanStatus::CORRUPT,
                                       "partial WAL operation");
                    return scan;
                }
                uint32_t payloadSize = 0;
                input.read(reinterpret_cast<char*>(&payloadSize), sizeof(payloadSize));
                if (input.gcount() != static_cast<std::streamsize>(sizeof(payloadSize))) {
                    setFileScanFailure(scan, fileMeta,
                                       finalFile ? WalScanStatus::PARTIAL_TAIL
                                                 : WalScanStatus::CORRUPT,
                                       "partial WAL frame header");
                    return scan;
                }
                if (!isValidWalOp(op) || op == WalOp::SEGMENT_HEADER ||
                    payloadSize == 0 || payloadSize > maxRecordBytes()) {
                    setScanFailure(scan, WalScanStatus::CORRUPT, "invalid WAL frame");
                    return scan;
                }
                std::string payload(payloadSize, '\0');
                input.read(payload.data(), payload.size());
                if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
                    setFileScanFailure(scan, fileMeta,
                                       finalFile ? WalScanStatus::PARTIAL_TAIL
                                                 : WalScanStatus::CORRUPT,
                                       "partial WAL payload");
                    return scan;
                }
                nlohmann::json entry;
                if (!decodeWalPayload(payload, entry) || !verifyWalPayloadCrc(entry)) {
                    setScanFailure(scan, WalScanStatus::CORRUPT,
                                   "invalid or checksummed WAL payload");
                    return scan;
                }
                uint64_t lsn = 0;
                const uint64_t entryIdentity = walIdentityHash(entry);
                if (legacy && entryIdentity != 0) {
                    if (fileMeta.identityHash == 0) fileMeta.identityHash = entryIdentity;
                    if (fileMeta.identityHash != entryIdentity) {
                        setScanFailure(scan, WalScanStatus::CORRUPT,
                                       "legacy WAL namespace identity changed");
                        return scan;
                    }
                }
                if (!readEntryLsn(entry, legacy, scan.publicResult.lastLsn + 1, lsn) ||
                    lsn == 0 || lsn <= scan.publicResult.lastLsn ||
                    (!legacy && fileMeta.identityHash != 0 && entryIdentity != 0 &&
                     entryIdentity != fileMeta.identityHash) ||
                    (firstSegmentRecord && lsn != fileMeta.firstLsn)) {
                    setScanFailure(scan, WalScanStatus::CORRUPT,
                                   "non-increasing WAL LSN or invalid segment boundary");
                    return scan;
                }
                firstSegmentRecord = false;
                if (fileMeta.firstLsn == 0) fileMeta.firstLsn = lsn;
                fileMeta.endLsn = lsn;
                scan.publicResult.lastLsn = lsn;
                ++scan.publicResult.records;
                fileMeta.validBytes = static_cast<uint64_t>(input.tellg());
                if (visitor && !visitor({op, lsn, std::move(entry)})) return scan;
                if (input.tellg() <= frameStart) {
                    setScanFailure(scan, WalScanStatus::IO_ERROR, "WAL scanner made no progress");
                    return scan;
                }
            }
            if (!legacy && fileMeta.endLsn == 0 && fileMeta.firstLsn > 0) {
                scan.publicResult.lastLsn = std::max(
                    scan.publicResult.lastLsn, fileMeta.firstLsn - 1);
            }
            scan.publicResult.validBytes += fileMeta.validBytes;
            scan.files.push_back(fileMeta);
        }
        return scan;
    }

    bool syncDirectory(const std::filesystem::path& directory) {
#ifdef _WIN32
        (void)directory;
        return true;
#else
        const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return false;
        const bool ok = fsync(fd) == 0;
        close(fd);
        return ok;
#endif
    }

    void updateGlobalWalSeq(uint64_t lsn) {
        uint64_t current = g_walSeq.load(std::memory_order_relaxed);
        while (current < lsn &&
               !g_walSeq.compare_exchange_weak(current, lsn, std::memory_order_relaxed)) {}
    }

    bool groupCommitEnabled() {
        if (inMemoryOltpModeEnabled()) {
            return boolEnvAny("WAL_GROUP_COMMIT_ENABLED", "WAL_GROUP_COMMIT", false);
        }
        return boolEnvAny("WAL_GROUP_COMMIT_ENABLED", "WAL_GROUP_COMMIT", walFsyncEnabled());
    }

    int groupCommitMaxMs() {
        return std::max(1, intEnvAny("WAL_GROUP_COMMIT_INTERVAL_MS", "WAL_GROUP_COMMIT_MAX_MS", "WAL_BATCH_INTERVAL_MS", 3));
    }

    size_t groupCommitMaxBatch() {
        const char* v = firstEnv("WAL_GROUP_COMMIT_MAX_BATCH");
        if (!v) return 512;
        try { return std::max(1, std::stoi(v)); } catch(...) { return 512; }
    }

    size_t groupCommitMaxBytes() {
        const char* v = firstEnv("WAL_GROUP_COMMIT_MAX_BYTES");
        if (!v) return 4 * 1024 * 1024;
        try { return static_cast<size_t>(std::max(1024, std::stoi(v))); } catch(...) { return 4 * 1024 * 1024; }
    }

    int batchIntervalMs() {
        return std::max(0, intEnvAny("WAL_BATCH_INTERVAL_MS", "WAL_GROUP_COMMIT_INTERVAL_MS", nullptr, 0));
    }

    struct AppendOutcome {
        bool success{false};
        std::string error;
    };

    void completeEntries(const std::vector<PendingWalEntry>& entries,
                         bool success,
                         const std::string& error) {
        std::unordered_set<WalCompletion*> completed;
        for (const auto& entry : entries) {
            if (!entry.completion || !completed.insert(entry.completion.get()).second) continue;
            {
                std::lock_guard<std::mutex> lock(entry.completion->mutex);
                entry.completion->success = success;
                entry.completion->error = error;
                entry.completion->done = true;
            }
            entry.completion->cv.notify_all();
        }
        if (success) {
            g_walStats.entriesAcknowledged.fetch_add(
                static_cast<uint64_t>(entries.size()), std::memory_order_relaxed);
        } else {
            g_walStats.entriesFailed.fetch_add(
                static_cast<uint64_t>(entries.size()), std::memory_order_relaxed);
        }
    }

    WalAppendResult waitForCompletion(const std::shared_ptr<WalCompletion>& completion) {
        std::unique_lock<std::mutex> lock(completion->mutex);
        completion->cv.wait(lock, [&] { return completion->done; });
        if (!completion->success) {
            throw std::runtime_error(completion->error.empty()
                ? "collection WAL append failed"
                : completion->error);
        }
        return completion->result;
    }

    void initializeLogicalWal(const std::string& logicalWal, LogicalWalState& state) {
        if (state.initialized) return;
        auto scan = scanWalFiles(logicalWal, {});
        if (scan.publicResult.status == WalScanStatus::PARTIAL_TAIL && !scan.files.empty()) {
            const auto tail = scan.files.back();
            closeWalFile(tail.path.string());
            std::error_code ec;
            if (tail.validBytes == 0) {
                std::filesystem::remove(tail.path, ec);
            } else {
                std::filesystem::resize_file(tail.path, tail.validBytes, ec);
            }
            if (ec) {
                throw std::runtime_error("cannot truncate partial WAL tail: " + ec.message());
            }
            if (tail.validBytes != 0 && !syncWalFile(tail.path.string())) {
                throw std::runtime_error("cannot synchronize truncated WAL tail");
            }
            if (!syncDirectory(tail.path.parent_path())) {
                throw std::runtime_error("cannot synchronize truncated WAL directory");
            }
            scan = scanWalFiles(logicalWal, {});
        }
        if (scan.publicResult.status != WalScanStatus::OK) {
            throw std::runtime_error("cannot append after invalid WAL scan: " +
                                     scan.publicResult.error);
        }
        state.nextLsn = scan.publicResult.lastLsn + 1;
        for (const auto& file : scan.files) {
            if (!file.legacy && file.segmentId >= state.activeSegment) {
                state.activeSegment = file.segmentId;
                state.activeBytes = file.validBytes;
                state.identityHash = file.identityHash;
            }
        }
        state.initialized = true;
        updateGlobalWalSeq(scan.publicResult.lastLsn);
    }

    AppendOutcome appendEntries(const std::string& file,
                                const std::vector<PendingWalEntry>& entries,
                                bool forceFsync) {
        if (entries.empty()) return {true, {}};
        auto appendStart = std::chrono::steady_clock::now();
        auto logicalState = logicalWalState(file);
        std::lock_guard<std::mutex> appendOrder(logicalState->mutex);
        try {
            if (logicalState->clearing) {
                const std::string error = "collection WAL is being cleared: " + file;
                completeEntries(entries, false, error);
                return {false, error};
            }
            std::filesystem::path p(file);
            if (!p.parent_path().empty()) {
                std::filesystem::create_directories(p.parent_path());
            }
            initializeLogicalWal(file, *logicalState);
            if (logicalState->poisoned) throw std::runtime_error(logicalState->error);
            const auto walDirectory = segmentDirectory(file);
            const bool createdWalDirectory = std::filesystem::create_directories(walDirectory);
            if (createdWalDirectory && !syncDirectory(walDirectory.parent_path())) {
                throw std::runtime_error("cannot synchronize new WAL segment directory");
            }

            uint64_t bytesWritten = 0;
            uint64_t nextLsn = logicalState->nextLsn;
            uint64_t activeSegment = logicalState->activeSegment;
            uint64_t activeBytes = logicalState->activeBytes;
            uint64_t identityHash = logicalState->identityHash;
            std::vector<std::pair<std::string, std::string>> chunks;
            for (const auto& entry : entries) {
                nlohmann::json stamped = entry.entry;
                const uint64_t entryIdentity = walIdentityHash(stamped);
                if (identityHash == 0) identityHash = entryIdentity;
                if (entryIdentity != 0 && identityHash != 0 && entryIdentity != identityHash) {
                    throw std::runtime_error("collection WAL namespace identity changed");
                }
                const uint64_t lsn = nextLsn++;
                stampWalIntegrityFields(stamped, lsn);
                const std::string payload = encodeWalPayload(stamped);
                if (payload.size() > maxRecordBytes()) {
                    throw std::runtime_error("collection WAL record exceeds WAL_MAX_RECORD_BYTES");
                }
                uint32_t size = static_cast<uint32_t>(payload.size());
                WalOp op = entry.op;
                std::string frame;
                frame.append(reinterpret_cast<const char*>(&op), sizeof(op));
                frame.append(reinterpret_cast<const char*>(&size), sizeof(size));
                frame.append(payload);
                if (activeSegment == 0 ||
                    (activeBytes > kSegmentHeaderBytes &&
                     activeBytes + frame.size() > segmentMaxBytes())) {
                    ++activeSegment;
                    activeBytes = 0;
                }
                const std::string physical = segmentPath(file, activeSegment).string();
                if (chunks.empty() || chunks.back().first != physical) {
                    chunks.push_back({physical, {}});
                }
                if (activeBytes == 0) {
                    chunks.back().second += segmentHeader(activeSegment, lsn, identityHash);
                    activeBytes = kSegmentHeaderBytes;
                    ++g_walStats.segmentsCreated;
                }
                chunks.back().second += frame;
                activeBytes += frame.size();
                bytesWritten += frame.size();
                if (entry.completion) {
                    if (entry.completion->result.firstLsn == 0) {
                        entry.completion->result.firstLsn = lsn;
                    }
                    entry.completion->result.lastLsn = lsn;
                }
            }
            for (const auto& [physical, bytes] : chunks) {
                if (!appendWalBytes(physical, bytes)) {
                    logicalState->poisoned = true;
                    logicalState->error = "failed collection WAL append requires restart";
                    const std::string error = "failed to append collection WAL file: " + physical;
                    std::cerr << "[WAL] " << error << "\n";
                    completeEntries(entries, false, error);
                    return {false, error};
                }
            }
            g_walStats.entriesWritten.fetch_add(
                static_cast<uint64_t>(entries.size()), std::memory_order_relaxed);
            g_walStats.bytesWritten.fetch_add(bytesWritten, std::memory_order_relaxed);

            auto appendEnd = std::chrono::steady_clock::now();
            const uint64_t appendUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    appendEnd - appendStart).count());
            pacificdb::timing::recordStage(pacificdb::timing::Stage::WalAppend, appendUs);

            if (forceFsync || walFsyncEnabled()) {
                auto fsyncStart = std::chrono::steady_clock::now();
                for (const auto& [physical, bytes] : chunks) {
                    (void)bytes;
                    if (!syncWalFile(physical)) {
                        logicalState->poisoned = true;
                        logicalState->error = "collection WAL sync failure requires restart";
                        const std::string error =
                            "failed to synchronize collection WAL file: " + physical;
                        std::cerr << "[WAL] " << error << "\n";
                        completeEntries(entries, false, error);
                        return {false, error};
                    }
                }
                g_walStats.entriesFsynced.fetch_add(1, std::memory_order_relaxed);
                auto fsyncEnd = std::chrono::steady_clock::now();
                const uint64_t fsyncUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        fsyncEnd - fsyncStart).count());
                pacificdb::timing::recordStage(pacificdb::timing::Stage::WalFsync, fsyncUs);
            }

            logicalState->nextLsn = nextLsn;
            logicalState->activeSegment = activeSegment;
            logicalState->activeBytes = activeBytes;
            logicalState->identityHash = identityHash;
            updateGlobalWalSeq(nextLsn - 1);
            g_walStats.activeSegments.store(
                activeSegment, std::memory_order_relaxed);

            auto flushEnd = std::chrono::steady_clock::now();
            const double flushMs =
                std::chrono::duration<double, std::milli>(flushEnd - appendStart).count();
            double current = g_walStats.avgFlushLatencyMs.load(std::memory_order_relaxed);
            double desired = (current <= 0.0) ? flushMs : (0.15 * flushMs + 0.85 * current);
            while (!g_walStats.avgFlushLatencyMs.compare_exchange_weak(
                current, desired, std::memory_order_relaxed)) {
                desired = (current <= 0.0) ? flushMs : (0.15 * flushMs + 0.85 * current);
            }

            g_walStats.batchesCommitted.fetch_add(1, std::memory_order_relaxed);
            completeEntries(entries, true, {});
            return {true, {}};
        } catch (const std::exception& ex) {
            const std::string error = "collection WAL append exception for " + file + ": " + ex.what();
            std::cerr << "[WAL] " << error << "\n";
            completeEntries(entries, false, error);
            return {false, error};
        }
    }

    bool shouldFlushNowLocked() {
        const size_t maxBatch = groupCommitMaxBatch();
        const size_t maxBytes = groupCommitMaxBytes();
        for (const auto &kv : buffers) {
            if (kv.second.size() >= maxBatch) return true;
            size_t bytes = 0;
            for (const auto &e : kv.second) {
                bytes += sizeof(WalOp) + sizeof(uint32_t) + encodeWalPayload(e.entry).size();
                if (bytes >= maxBytes) return true;
            }
        }
        return false;
    }

    void flushOnce() {
        std::unordered_map<std::string, std::vector<PendingWalEntry>> toFlush;
        {
            std::lock_guard<std::mutex> lk(bufMutex);
            toFlush.swap(buffers);
        }

        for (auto &kv : toFlush) {
            const std::string &file = kv.first;
            auto &vec = kv.second;
            g_walStats.pendingEntries.fetch_sub(
                static_cast<uint64_t>(vec.size()), std::memory_order_relaxed);
            appendEntries(file, vec, false);
        }
    }

    void flusherLoop() {
        const int interval = std::max(batchIntervalMs(), groupCommitMaxMs());
        while (flusherRunning) {
            std::unique_lock<std::mutex> lk(bufMutex);
            bufCv.wait_for(lk, std::chrono::milliseconds(interval), []() {
                return !flusherRunning || shouldFlushNowLocked();
            });
            lk.unlock();
            flushOnce();
        }
        // final flush
        flushOnce();
    }
}

void WAL::init() {
    std::lock_guard<std::mutex> lk(bufMutex);
    if (flusherRunning) return;
    int interval = std::max(batchIntervalMs(), groupCommitMaxMs());
    const bool groupCommit = groupCommitEnabled();
    std::cout << "[WAL] group_commit=" << (groupCommit ? "enabled" : "disabled") << std::endl;
    std::cout << "[WAL] group_commit_interval_ms=" << interval << std::endl;
    std::cout << "[WAL] group_commit_batch_size=" << groupCommitMaxBatch() << std::endl;
    std::cout << "[WAL] group_commit_max_bytes=" << groupCommitMaxBytes() << std::endl;
    if (!groupCommit && interval <= 0) return;
    flusherRunning = true;
    flusherThread = std::thread(flusherLoop);
    std::cout << "[WAL] Background flusher started, interval=" << interval << "ms"
              << " groupCommit=" << (groupCommit ? "on" : "off") << std::endl;
}

void WAL::shutdown() {
    bool joinFlusher = false;
    {
        std::lock_guard<std::mutex> lk(bufMutex);
        if (flusherRunning) {
            flusherRunning = false;
            joinFlusher = true;
        }
    }
    if (joinFlusher) {
        bufCv.notify_all();
        if (flusherThread.joinable()) flusherThread.join();
    }
    closeWalFiles();
}

void WAL::flush(const std::string& file, bool forceFsync) {
    std::vector<PendingWalEntry> entries;
    {
        std::lock_guard<std::mutex> lk(bufMutex);
        auto it = buffers.find(file);
        if (it != buffers.end()) {
            entries.swap(it->second);
            if (entries.empty()) {
                // fall through for optional fsync
            }
        } else {
            // fall through for optional fsync
        }
    }

    if (!entries.empty()) {
        g_walStats.pendingEntries.fetch_sub(
            static_cast<uint64_t>(entries.size()), std::memory_order_relaxed);
        const auto outcome = appendEntries(file, entries, forceFsync);
        if (!outcome.success) throw std::runtime_error(outcome.error);
        return;
    }

    if (!forceFsync) {
        return;
    }

    // Ensure durability even when no buffered entries are present.
    auto state = logicalWalState(file);
    std::lock_guard<std::mutex> appendOrder(state->mutex);
    initializeLogicalWal(file, *state);
    const std::string physical = state->activeSegment == 0
        ? file : segmentPath(file, state->activeSegment).string();
    if (!syncWalFile(physical)) {
        throw std::runtime_error("failed to synchronize collection WAL file: " + file);
    }
    g_walStats.entriesFsynced.fetch_add(1, std::memory_order_relaxed);
}

WalAppendResult WAL::log(const std::string& file, const nlohmann::json& entry) {
    auto completion = std::make_shared<WalCompletion>();
    completion->result.entries = 1;
    PendingWalEntry pending{WalOp::INSERT, entry, completion};
    const bool useGroupCommit = groupCommitEnabled();
    int interval = batchIntervalMs();

    if (!useGroupCommit && interval <= 0) {
        appendEntries(file, std::vector<PendingWalEntry>{std::move(pending)}, false);
        return waitForCompletion(completion);
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(bufMutex);
        if (flusherRunning) {
            buffers[file].push_back(std::move(pending));
            g_walStats.pendingEntries.fetch_add(1, std::memory_order_relaxed);
            queued = true;
        }
    }
    if (!queued) {
        appendEntries(file, std::vector<PendingWalEntry>{std::move(pending)}, false);
    } else {
        bufCv.notify_all();
    }
    return waitForCompletion(completion);
}

WalAppendResult WAL::logBatch(const std::string& file,
                              const std::vector<nlohmann::json>& entries) {
    if (entries.empty()) return {};

    auto completion = std::make_shared<WalCompletion>();
    std::vector<PendingWalEntry> pending;
    pending.reserve(entries.size());
    for (const auto& entry : entries) {
        pending.push_back(PendingWalEntry{WalOp::INSERT, entry, completion});
    }
    completion->result.entries = entries.size();

    const bool useGroupCommit = groupCommitEnabled();
    int interval = batchIntervalMs();

    if (!useGroupCommit && interval <= 0) {
        appendEntries(file, pending, false);
        return waitForCompletion(completion);
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(bufMutex);
        if (flusherRunning) {
            auto& buffer = buffers[file];
            buffer.reserve(buffer.size() + pending.size());
            buffer.insert(buffer.end(),
                          std::make_move_iterator(pending.begin()),
                          std::make_move_iterator(pending.end()));
            g_walStats.pendingEntries.fetch_add(
                static_cast<uint64_t>(pending.size()), std::memory_order_relaxed);
            queued = true;
        }
    }
    if (!queued) {
        appendEntries(file, pending, false);
    } else {
        bufCv.notify_all();
    }
    return waitForCompletion(completion);
}

void WAL::replay(const std::string& file) {
    auto records = WALIntegrity::scanAndTruncateWAL(file);
    for (const auto& record : records) {
        if (!record.isCommitted) continue;
        try {
            // V11.4-DIV-001: this result was previously DISCARDED, so a committed WAL
            // record that failed to apply was skipped and the loop carried on applying
            // later records over the resulting gap. Stop at the first failure instead.
            const bool applied = DatabaseEngine::applyReplicatedEntry(record.payload);
            if (!applied) {
                RaftCore::instance().blockApply(
                    record.payload.value("_raft_commit_index", static_cast<uint64_t>(0)),
                    record.payload.value("_raft_term", static_cast<uint64_t>(0)),
                    record.payload.value("op", record.payload.value("type", std::string("unknown"))),
                    record.payload.value("logicalWriteId", std::string("")),
                    "WAL::replay: applyReplicatedEntry returned false at LSN " +
                        std::to_string(record.lsn),
                    /*mutationMayHaveOccurred=*/true);
                std::cerr << "[WAL] Apply returned false for record LSN " << record.lsn
                          << "; halting replay rather than skipping the record\n";
                break;
            }
        } catch (const std::exception& ex) {
            std::cerr << "[WAL] Failed to replay WAL record LSN " << record.lsn
                      << ": " << ex.what() << "\n";
            break;
        } catch (...) {
            std::cerr << "[WAL] Failed to replay WAL record LSN " << record.lsn
                      << ": unknown error\n";
            break;
        }
    }
}

WalScanResult WAL::scan(const std::string& logicalWal, const WalVisitor& visitor) {
    auto state = logicalWalState(logicalWal);
    std::lock_guard<std::mutex> lock(state->mutex);
    return scanWalFiles(logicalWal, visitor).publicResult;
}

bool WAL::reclaimThrough(const std::string& logicalWal, uint64_t coveredLsn,
                         std::string* error) {
    auto state = logicalWalState(logicalWal);
    std::lock_guard<std::mutex> lock(state->mutex);
    auto scan = scanWalFiles(logicalWal, {});
    if (scan.publicResult.status != WalScanStatus::OK) {
        if (error) *error = scan.publicResult.error;
        return false;
    }

    uint64_t highestSegment = 0;
    bool reclaimsLegacy = false;
    uint64_t legacyIdentity = 0;
    for (const auto& file : scan.files) {
        highestSegment = std::max(highestSegment, file.segmentId);
        if (file.legacy && file.endLsn != 0 && file.endLsn <= coveredLsn) {
            reclaimsLegacy = true;
            legacyIdentity = file.identityHash;
        }
    }
    std::error_code ec;
    if (reclaimsLegacy && highestSegment == 0) {
        if (coveredLsn == std::numeric_limits<uint64_t>::max()) {
            if (error) *error = "checkpoint WAL LSN cannot advance beyond uint64 max";
            return false;
        }
        const auto directory = segmentDirectory(logicalWal);
        const bool created = std::filesystem::create_directories(directory, ec);
        if (ec || (created && !syncDirectory(directory.parent_path()))) {
            if (error) *error = "cannot create checkpoint WAL anchor directory";
            return false;
        }
        const auto anchor = segmentPath(logicalWal, 1);
        if (!appendWalBytes(anchor.string(),
                            segmentHeader(1, coveredLsn + 1, legacyIdentity)) ||
            !syncWalFile(anchor.string()) || !syncDirectory(directory)) {
            if (error) *error = "cannot persist checkpoint WAL LSN anchor";
            return false;
        }
    }
    for (const auto& file : scan.files) {
        const bool coveredLegacy = file.legacy && file.endLsn != 0 && file.endLsn <= coveredLsn;
        const bool coveredSealed = !file.legacy && file.segmentId < highestSegment &&
                                   file.endLsn != 0 && file.endLsn <= coveredLsn;
        if (!coveredLegacy && !coveredSealed) continue;
        closeWalFile(file.path.string());
        std::filesystem::remove(file.path, ec);
        if (ec) {
            if (error) *error = "cannot remove covered WAL file: " + ec.message();
            return false;
        }
        g_walStats.segmentsCompacted.fetch_add(!file.legacy, std::memory_order_relaxed);
    }
    const auto directory = segmentDirectory(logicalWal);
    if (std::filesystem::exists(directory, ec) && !syncDirectory(directory)) {
        if (error) *error = "cannot synchronize WAL segment directory";
        return false;
    }
    const auto parent = std::filesystem::path(logicalWal).parent_path();
    if (!parent.empty() && !syncDirectory(parent)) {
        if (error) *error = "cannot synchronize legacy WAL directory";
        return false;
    }
    state->initialized = false;
    return true;
}

// Read all WAL entries as JSON objects.
std::vector<std::string> WAL::readAll(const std::string& walFile) {
    std::vector<std::string> entries;
    auto result = WAL::scan(walFile, [&](const WalReplayRecord& record) {
        entries.push_back(record.entry.dump());
        return true;
    });
    if (result.status == WalScanStatus::CORRUPT || result.status == WalScanStatus::IO_ERROR) {
        throw std::runtime_error(result.error.empty() ? "collection WAL scan failed" : result.error);
    }
    std::cout << "[WAL] Read " << entries.size() << " entries\n";
    return entries;
}

void WAL::clear(const std::string& file) {
    std::vector<PendingWalEntry> rejected;
    {
        std::lock_guard<std::mutex> bufferLock(bufMutex);
        auto found = buffers.find(file);
        if (found != buffers.end()) {
            rejected.swap(found->second);
            buffers.erase(found);
            g_walStats.pendingEntries.fetch_sub(
                static_cast<uint64_t>(rejected.size()), std::memory_order_relaxed);
        }
    }
    if (!rejected.empty()) {
        completeEntries(rejected, false, "collection WAL cleared before append");
    }

    auto state = logicalWalState(file);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->clearing = true;
    closeWalFile(file);
    for (const auto& segment : sortedSegmentFiles(file)) closeWalFile(segment.string());
    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (ec) {
        std::cerr << "[WAL] Failed to clear WAL file: " << file << " error=" << ec.message() << "\n";
    }
    ec.clear();
    std::filesystem::remove_all(segmentDirectory(file), ec);
    if (ec) {
        std::cerr << "[WAL] Failed to clear WAL segments: " << file
                  << " error=" << ec.message() << "\n";
    }
    state->initialized = false;
    state->poisoned = false;
    state->error.clear();
    state->nextLsn = 1;
    state->activeSegment = 0;
    state->activeBytes = 0;
    state->identityHash = 0;
    state->clearing = false;
}

WalStats& WAL::getStats() {
    return g_walStats;
}

size_t WAL::getPendingCount() {
    std::lock_guard<std::mutex> lock(bufMutex);
    size_t count = 0;
    for (const auto& [key, entries] : buffers) {
        count += entries.size();
    }
    g_walStats.pendingEntries.store(static_cast<uint64_t>(count), std::memory_order_relaxed);
    return count;
}

std::string WAL::decompressPayload(const std::string& payload) {
    nlohmann::json decoded;
    if (decodeWalPayload(payload, decoded)) {
        return decoded.dump();
    }
    return payload;
}

uint64_t WAL::getCurrentLSN() {
    return g_walSeq.load(std::memory_order_relaxed);
}

uint64_t WAL::getSegmentCount() {
    return static_cast<uint64_t>(g_walStats.activeSegments.load(std::memory_order_relaxed));
}

uint64_t WAL::getSegmentCount(const std::string& logicalWal) {
    return static_cast<uint64_t>(sortedSegmentFiles(logicalWal).size());
}
