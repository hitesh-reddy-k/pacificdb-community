#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pacificdb {
namespace storage {

// ============================================================================
// TTL EXPIRY ENGINE
// ============================================================================
// Manages time-to-live for documents. Background reaper thread scans
// and removes expired entries, integrating with LSM tombstone writes.

struct TTLEntry {
    std::string userId;
    std::string dbName;
    std::string collection;
    std::string documentId;
    std::chrono::system_clock::time_point expiresAt;
    uint64_t ttlSeconds;
};

class TTLEngine {
public:
    static TTLEngine& instance() {
        static TTLEngine inst;
        return inst;
    }

    // Set TTL on a document (absolute expiry)
    void setTTL(const std::string& userId, const std::string& dbName,
                const std::string& collection, const std::string& documentId,
                uint64_t ttlSeconds);

    // Remove TTL (make document permanent)
    void removeTTL(const std::string& userId, const std::string& dbName,
                   const std::string& collection, const std::string& documentId);

    // Check if a document is expired
    bool isExpired(const std::string& userId, const std::string& dbName,
                   const std::string& collection, const std::string& documentId) const;

    // Get remaining TTL in seconds (0 = no TTL, -1 = expired)
    int64_t getRemainingTTL(const std::string& userId, const std::string& dbName,
                            const std::string& collection, const std::string& documentId) const;

    // Set default TTL for a collection
    void setCollectionDefaultTTL(const std::string& userId, const std::string& dbName,
                                  const std::string& collection, uint64_t ttlSeconds);

    // Lifecycle
    void start(int reapIntervalSeconds = 10);
    void stop();
    bool isRunning() const { return running_.load(); }

    // Manual reap (for testing)
    size_t reapExpired();

    // Callback for expiry (called when doc is reaped)
    using ExpiryCallback = std::function<void(const std::string& userId,
                                               const std::string& dbName,
                                               const std::string& collection,
                                               const std::string& documentId)>;
    void setExpiryCallback(ExpiryCallback cb) { onExpiry_ = cb; }

    // Metrics
    json getMetrics() const;

private:
    TTLEngine() : running_(false), totalExpired_(0), totalTracked_(0) {}
    ~TTLEngine() { stop(); }

    void reaperLoop();
    std::string makeKey(const std::string& userId, const std::string& dbName,
                        const std::string& collection, const std::string& documentId) const;

    std::unordered_map<std::string, TTLEntry> entries_;
    std::unordered_map<std::string, uint64_t> collectionDefaults_; // collection -> default TTL
    mutable std::mutex mutex_;

    std::atomic<bool> running_;
    int reapIntervalSeconds_ = 10;
    std::thread reaperThread_;

    ExpiryCallback onExpiry_;

    std::atomic<uint64_t> totalExpired_;
    std::atomic<uint64_t> totalTracked_;
};

// ============================================================================
// COMPRESSION ENGINE
// ============================================================================
// LZ4-compatible fast compression for SSTable blocks, WAL entries,
// and network payloads. Uses a simple byte-level compressor when
// LZ4 library is not available.

enum class CompressionType : uint8_t {
    NONE = 0,
    LZ4  = 1,
    SNAPPY = 2,  // Placeholder
    ZSTD = 3     // Placeholder
};

struct CompressionResult {
    std::vector<uint8_t> data;
    CompressionType type;
    size_t originalSize;
    size_t compressedSize;
    double ratio;  // compressed / original
};

class CompressionEngine {
public:
    static CompressionEngine& instance() {
        static CompressionEngine inst;
        return inst;
    }

    // Compress data
    CompressionResult compress(const void* data, size_t length,
                               CompressionType type = CompressionType::LZ4);
    CompressionResult compress(const std::string& data,
                               CompressionType type = CompressionType::LZ4);

    // Decompress data (type must match what was used to compress)
    std::vector<uint8_t> decompress(const void* data, size_t compressedLength,
                                    size_t originalLength, CompressionType type);
    std::string decompressToString(const void* data, size_t compressedLength,
                                   size_t originalLength, CompressionType type);

    // Check if compression is worthwhile (small data may not benefit)
    bool shouldCompress(size_t dataSize) const { return dataSize >= minCompressSize_; }

    // Config
    void setMinCompressSize(size_t bytes) { minCompressSize_ = bytes; }
    void setDefaultType(CompressionType type) { defaultType_ = type; }
    CompressionType getDefaultType() const { return defaultType_; }

    // Metrics
    json getMetrics() const;

private:
    CompressionEngine() : minCompressSize_(64), defaultType_(CompressionType::LZ4),
                          totalCompressed_(0), totalDecompressed_(0),
                          bytesIn_(0), bytesOut_(0) {}

    // LZ4-style fast compressor (simplified implementation)
    std::vector<uint8_t> lz4Compress(const void* data, size_t length);
    std::vector<uint8_t> lz4Decompress(const void* data, size_t compressedLength,
                                       size_t originalLength);

    size_t minCompressSize_;
    CompressionType defaultType_;

    std::atomic<uint64_t> totalCompressed_;
    std::atomic<uint64_t> totalDecompressed_;
    std::atomic<uint64_t> bytesIn_;
    std::atomic<uint64_t> bytesOut_;
};

} // namespace storage
} // namespace pacificdb
