#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include <mutex>
#include <unordered_map>

enum class WalOp : uint8_t {
    INSERT = 1,
    UPDATE = 2,
    DELETE = 3,
    BATCH_COMPRESSED = 4,   // Phase 1: compressed batch entry
    COMPRESSED_ZLIB = 5,    // Phase B: zlib-compressed single batch
    SEGMENT_HEADER  = 6,    // Phase B: segment boundary marker
};

// Phase 1 + B: WAL statistics for metrics export and backpressure signaling
struct WalStats {
    std::atomic<uint64_t> entriesWritten{0};
    std::atomic<uint64_t> entriesFsynced{0};
    std::atomic<uint64_t> batchesCommitted{0};
    std::atomic<uint64_t> bytesWritten{0};
    std::atomic<uint64_t> pendingEntries{0};
    std::atomic<uint64_t> entriesAcknowledged{0};
    std::atomic<uint64_t> entriesFailed{0};
    std::atomic<double>   avgFlushLatencyMs{0.0};

    // Phase B: Compression metrics
    std::atomic<uint64_t> bytesBeforeCompression{0};
    std::atomic<uint64_t> bytesAfterCompression{0};
    std::atomic<uint64_t> compressedBatches{0};
    std::atomic<uint64_t> uncompressedBatches{0};

    // Phase B: Segment metrics
    std::atomic<uint64_t> segmentsCreated{0};
    std::atomic<uint64_t> segmentsCompacted{0};
    std::atomic<uint64_t> activeSegments{1};
};

struct WalAppendResult {
    uint64_t firstLsn{0};
    uint64_t lastLsn{0};
    size_t entries{0};
};

enum class WalScanStatus { OK, PARTIAL_TAIL, CORRUPT, IO_ERROR };

struct WalReplayRecord {
    WalOp op;
    std::uint64_t lsn;
    nlohmann::json entry;
};

struct WalScanResult {
    WalScanStatus status{WalScanStatus::OK};
    std::uint64_t records{0};
    std::uint64_t lastLsn{0};
    std::uint64_t validBytes{0};
    std::string error;
};

using WalVisitor = std::function<bool(const WalReplayRecord&)>;

// Phase B: WAL Segment tracking
struct WalSegment {
    uint64_t segmentId;
    uint64_t startLSN;
    uint64_t endLSN;
    uint64_t bytesWritten;
    uint64_t createdAt;    // unix timestamp
    std::string filePath;
    bool active;
};

class WAL {
public:
    // Append entry to WAL (may be buffered and flushed by background flusher)
    static WalAppendResult log(const std::string& file,
                               const nlohmann::json& entry);

    // Append a logical batch to one WAL file. This keeps bulk writes from
    // reopening the same WAL for every document when group commit is disabled.
    static WalAppendResult logBatch(const std::string& file,
                                    const std::vector<nlohmann::json>& entries);

    // Initialize the WAL subsystem (starts background flusher)
    static void init();

    // Shutdown the WAL subsystem (flushes buffers)
    static void shutdown();

    // Flush buffered WAL entries for a specific file
    static void flush(const std::string& file, bool forceFsync = false);

    static std::vector<std::string>
    readAll(const std::string& walFile);

    static WalScanResult scan(const std::string& logicalWal, const WalVisitor& visitor);
    static bool reclaimThrough(const std::string& logicalWal, std::uint64_t coveredLsn,
                               std::string* error = nullptr);

    static void replay(const std::string& file);
    static void clear(const std::string& file);

    // Phase 1: Stats for metrics export
    static WalStats& getStats();

    // Phase 1: Pending entry count for backpressure signaling
    static size_t getPendingCount();

    // Phase B: Compression utilities (lightweight RLE + varint, no external deps)
    static std::string compressPayload(const std::string& input);
    static std::string decompressPayload(const std::string& input);

    // Phase B: Segment management
    static uint64_t getCurrentLSN();
    static uint64_t getSegmentCount();
    static uint64_t getSegmentCount(const std::string& logicalWal);
};
