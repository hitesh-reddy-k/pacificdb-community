#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * WAL Record Structure with integrity support
 * Ensures recovery can distinguish complete vs partial/corrupt records
 */
class WALRecord {
public:
    uint64_t lsn;           // Log Sequence Number
    uint64_t txnId;         // Transaction ID
    std::string type;       // INSERT, UPDATE, DELETE, BEGIN, COMMIT, ABORT
    json payload;           // Record data
    uint64_t timestamp;     // Microseconds
    uint32_t checksum;      // CRC32 of record (0 = not computed yet)
    bool isCommitted;       // Raft commit status

    WALRecord() : lsn(0), txnId(0), timestamp(0), checksum(0), isCommitted(false) {}

    // Compute CRC32 checksum
    void computeChecksum();

    // Verify checksum
    bool verifyChecksum() const;

    // Serialize to JSON
    json toJson() const;

    // Deserialize from JSON
    static WALRecord fromJson(const json& j);

    // Get serialized size in bytes
    size_t getSerializedSize() const;
};

/**
 * WAL Integrity Manager
 * Handles:
 * - Checksum validation
 * - Partial record truncation
 * - Clean shutdown marker
 * - Durability fsync
 */
class WALIntegrity {
public:
    static constexpr const char* CLEAN_SHUTDOWN_MARKER = ".clean_shutdown";
    static constexpr const char* WAL_METADATA_FILE = ".wal_metadata.json";

    // Write WAL record with checksum + fsync
    static bool writeRecordWithFsync(const std::string& walPath, const WALRecord& record);

    // Scan WAL and detect/truncate partial tail
    static std::vector<WALRecord> scanAndTruncateWAL(const std::string& walPath);

    // Mark clean shutdown (call on graceful exit)
    static void markCleanShutdown(const std::string& dataRoot);

    // Check if last shutdown was clean
    static bool wasCleanShutdown(const std::string& dataRoot);

    // Durably remove the marker before accepting new work.
    static void clearCleanShutdownMarker(const std::string& dataRoot);

    // Get WAL metadata
    static json getWALMetadata(const std::string& dataRoot);

    // Update WAL metadata
    static void updateWALMetadata(const std::string& dataRoot, const json& meta);
};
