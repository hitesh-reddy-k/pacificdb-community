#include "wal_integrity.hpp"
#include <iostream>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cctype>
#include <cerrno>
#include <stdexcept>
#include "wal.hpp"
#include "data_durability.hpp"

#ifdef _WIN32
    #include <io.h>
    #include <fcntl.h>
    #define fsync _commit
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

// CRC32 polynomial
static const uint32_t CRC32_TABLE[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71642, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa44e5d6, 0x8d79e1d3,
    0x29b8c6e0, 0x5eead776, 0xc2007e8c, 0xb58c1c1a, 0x28f7dd59, 0x5f0e5ccf, 0xc6bdc035, 0xb1d6d6a3,
    0x2b0af5d2, 0x5cba3944, 0xc5ba4c2e, 0xb2bd0b8b, 0x2e909d28, 0x596f5bbc, 0xc0ba0146, 0xb7a881d0,
    0x3cfc5a8f, 0x4b97ac19, 0xd2d7a9e3, 0xa5c88f75, 0x3c88e6d6, 0x4b49c140, 0xd293c8ba, 0xa5a65c0c,
    0x3d58b23d, 0x4a948fab, 0xd3d27c51, 0xa4d9fdc7, 0x389d9364, 0x4f4d47f2, 0xd6d8dd08, 0xa1d1d59e,
    0x79da4d0a, 0x0ececd9c, 0x97ddd066, 0xe0801d0f, 0x7dc7cdac, 0x0ad94d3a, 0x93d0d980, 0xe40f1816,
    0x7d7b6267, 0x0ac542f1, 0x9df99c0b, 0xeae1e69d, 0x7737367e, 0x0a5d56e8, 0x9d0e6f12, 0xeacaef84,
    0x607f3b9b, 0x17b0abb0, 0x8eb1515a, 0xf94ccc0c, 0x647c4aaf, 0x13ca3a39, 0x8ad0c383, 0xfd04cd15,
    0x67c0cc44, 0x10fad0d2, 0x89d8e028, 0xfe6f60be, 0x69d0e41d, 0x1eaa548b, 0x87f7e971, 0xf0368e7,
    0x3dc7d0d2, 0x4a5e5044, 0xd37aabe3, 0xa45aaf75, 0x393b0e3c, 0x4e458eaa, 0xd7d8e50, 0x7a89e5c6,
    0x405e1f57, 0x37c9ffc1, 0xaea1873b, 0xd96c94ad, 0x45e4d20e, 0x329f5c98, 0xab7f84c2, 0xdce14454,
    0x5c1c6d4b, 0x2b2fdcdd, 0xb2d59227, 0xc5c612b1, 0x587c3a12, 0x2fbaba84, 0xb63c317e, 0xc1b3b3e8,
    0x5f45fc19, 0x283e2b8f, 0xb1f37c75, 0xc6a53ce3, 0x5a2bb1c0, 0x2dd3b756, 0xb40d3dac, 0xc39dfdda,
    0x7bab4d9a, 0x0c5bcddc, 0x95d89226, 0xe2b712b0, 0x7f96db13, 0x085e5b85, 0x917f3f7f, 0xe6bfafe9,
    0x7d5f8d38, 0x0ae0daae, 0x935f1054, 0xe49c6f82, 0x79988921, 0x0ed5a0b7, 0x975f564d, 0xe04d36db,
    0xb4f1d4c4, 0xc3a55452, 0x5a3e3ea8, 0x2dbebea3, 0xb0c8a40e, 0xc76b4498, 0x5e2f2f62, 0x296fafea,
    0xb3aba23b, 0xc0602262, 0x595d4898, 0x2edb480e, 0xb2768ead, 0xc5644a3b, 0x5c2a4ac1, 0x2b7a0a57,
    0x8e08e08c, 0xf9802c1a, 0x60c8ae00, 0x17a50e96, 0x8a3a9835, 0xfd3e58a3, 0x64e73359, 0x13aa3dcf,
    0x8d040c1e, 0xfa8c8c88, 0x63a78c72, 0x140e7ce4, 0x89a6eaa7, 0xfe6fca31, 0x6d0e01cb, 0x1a8d3c5d,
    0xd9ed0672, 0xaec486e4, 0x37adf1e, 0x70489788, 0xdd9c5bcb, 0xaa8db75d, 0x33dc2ba7, 0x44eebf31,
    0xdefc5c0, 0x7b17a056, 0xe2c74aac, 0x9551833a, 0x08f7f099, 0x7f86310f, 0xe62c35f5, 0x9163b583,
    0xc1eaf252, 0xb6a27e24, 0x2f5445de, 0x58c45c48, 0xcfb8a00b, 0xb8af209d, 0x21a0b667, 0x568b56f1,
    0xc6dde720, 0xb1ed8aa6, 0x2843e05c, 0x5f4080ca, 0xc2ab5669, 0xb59256ff, 0x2c917d05, 0x5bfbffb3,
};

static inline uint32_t computeCRC32Internal(const std::string& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (unsigned char byte : data) {
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

static bool looksLikeJsonText(const std::string& payload) {
    for (unsigned char ch : payload) {
        if (std::isspace(ch)) continue;
        return ch == '{' || ch == '[' || ch == '"';
    }
    return false;
}

static bool decodeWalPayload(const std::string& payload, json& out) {
    try {
        if (looksLikeJsonText(payload)) {
            out = json::parse(payload);
            return true;
        }
        std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
        out = json::from_msgpack(bytes);
        return true;
    } catch (...) {
        try {
            out = json::parse(payload);
            return true;
        } catch (...) {
            return false;
        }
    }
}

static bool verifyWalPayloadCrc(const json& entry) {
    if (!entry.is_object()) return false;
    if (!entry.contains("_wal_crc32")) return true;
    const auto& crcVal = entry["_wal_crc32"];
    if (!crcVal.is_number_unsigned() && !crcVal.is_number_integer()) return false;
    const uint32_t expected = crcVal.get<uint32_t>();
    json forCrc = entry;
    forCrc.erase("_wal_crc32");
    const std::string dumped = forCrc.dump();
    const uint32_t actual = pacificdb::durability::ChecksumCalculator::crc32(dumped);
    return actual == expected;
}

static bool isValidWalOp(WalOp op) {
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

static bool walLooksLikeJsonLines(const std::string& walPath) {
    std::ifstream wal(walPath, std::ios::binary);
    if (!wal) return false;

    char ch = 0;
    while (wal.get(ch)) {
        if (std::isspace(static_cast<unsigned char>(ch))) continue;
        return ch == '{' || ch == '[';
    }

    return false;
}

static std::vector<WALRecord> scanJsonLineWAL(const std::string& walPath) {
    std::vector<WALRecord> records;
    std::ifstream wal(walPath, std::ios::binary);
    if (!wal) return records;

    std::error_code ec;
    const auto fileSize = std::filesystem::exists(walPath, ec) ? std::filesystem::file_size(walPath, ec) : 0;

    std::string line;
    std::streamoff lastGoodPos = 0;
    std::streamoff lastRecordStart = 0;
    uint64_t lastSeq = 0;

    while (true) {
        std::streamoff recordStart = wal.tellg();
        if (!std::getline(wal, line)) break;
        std::streamoff afterLine = wal.tellg();
        if (afterLine < 0 && wal.eof()) {
            afterLine = static_cast<std::streamoff>(fileSize);
        }

        if (line.empty()) {
            lastGoodPos = afterLine;
            continue;
        }

        try {
            auto j = json::parse(line);
            WALRecord r = WALRecord::fromJson(j);

            if (r.checksum != 0 && !r.verifyChecksum()) {
                std::cerr << "[WAL] JSON WAL checksum failed at LSN " << r.lsn << ", truncating" << std::endl;
                break;
            }

            if (r.lsn != 0) {
                if (lastSeq != 0 && r.lsn <= lastSeq) {
                    std::cerr << "[WAL] JSON WAL sequence duplicate/regression detected, truncating" << std::endl;
                    lastGoodPos = r.lsn < lastSeq ? lastRecordStart : recordStart;
                    if (r.lsn < lastSeq && !records.empty()) records.pop_back();
                    break;
                }
                lastSeq = r.lsn;
            }

            records.push_back(r);
            lastRecordStart = recordStart;
            lastGoodPos = afterLine;
        } catch (const json::exception& e) {
            std::cerr << "[WAL] JSON WAL parse error: " << e.what() << ", truncating" << std::endl;
            break;
        } catch (...) {
            std::cerr << "[WAL] JSON WAL scan failed, truncating" << std::endl;
            break;
        }
    }

    wal.close();

    if (!ec && lastGoodPos >= 0 && static_cast<uintmax_t>(lastGoodPos) < fileSize) {
        std::filesystem::resize_file(walPath, static_cast<uintmax_t>(lastGoodPos), ec);
        if (!ec) {
            std::cout << "[WAL] Truncated JSON WAL to " << lastGoodPos << " bytes" << std::endl;
        }
    }

    return records;
}

static void doubleFsync(FILE* f) {
    fflush(f);
    #ifdef _WIN32
        int fd = _fileno(f);
        _commit(fd);
        _commit(fd);
    #else
        int fd = fileno(f);
        fsync(fd);
        fsync(fd);
    #endif
}

// WALRecord implementations
void WALRecord::computeChecksum() {
    std::ostringstream oss;
    oss << lsn << txnId << type << payload.dump() << timestamp << isCommitted;
    checksum = computeCRC32Internal(oss.str());
}

bool WALRecord::verifyChecksum() const {
    std::ostringstream oss;
    oss << lsn << txnId << type << payload.dump() << timestamp << isCommitted;
    uint32_t computed = computeCRC32Internal(oss.str());
    return computed == checksum;
}

json WALRecord::toJson() const {
    json j;
    j["lsn"] = lsn;
    j["txnId"] = txnId;
    j["type"] = type;
    j["payload"] = payload;
    j["timestamp"] = timestamp;
    j["checksum"] = checksum;
    j["isCommitted"] = isCommitted;
    return j;
}

WALRecord WALRecord::fromJson(const json& j) {
    WALRecord r;
    r.lsn = j.value("lsn", 0UL);
    r.txnId = j.value("txnId", 0UL);
    r.type = j.value("type", "");
    r.payload = j.value("payload", json());
    r.timestamp = j.value("timestamp", 0UL);
    r.checksum = j.value("checksum", 0U);
    r.isCommitted = j.value("isCommitted", false);
    return r;
}

size_t WALRecord::getSerializedSize() const {
    return toJson().dump().size();
}

bool WALIntegrity::writeRecordWithFsync(const std::string& walPath, const WALRecord& record) {
    try {
        WALRecord mutableRecord = record;
        mutableRecord.computeChecksum();

        FILE* f = fopen(walPath.c_str(), "a");
        if (!f) return false;

        std::string jsonStr = mutableRecord.toJson().dump() + "\n";
        if (fwrite(jsonStr.c_str(), 1, jsonStr.length(), f) != jsonStr.length()) {
            fclose(f);
            return false;
        }

        doubleFsync(f);
        fclose(f);
        return true;
    } catch (...) {
        std::cerr << "Error writing WAL record" << std::endl;
        return false;
    }
}

std::vector<WALRecord> WALIntegrity::scanAndTruncateWAL(const std::string& walPath) {
    std::vector<WALRecord> records;

    if (walLooksLikeJsonLines(walPath)) {
        return scanJsonLineWAL(walPath);
    }

    std::ifstream wal(walPath, std::ios::binary);

    if (!wal) return records;

    constexpr uint32_t kMaxWalEntryBytes = 32 * 1024 * 1024;
    std::streamoff lastGoodPos = 0;
    std::streamoff lastRecordStart = 0;
    uint64_t lastSeq = 0;

    while (true) {
        WalOp op;
        uint32_t size = 0;
        std::streamoff recordStart = wal.tellg();
        if (!wal.read(reinterpret_cast<char*>(&op), sizeof(op))) break;
        if (!wal.read(reinterpret_cast<char*>(&size), sizeof(size))) {
            std::cerr << "[WAL] Partial WAL header detected, truncating" << std::endl;
            break;
        }

        if (!isValidWalOp(op)) {
            std::cerr << "[WAL] Invalid WAL op detected, truncating" << std::endl;
            break;
        }

        if (size == 0) {
            lastGoodPos = wal.tellg();
            continue;
        }

        if (size > kMaxWalEntryBytes) {
            std::cerr << "[WAL] Invalid WAL record size " << size << ", truncating" << std::endl;
            break;
        }

        std::string payload(size, '\0');
        if (!wal.read(payload.data(), size)) {
            std::cerr << "[WAL] Partial WAL payload detected, truncating" << std::endl;
            break;
        }

        json entry;
        if (!decodeWalPayload(payload, entry)) {
            std::cerr << "[WAL] WAL payload decode failed, truncating" << std::endl;
            break;
        }

        if (!verifyWalPayloadCrc(entry)) {
            std::cerr << "[WAL] WAL payload checksum failed, truncating" << std::endl;
            break;
        }

        uint64_t seq = entry.value("_wal_seq", 0UL);
        if (seq != 0) {
            if (lastSeq != 0 && seq <= lastSeq) {
                std::cerr << "[WAL] WAL sequence duplicate/regression detected, truncating" << std::endl;
                lastGoodPos = seq < lastSeq ? lastRecordStart : recordStart;
                if (seq < lastSeq && !records.empty()) records.pop_back();
                break;
            }
            lastSeq = seq;
        }

        WALRecord record;
        record.lsn = seq != 0 ? seq : static_cast<uint64_t>(records.size() + 1);
        record.txnId = entry.value("txnId", 0UL);
        record.type = entry.value("op", entry.value("action", ""));
        record.payload = entry;
        record.timestamp = entry.value("_wal_ts_ms", entry.value("timestamp", 0UL));
        record.checksum = entry.value("_wal_crc32", 0U);
        record.isCommitted = entry.value("isCommitted", true);
        records.push_back(record);

        lastRecordStart = recordStart;
        lastGoodPos = wal.tellg();
        if (lastGoodPos <= recordStart) break;
    }

    wal.close();

    std::error_code ec;
    const auto fileSize = std::filesystem::exists(walPath, ec) ? std::filesystem::file_size(walPath, ec) : 0;
    if (!ec && lastGoodPos >= 0 && static_cast<uintmax_t>(lastGoodPos) < fileSize) {
        std::filesystem::resize_file(walPath, static_cast<uintmax_t>(lastGoodPos), ec);
        if (!ec) {
            std::cout << "[WAL] Truncated to " << lastGoodPos << " bytes" << std::endl;
        }
    }

    return records;
}

void WALIntegrity::markCleanShutdown(const std::string& dataRoot) {
    std::string markerPath = dataRoot + "/" + CLEAN_SHUTDOWN_MARKER;

    FILE* f = fopen(markerPath.c_str(), "w");
    if (!f) throw std::runtime_error("cannot create clean shutdown marker");
    // Version 2 means the caller proved that active LSM memtables were
    // force-flushed before writing this marker. Legacy "clean" markers did
    // not carry that guarantee and must never enable the no-replay path.
    bool persisted = fprintf(f, "clean-v2\n") > 0 && fflush(f) == 0;
#ifndef _WIN32
    persisted = persisted && fsync(fileno(f)) == 0 && fsync(fileno(f)) == 0;
#else
    persisted = persisted && _commit(_fileno(f)) == 0 && _commit(_fileno(f)) == 0;
#endif
    persisted = fclose(f) == 0 && persisted;
#ifndef _WIN32
    const int dirFd = open(dataRoot.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0 || fsync(dirFd) != 0) persisted = false;
    if (dirFd >= 0) close(dirFd);
#endif
    if (!persisted) {
        std::remove(markerPath.c_str());
        throw std::runtime_error("cannot persist clean shutdown marker");
    }
    std::cout << "[WAL] Clean shutdown marked at " << markerPath << std::endl;
}

void WALIntegrity::clearCleanShutdownMarker(const std::string& dataRoot) {
    const std::string markerPath = dataRoot + "/" + CLEAN_SHUTDOWN_MARKER;
    if (std::remove(markerPath.c_str()) != 0 && errno != ENOENT) {
        throw std::runtime_error("cannot remove clean shutdown marker");
    }
#ifndef _WIN32
    const int dirFd = open(dataRoot.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0 || fsync(dirFd) != 0) {
        if (dirFd >= 0) close(dirFd);
        throw std::runtime_error("cannot persist clean shutdown marker removal");
    }
    close(dirFd);
#endif
}

bool WALIntegrity::wasCleanShutdown(const std::string& dataRoot) {
    std::string markerPath = dataRoot + "/" + CLEAN_SHUTDOWN_MARKER;

    std::ifstream marker(markerPath);
    std::string markerVersion;
    if (marker.good()) std::getline(marker, markerVersion);
    const bool clean = markerVersion == "clean-v2";
    marker.close();

    // Startup retains this proof until all pre-listener migrations complete.
    // main consumes it immediately before Raft networking starts.
    return clean;
}

json WALIntegrity::getWALMetadata(const std::string& dataRoot) {
    std::string metaPath = dataRoot + "/" + WAL_METADATA_FILE;

    std::ifstream meta(metaPath);
    if (!meta) {
        return json{{"version", 1}, {"lastLSN", 0}, {"snapshotLSN", 0}};
    }

    try {
        json j;
        meta >> j;
        meta.close();
        return j;
    } catch (...) {
        return json{{"version", 1}, {"lastLSN", 0}, {"snapshotLSN", 0}};
    }
}

void WALIntegrity::updateWALMetadata(const std::string& dataRoot, const json& meta) {
    std::string metaPath = dataRoot + "/" + WAL_METADATA_FILE;

    FILE* f = fopen(metaPath.c_str(), "w");
    if (f) {
        std::string content = meta.dump(2) + "\n";
        fwrite(content.c_str(), 1, content.length(), f);
        doubleFsync(f);
        fclose(f);
    }
}
