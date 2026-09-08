#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pacificdb {
namespace durability {

// ============================================================================
// CHECKSUM ALGORITHMS
// ============================================================================

enum class ChecksumType {
    CRC32,          // Fast, good for error detection
    CRC32C,         // Hardware-accelerated CRC32
    XXHASH64,       // Very fast 64-bit hash
    SHA256,         // Cryptographic, for critical data
    MURMUR3         // Fast non-crypto hash
};

class ChecksumCalculator {
public:
    static uint32_t crc32(const void* data, size_t length);
    static uint32_t crc32c(const void* data, size_t length);  // HW accelerated
    static uint64_t xxhash64(const void* data, size_t length, uint64_t seed = 0);
    static std::string sha256(const void* data, size_t length);
    static std::string sha256File(const std::string& path);
    static uint64_t murmur3(const void* data, size_t length, uint32_t seed = 0);

    // Convenience overloads
    static uint32_t crc32(const std::string& data);
    static uint64_t xxhash64(const std::string& data);

    // Incremental checksum (for large data)
    class IncrementalCRC32 {
    public:
        IncrementalCRC32();
        void update(const void* data, size_t length);
        uint32_t finalize();
        void reset();
    private:
        uint32_t state_;
    };
};

// ============================================================================
// DATA BLOCK WITH CHECKSUM
// ============================================================================

#pragma pack(push, 1)
struct BlockHeader {
    uint32_t magic;           // Magic number for validation (0xBAD0B10C)
    uint32_t version;         // Block format version
    uint32_t blockSize;       // Total block size including header
    uint32_t dataSize;        // Size of actual data
    uint32_t checksum;        // CRC32 of data
    uint64_t sequenceNumber;  // For ordering/WAL
    uint8_t  checksumType;    // Algorithm used
    uint8_t  compressionType; // 0=none, 1=lz4, 2=zstd
    uint16_t flags;           // Additional flags
    char     reserved[8];     // Future use
};
#pragma pack(pop)

static constexpr uint32_t BLOCK_MAGIC = 0xBAD0B10C;
static constexpr uint32_t BLOCK_VERSION = 1;

class DataBlock {
public:
    DataBlock(ChecksumType checksumType = ChecksumType::CRC32);

    // Write data with checksum
    bool write(const void* data, size_t length);
    bool write(const std::string& data);

    // Read and verify
    bool read(void* buffer, size_t bufferSize, size_t& actualSize);
    std::optional<std::string> read();

    // Verification
    bool verify() const;
    bool verifyChecksum() const;

    // Accessors
    const BlockHeader& header() const { return header_; }
    const std::vector<uint8_t>& data() const { return data_; }
    size_t totalSize() const { return sizeof(BlockHeader) + data_.size(); }

    // Serialization
    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& bytes);
    bool deserialize(const void* data, size_t length);

private:
    uint32_t calculateChecksum() const;

    BlockHeader header_;
    std::vector<uint8_t> data_;
    ChecksumType checksumType_;
};

// ============================================================================
// SSTABLE BLOCK CHECKSUMS
// ============================================================================

struct SSTableBlockInfo {
    uint64_t offset;          // Offset in file
    uint32_t size;            // Block size
    uint32_t checksum;        // Block checksum
    std::string minKey;       // First key in block
    std::string maxKey;       // Last key in block
    uint32_t entryCount;      // Number of entries

    json toJson() const {
        return {
            {"offset", offset},
            {"size", size},
            {"checksum", checksum},
            {"minKey", minKey},
            {"maxKey", maxKey},
            {"entryCount", entryCount}
        };
    }
};

class SSTableVerifier {
public:
    SSTableVerifier(const std::string& sstablePath);

    // Full verification
    struct VerificationResult {
        bool valid;
        int blocksChecked;
        int blocksCorrupted;
        std::vector<SSTableBlockInfo> corruptedBlocks;
        std::chrono::milliseconds duration;
        std::string error;
    };

    VerificationResult verify();
    VerificationResult verifyBlock(uint64_t offset);

    // Quick check (sample blocks)
    VerificationResult quickVerify(double sampleRate = 0.1);

    // Repair (if possible)
    bool repairFromReplica(const std::string& replicaPath);

private:
    std::string sstablePath_;
};

// ============================================================================
// FSYNC DURABILITY MODES
// ============================================================================

enum class DurabilityMode {
    NONE,           // No explicit fsync (fastest, least durable)
    WRITE_THROUGH,  // O_DIRECT - bypass page cache
    PERIODIC,       // Fsync every N milliseconds
    BATCH,          // Fsync after batch of writes
    EVERY_WRITE,    // Fsync after every write (slowest, most durable)
    FDATASYNC       // fdatasync - only data, not metadata
};

struct DurabilityConfig {
    DurabilityMode mode;
    int periodicIntervalMs;    // For PERIODIC mode
    int batchSize;             // For BATCH mode
    bool preallocateFiles;     // Preallocate file space
    bool directIO;             // Use O_DIRECT

    json toJson() const {
        return {
            {"mode", static_cast<int>(mode)},
            {"periodicIntervalMs", periodicIntervalMs},
            {"batchSize", batchSize},
            {"preallocateFiles", preallocateFiles},
            {"directIO", directIO}
        };
    }
};

// ============================================================================
// DURABLE FILE WRITER
// ============================================================================

class DurableFileWriter {
public:
    DurableFileWriter(const std::string& path, const DurabilityConfig& config);
    ~DurableFileWriter();

    // Open/Close
    bool open();
    void close();
    bool isOpen() const { return isOpen_; }

    // Write with durability guarantees
    bool write(const void* data, size_t length);
    bool write(const std::string& data);
    bool writeBlock(const DataBlock& block);

    // Explicit sync
    bool sync();
    bool datasync();

    // Position
    int64_t position() const { return position_; }
    bool seek(int64_t offset);

    // Stats
    json getStats() const;

private:
    void backgroundSync();
    void checkBatchSync();

    std::string path_;
    DurabilityConfig config_;

    int fd_ = -1;
    bool isOpen_ = false;
    int64_t position_ = 0;

    std::atomic<int> pendingWrites_{0};
    std::atomic<uint64_t> bytesWritten_{0};
    std::atomic<uint64_t> syncsPerformed_{0};

    std::thread syncThread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
};

// ============================================================================
// CRASH RECOVERY
// ============================================================================

struct RecoveryPoint {
    uint64_t walSequence;
    uint64_t lastCheckpoint;
    std::chrono::system_clock::time_point timestamp;
    std::string checkpointPath;

    json toJson() const {
        return {
            {"walSequence", walSequence},
            {"lastCheckpoint", lastCheckpoint},
            {"checkpointPath", checkpointPath}
        };
    }
};

class CrashRecoveryManager {
public:
    static CrashRecoveryManager& instance() {
        static CrashRecoveryManager inst;
        return inst;
    }

    // Initialize recovery system
    void initialize(const std::string& dataDir);

    // Checkpoint management
    bool createCheckpoint();
    RecoveryPoint getLastCheckpoint() const;
    std::vector<RecoveryPoint> listCheckpoints() const;
    bool deleteOldCheckpoints(int keepCount);

    // WAL recovery
    struct RecoveryResult {
        bool success;
        uint64_t entriesRecovered;
        uint64_t entriesSkipped;
        std::chrono::milliseconds duration;
        std::vector<std::string> errors;
    };

    RecoveryResult recover();
    RecoveryResult recoverFromCheckpoint(const RecoveryPoint& checkpoint);
    RecoveryResult replayWAL(uint64_t fromSequence);

    // Validation
    bool validateDataIntegrity();
    bool validateWALIntegrity();

    // Recovery callbacks
    using EntryProcessor = std::function<bool(const std::string& entry)>;
    void setEntryProcessor(EntryProcessor processor) {
        entryProcessor_ = processor;
    }

    // Status
    json getStatus() const;

private:
    CrashRecoveryManager() = default;

    std::string dataDir_;
    RecoveryPoint lastCheckpoint_;
    EntryProcessor entryProcessor_;
    mutable std::mutex mutex_;
};

// ============================================================================
// DATA INTEGRITY MONITOR
// ============================================================================

class DataIntegrityMonitor {
public:
    static DataIntegrityMonitor& instance() {
        static DataIntegrityMonitor inst;
        return inst;
    }

    // Start background integrity checking
    void start(int intervalSeconds = 3600);  // Default: hourly
    void stop();
    bool isRunning() const { return running_; }

    // Manual checks
    struct IntegrityReport {
        std::chrono::system_clock::time_point checkTime;
        bool passed;
        int filesChecked;
        int filesCorrupted;
        int blocksChecked;
        int blocksCorrupted;
        std::vector<std::string> corruptedFiles;
        std::chrono::milliseconds duration;

        json toJson() const;
    };

    IntegrityReport checkAll();
    IntegrityReport checkFile(const std::string& path);
    IntegrityReport checkDirectory(const std::string& dir);

    // Get last report
    IntegrityReport getLastReport() const;

    // Corruption callbacks
    using CorruptionHandler = std::function<void(const std::string& path,
                                                  const std::string& error)>;
    void setCorruptionHandler(CorruptionHandler handler) {
        corruptionHandler_ = handler;
    }

    // Metrics
    json getMetrics() const;

private:
    DataIntegrityMonitor() : running_(false) {}

    void backgroundCheck();

    std::atomic<bool> running_;
    int intervalSeconds_ = 3600;
    std::thread checkThread_;
    mutable std::mutex mutex_;

    IntegrityReport lastReport_;
    CorruptionHandler corruptionHandler_;

    std::atomic<uint64_t> totalChecks_{0};
    std::atomic<uint64_t> corruptionsDetected_{0};
};

} // namespace durability
} // namespace pacificdb
