#include "ttl_engine.hpp"
#include "lsm.hpp"

#include <iostream>
#include <algorithm>

namespace pacificdb {
namespace storage {

// ============================================================================
// TTL ENGINE IMPLEMENTATION
// ============================================================================

void TTLEngine::setTTL(const std::string& userId, const std::string& dbName,
                       const std::string& collection, const std::string& documentId,
                       uint64_t ttlSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    TTLEntry entry;
    entry.userId = userId;
    entry.dbName = dbName;
    entry.collection = collection;
    entry.documentId = documentId;
    entry.ttlSeconds = ttlSeconds;
    entry.expiresAt = std::chrono::system_clock::now() + std::chrono::seconds(ttlSeconds);

    std::string key = makeKey(userId, dbName, collection, documentId);
    entries_[key] = entry;
    totalTracked_.fetch_add(1, std::memory_order_relaxed);
}

void TTLEngine::removeTTL(const std::string& userId, const std::string& dbName,
                          const std::string& collection, const std::string& documentId) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(makeKey(userId, dbName, collection, documentId));
}

bool TTLEngine::isExpired(const std::string& userId, const std::string& dbName,
                          const std::string& collection, const std::string& documentId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(makeKey(userId, dbName, collection, documentId));
    if (it == entries_.end()) return false;
    return std::chrono::system_clock::now() >= it->second.expiresAt;
}

int64_t TTLEngine::getRemainingTTL(const std::string& userId, const std::string& dbName,
                                   const std::string& collection, const std::string& documentId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(makeKey(userId, dbName, collection, documentId));
    if (it == entries_.end()) return 0; // No TTL

    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        it->second.expiresAt - std::chrono::system_clock::now()).count();
    return remaining < 0 ? -1 : remaining;
}

void TTLEngine::setCollectionDefaultTTL(const std::string& userId, const std::string& dbName,
                                         const std::string& collection, uint64_t ttlSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = userId + "/" + dbName + "/" + collection;
    collectionDefaults_[key] = ttlSeconds;
}

void TTLEngine::start(int reapIntervalSeconds) {
    if (running_.exchange(true)) return;
    reapIntervalSeconds_ = reapIntervalSeconds;
    reaperThread_ = std::thread(&TTLEngine::reaperLoop, this);
    std::cout << "[TTL] Expiry engine started (interval=" << reapIntervalSeconds << "s)\n";
}

void TTLEngine::stop() {
    if (!running_.exchange(false)) return;
    if (reaperThread_.joinable()) reaperThread_.join();
    std::cout << "[TTL] Expiry engine stopped (total expired=" << totalExpired_.load() << ")\n";
}

size_t TTLEngine::reapExpired() {
    std::vector<TTLEntry> expired;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::system_clock::now();

        for (auto it = entries_.begin(); it != entries_.end(); ) {
            if (now >= it->second.expiresAt) {
                expired.push_back(it->second);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Delete expired documents via LSM tombstone
    for (const auto& entry : expired) {
        try {
            LSM::del(entry.userId, entry.dbName, entry.collection, entry.documentId);

            if (onExpiry_) {
                onExpiry_(entry.userId, entry.dbName, entry.collection, entry.documentId);
            }
        } catch (const std::exception& ex) {
            std::cerr << "[TTL] Failed to delete expired doc " << entry.documentId
                      << ": " << ex.what() << "\n";
        }
    }

    totalExpired_.fetch_add(expired.size(), std::memory_order_relaxed);
    return expired.size();
}

void TTLEngine::reaperLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(reapIntervalSeconds_));
        if (!running_.load()) break;

        size_t reaped = reapExpired();
        if (reaped > 0) {
            std::cout << "[TTL] Reaped " << reaped << " expired documents\n";
        }
    }
}

json TTLEngine::getMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        {"tracked_entries", entries_.size()},
        {"total_expired", totalExpired_.load()},
        {"total_tracked", totalTracked_.load()},
        {"collection_defaults", collectionDefaults_.size()},
        {"reap_interval_seconds", reapIntervalSeconds_},
        {"running", running_.load()}
    };
}

std::string TTLEngine::makeKey(const std::string& userId, const std::string& dbName,
                               const std::string& collection, const std::string& documentId) const {
    return userId + "/" + dbName + "/" + collection + "/" + documentId;
}

// ============================================================================
// COMPRESSION ENGINE IMPLEMENTATION
// ============================================================================

CompressionResult CompressionEngine::compress(const void* data, size_t length,
                                              CompressionType type) {
    CompressionResult result;
    result.type = type;
    result.originalSize = length;

    if (type == CompressionType::NONE || length < minCompressSize_) {
        result.data.assign(static_cast<const uint8_t*>(data),
                          static_cast<const uint8_t*>(data) + length);
        result.compressedSize = length;
        result.ratio = 1.0;
        result.type = CompressionType::NONE;
        return result;
    }

    // LZ4-style compression
    result.data = lz4Compress(data, length);
    result.compressedSize = result.data.size();
    result.ratio = static_cast<double>(result.compressedSize) / result.originalSize;

    // If compression didn't help, store uncompressed
    if (result.compressedSize >= length) {
        result.data.assign(static_cast<const uint8_t*>(data),
                          static_cast<const uint8_t*>(data) + length);
        result.compressedSize = length;
        result.ratio = 1.0;
        result.type = CompressionType::NONE;
    }

    totalCompressed_.fetch_add(1, std::memory_order_relaxed);
    bytesIn_.fetch_add(length, std::memory_order_relaxed);
    bytesOut_.fetch_add(result.compressedSize, std::memory_order_relaxed);

    return result;
}

CompressionResult CompressionEngine::compress(const std::string& data, CompressionType type) {
    return compress(data.data(), data.size(), type);
}

std::vector<uint8_t> CompressionEngine::decompress(const void* data, size_t compressedLength,
                                                   size_t originalLength, CompressionType type) {
    if (type == CompressionType::NONE) {
        return std::vector<uint8_t>(static_cast<const uint8_t*>(data),
                                   static_cast<const uint8_t*>(data) + compressedLength);
    }

    totalDecompressed_.fetch_add(1, std::memory_order_relaxed);
    return lz4Decompress(data, compressedLength, originalLength);
}

std::string CompressionEngine::decompressToString(const void* data, size_t compressedLength,
                                                  size_t originalLength, CompressionType type) {
    auto result = decompress(data, compressedLength, originalLength, type);
    return std::string(result.begin(), result.end());
}

// ============================================================================
// LZ4-STYLE FAST COMPRESSOR
// ============================================================================
// Simplified LZ4: uses a hash table for match finding, encodes
// literal runs and match copies. Compatible with our own format.

std::vector<uint8_t> CompressionEngine::lz4Compress(const void* input, size_t inputSize) {
    const uint8_t* src = static_cast<const uint8_t*>(input);
    std::vector<uint8_t> output;
    output.reserve(inputSize);

    // Hash table for match finding (4-byte matches)
    constexpr size_t HASH_SIZE = 4096;
    uint16_t hashTable[HASH_SIZE] = {};

    size_t pos = 0;
    size_t literalStart = 0;

    while (pos + 4 <= inputSize) {
        // Hash current 4 bytes
        uint32_t val = 0;
        std::memcpy(&val, src + pos, 4);
        size_t h = (val * 2654435761u) >> 20 & (HASH_SIZE - 1);

        size_t matchPos = hashTable[h];
        hashTable[h] = static_cast<uint16_t>(pos);

        // Check for match
        if (matchPos < pos && pos - matchPos < 65535 &&
            pos + 4 <= inputSize && matchPos + 4 <= inputSize &&
            std::memcmp(src + pos, src + matchPos, 4) == 0) {

            // Extend match
            size_t matchLen = 4;
            while (pos + matchLen < inputSize && src[pos + matchLen] == src[matchPos + matchLen]) {
                matchLen++;
                if (matchLen >= 255 + 15) break;
            }

            // Encode literals
            size_t literalLen = pos - literalStart;
            uint8_t token = 0;

            // Token: high 4 bits = literal length, low 4 bits = match length - 4
            token = static_cast<uint8_t>((std::min(literalLen, size_t(15)) << 4) |
                     std::min(matchLen - 4, size_t(15)));
            output.push_back(token);

            // Extended literal length
            if (literalLen >= 15) {
                size_t remaining = literalLen - 15;
                while (remaining >= 255) { output.push_back(255); remaining -= 255; }
                output.push_back(static_cast<uint8_t>(remaining));
            }

            // Literal data
            output.insert(output.end(), src + literalStart, src + literalStart + literalLen);

            // Offset (2 bytes, little-endian)
            uint16_t offset = static_cast<uint16_t>(pos - matchPos);
            output.push_back(offset & 0xFF);
            output.push_back((offset >> 8) & 0xFF);

            // Extended match length
            if (matchLen - 4 >= 15) {
                size_t remaining = matchLen - 4 - 15;
                while (remaining >= 255) { output.push_back(255); remaining -= 255; }
                output.push_back(static_cast<uint8_t>(remaining));
            }

            pos += matchLen;
            literalStart = pos;
        } else {
            pos++;
        }
    }

    // Final literals
    size_t literalLen = inputSize - literalStart;
    if (literalLen > 0) {
        uint8_t token = static_cast<uint8_t>(std::min(literalLen, size_t(15)) << 4);
        output.push_back(token);
        if (literalLen >= 15) {
            size_t remaining = literalLen - 15;
            while (remaining >= 255) { output.push_back(255); remaining -= 255; }
            output.push_back(static_cast<uint8_t>(remaining));
        }
        output.insert(output.end(), src + literalStart, src + inputSize);
    }

    return output;
}

std::vector<uint8_t> CompressionEngine::lz4Decompress(const void* input, size_t compressedLength,
                                                      size_t originalLength) {
    const uint8_t* src = static_cast<const uint8_t*>(input);
    std::vector<uint8_t> output(originalLength);
    size_t srcPos = 0, dstPos = 0;

    while (srcPos < compressedLength && dstPos < originalLength) {
        uint8_t token = src[srcPos++];
        size_t literalLen = (token >> 4) & 0x0F;
        size_t matchLen = (token & 0x0F) + 4;

        // Extended literal length
        if (literalLen == 15) {
            while (srcPos < compressedLength) {
                uint8_t b = src[srcPos++];
                literalLen += b;
                if (b < 255) break;
            }
        }

        // Copy literals
        if (literalLen > 0 && srcPos + literalLen <= compressedLength &&
            dstPos + literalLen <= originalLength) {
            std::memcpy(output.data() + dstPos, src + srcPos, literalLen);
            srcPos += literalLen;
            dstPos += literalLen;
        } else {
            // Copy remaining
            size_t toCopy = std::min(literalLen,
                            std::min(compressedLength - srcPos, originalLength - dstPos));
            if (toCopy > 0) std::memcpy(output.data() + dstPos, src + srcPos, toCopy);
            break;
        }

        // Check if there's a match
        if (srcPos + 2 > compressedLength) break;

        uint16_t offset = src[srcPos] | (src[srcPos + 1] << 8);
        srcPos += 2;

        // Extended match length
        if ((token & 0x0F) == 15) {
            while (srcPos < compressedLength) {
                uint8_t b = src[srcPos++];
                matchLen += b;
                if (b < 255) break;
            }
        }

        // Copy match (may overlap)
        if (offset > 0 && dstPos >= offset) {
            size_t matchStart = dstPos - offset;
            for (size_t i = 0; i < matchLen && dstPos < originalLength; i++) {
                output[dstPos++] = output[matchStart + i];
            }
        }
    }

    output.resize(dstPos);
    return output;
}

json CompressionEngine::getMetrics() const {
    uint64_t in = bytesIn_.load();
    uint64_t out = bytesOut_.load();
    return {
        {"total_compressed", totalCompressed_.load()},
        {"total_decompressed", totalDecompressed_.load()},
        {"bytes_in", in},
        {"bytes_out", out},
        {"overall_ratio", in > 0 ? static_cast<double>(out) / in : 1.0},
        {"min_compress_size", minCompressSize_},
        {"default_type", static_cast<int>(defaultType_)}
    };
}

} // namespace storage
} // namespace pacificdb
