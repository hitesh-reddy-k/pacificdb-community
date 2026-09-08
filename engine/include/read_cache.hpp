#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pacificdb {

// ═══════════════════════════════════════════════════════════════════════════
// Phase C: Read Cache — LRU cache for hot keys to skip version chain traversal
// ═══════════════════════════════════════════════════════════════════════════

struct ReadCacheEntry {
    json value;
    uint64_t version;
    std::chrono::steady_clock::time_point expiresAt;
    std::chrono::steady_clock::time_point lastAccessed;
    uint32_t accessCount;
};

class ReadCache {
public:
    ReadCache(size_t maxEntries = 10000, uint32_t ttlMs = 10000)
        : maxEntries_(maxEntries), ttlMs_(ttlMs) {}

    bool get(const std::string& key, json& outValue, uint64_t& outVersion) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            misses_.fetch_add(1);
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= it->second.expiresAt) {
            cache_.erase(it);
            misses_.fetch_add(1);
            return false;
        }

        it->second.lastAccessed = now;
        it->second.accessCount++;
        outValue = it->second.value;
        outVersion = it->second.version;
        hits_.fetch_add(1);
        return true;
    }

    void put(const std::string& key, const json& value, uint64_t version) {
        std::lock_guard<std::mutex> lock(mu_);

        // Evict if at capacity (remove oldest accessed)
        if (cache_.size() >= maxEntries_) {
            evictOne();
        }

        auto now = std::chrono::steady_clock::now();
        cache_[key] = ReadCacheEntry{
            value, version,
            now + std::chrono::milliseconds(ttlMs_),
            now, 1
        };
    }

    void invalidate(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.erase(key);
        invalidations_.fetch_add(1);
    }

    void invalidateAll() {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.clear();
    }

    json getStats() const {
        return {
            {"size", cache_.size()},
            {"maxEntries", maxEntries_},
            {"ttlMs", ttlMs_},
            {"hits", hits_.load()},
            {"misses", misses_.load()},
            {"invalidations", invalidations_.load()},
            {"hitRatio", (hits_.load() + misses_.load()) > 0
                ? static_cast<double>(hits_.load()) / (hits_.load() + misses_.load())
                : 0.0}
        };
    }

private:
    void evictOne() {
        if (cache_.empty()) return;

        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.lastAccessed < oldest->second.lastAccessed) {
                oldest = it;
            }
        }
        cache_.erase(oldest);
    }

    size_t maxEntries_;
    uint32_t ttlMs_;
    std::mutex mu_;
    std::unordered_map<std::string, ReadCacheEntry> cache_;
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> invalidations_{0};
};

// ═══════════════════════════════════════════════════════════════════════════
// Phase C: Bloom Filter — fast negative lookups to skip disk reads
// ═══════════════════════════════════════════════════════════════════════════

class BloomFilter {
public:
    BloomFilter(size_t expectedItems = 10000, double falsePositiveRate = 0.01) {
        // Calculate optimal size and hash count
        double m = -(static_cast<double>(expectedItems) * std::log(falsePositiveRate))
                   / (std::log(2.0) * std::log(2.0));
        numBits_ = static_cast<size_t>(std::ceil(m));
        if (numBits_ < 64) numBits_ = 64;

        double k = (static_cast<double>(numBits_) / expectedItems) * std::log(2.0);
        numHashes_ = static_cast<size_t>(std::ceil(k));
        if (numHashes_ < 1) numHashes_ = 1;
        if (numHashes_ > 16) numHashes_ = 16;

        bits_.resize((numBits_ + 7) / 8, 0);
        itemCount_ = 0;
    }

    void add(const std::string& key) {
        for (size_t i = 0; i < numHashes_; ++i) {
            size_t pos = hash(key, i) % numBits_;
            bits_[pos / 8] |= (1u << (pos % 8));
        }
        itemCount_++;
    }

    bool mightContain(const std::string& key) const {
        for (size_t i = 0; i < numHashes_; ++i) {
            size_t pos = hash(key, i) % numBits_;
            if (!(bits_[pos / 8] & (1u << (pos % 8)))) {
                return false; // Definitely not present
            }
        }
        return true; // Might be present
    }

    void clear() {
        std::fill(bits_.begin(), bits_.end(), 0);
        itemCount_ = 0;
    }

    size_t size() const { return itemCount_; }
    size_t bitSize() const { return numBits_; }
    size_t hashCount() const { return numHashes_; }

    json getStats() const {
        double fillRatio = 0.0;
        size_t setBits = 0;
        for (auto byte : bits_) {
            for (int b = 0; b < 8; ++b) {
                if (byte & (1 << b)) setBits++;
            }
        }
        fillRatio = static_cast<double>(setBits) / numBits_;

        return {
            {"items", itemCount_},
            {"bits", numBits_},
            {"hashes", numHashes_},
            {"fillRatio", fillRatio},
            {"memoryBytes", bits_.size()}
        };
    }

private:
    // FNV-1a variant with seed for multiple hash functions
    size_t hash(const std::string& key, size_t seed) const {
        size_t h = 14695981039346656037ULL ^ seed;
        for (char c : key) {
            h ^= static_cast<size_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        return h;
    }

    size_t numBits_;
    size_t numHashes_;
    std::vector<uint8_t> bits_;
    size_t itemCount_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Phase E: HNSW Vector Index — Hierarchical Navigable Small World graph
// ═══════════════════════════════════════════════════════════════════════════

enum class DistanceMetric {
    COSINE,
    EUCLIDEAN,
    DOT_PRODUCT,
};

struct HNSWNode {
    std::string id;
    std::vector<float> vector;
    std::vector<std::vector<size_t>> neighbors; // neighbors[level] = [idx, ...]
};

struct HNSWConfig {
    size_t M = 16;             // Max connections per node per layer
    size_t efConstruction = 200; // Size of dynamic candidate list during construction
    size_t efSearch = 50;       // Size of dynamic candidate list during search
    DistanceMetric metric = DistanceMetric::COSINE;
    size_t dimensions = 0;     // Set on first insert
};

class HNSWIndex {
public:
    HNSWIndex(HNSWConfig config = {}) : config_(config), maxLevel_(0), entryPoint_(0) {}

    // Insert a vector with an ID
    bool insert(const std::string& id, const std::vector<float>& vec);

    // Search for k nearest neighbors
    std::vector<std::pair<std::string, float>> search(
        const std::vector<float>& query, size_t k) const;

    // Batch insert
    size_t batchInsert(const std::vector<std::pair<std::string, std::vector<float>>>& items);

    size_t size() const { return nodes_.size(); }
    size_t dimensions() const { return config_.dimensions; }

    json getStats() const {
        return {
            {"size", nodes_.size()},
            {"dimensions", config_.dimensions},
            {"maxLevel", maxLevel_},
            {"M", config_.M},
            {"efConstruction", config_.efConstruction},
            {"efSearch", config_.efSearch},
            {"metric", config_.metric == DistanceMetric::COSINE ? "cosine" :
                       config_.metric == DistanceMetric::EUCLIDEAN ? "euclidean" : "dot_product"}
        };
    }

private:
    float distance(const std::vector<float>& a, const std::vector<float>& b) const;
    int randomLevel() const;

    // Search from entry point at a specific level
    std::vector<std::pair<size_t, float>> searchLayer(
        const std::vector<float>& query, size_t entryIdx, size_t ef, int level) const;

    HNSWConfig config_;
    std::vector<HNSWNode> nodes_;
    mutable std::mutex mu_;
    size_t maxLevel_;
    size_t entryPoint_;
    std::unordered_map<std::string, size_t> idToIndex_;
};

} // namespace pacificdb
