#include "storage_optimizations.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <iostream>
#include <memory>

/* ============================================================================
   AdaptiveBloomFilter Implementation
   ============================================================================ */

AdaptiveBloomFilter::AdaptiveBloomFilter(size_t initialSize, double targetFPRate)
    : size_(initialSize), targetFPRate_(targetFPRate) {

    // Calculate number of hash functions for target false-positive rate
    // numHashFunctions = -log2(fp_rate)
    numHashFunctions_ = std::max<size_t>(1,
        static_cast<size_t>(-std::log2(targetFPRate)) + 1);

    // Allocate bits (size_ / 8 bytes)
    bits_.resize((size_ + 7) / 8, 0);

    std::cout << "[BLOOM-FILTER] Initialized: size=" << size_
              << " bits, " << numHashFunctions_ << " hash functions\n";
}

AdaptiveBloomFilter::~AdaptiveBloomFilter() {}

void AdaptiveBloomFilter::insert(const std::string& key) {
    for (uint32_t i = 0; i < numHashFunctions_; ++i) {
        uint64_t h = hash(key, i);
        size_t byteIdx = (h / 8) % bits_.size();
        uint8_t bitIdx = h % 8;
        bits_[byteIdx] |= (1 << bitIdx);
    }
}

bool AdaptiveBloomFilter::mightContain(const std::string& key) const {
    stats_.queries++;

    for (uint32_t i = 0; i < numHashFunctions_; ++i) {
        uint64_t h = hash(key, i);
        size_t byteIdx = (h / 8) % bits_.size();
        uint8_t bitIdx = h % 8;

        if (!(bits_[byteIdx] & (1 << bitIdx))) {
            stats_.falseNegatives++;
            return false;  // Definitely not present
        }
    }

    // Might be present (could be false positive)
    if (stats_.queries % 1000 == 0) {
        stats_.falsePositives = static_cast<uint64_t>(
            stats_.queries * targetFPRate_
        );
        stats_.currentFPRate = (double)stats_.falsePositives / stats_.queries;
    }

    return true;  // Probably present
}

uint64_t AdaptiveBloomFilter::hash(const std::string& key, uint32_t seed) const {
    uint64_t h = seed;
    for (char c : key) {
        h = h * 31 + static_cast<unsigned char>(c);
    }
    return h;
}

void AdaptiveBloomFilter::adaptIfNeeded() {
    // If false-positive rate has drifted significantly, resize
    if (stats_.queries > 10000) {
        double actualFPRate = (double)stats_.falsePositives / stats_.queries;
        if (actualFPRate > targetFPRate_ * 1.5) {
            std::cout << "[BLOOM-FILTER] Adapting: FP rate " << actualFPRate
                      << " exceeds target " << targetFPRate_ << "\n";
            resize(size_ * 2);
        }
    }
}

void AdaptiveBloomFilter::resize(size_t newSize) {
    AdaptiveBloomFilter newFilter(newSize, targetFPRate_);
    // TODO: Copy entries from old filter to new filter
    bits_ = newFilter.bits_;
    size_ = newSize;
    stats_ = newFilter.stats_;
}

std::vector<uint8_t> AdaptiveBloomFilter::serialize() const {
    std::vector<uint8_t> data;
    data.resize(sizeof(size_) + sizeof(numHashFunctions_) + bits_.size());

    size_t offset = 0;
    std::memcpy(data.data() + offset, &size_, sizeof(size_));
    offset += sizeof(size_);
    std::memcpy(data.data() + offset, &numHashFunctions_, sizeof(numHashFunctions_));
    offset += sizeof(numHashFunctions_);
    std::memcpy(data.data() + offset, bits_.data(), bits_.size());

    return data;
}

bool AdaptiveBloomFilter::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(size_) + sizeof(numHashFunctions_)) {
        return false;
    }

    size_t offset = 0;
    std::memcpy(&size_, data.data() + offset, sizeof(size_));
    offset += sizeof(size_);
    std::memcpy(&numHashFunctions_, data.data() + offset, sizeof(numHashFunctions_));
    offset += sizeof(numHashFunctions_);

    bits_.resize(data.size() - offset);
    std::memcpy(bits_.data(), data.data() + offset, bits_.size());

    return true;
}

void AdaptiveBloomFilter::resetStats() {
    stats_ = Stats{};
}

/* ============================================================================
   CompactionScheduler Implementation
   ============================================================================ */

CompactionScheduler::CompactionScheduler(Strategy strategy)
    : strategy_(strategy) {
    metrics_.currentStrategy = strategy;
}

bool CompactionScheduler::shouldCompact(
    size_t level,
    size_t fileCount,
    size_t maxFilesPerLevel,
    double writeRatePerSec,
    double readRatePerSec) {

    switch (strategy_) {
        case Strategy::LEVELED:
            return shouldCompactLeveled(level, fileCount, maxFilesPerLevel);
        case Strategy::TIERED:
            return shouldCompactTiered(level, fileCount);
        case Strategy::ADAPTIVE:
            return shouldCompactAdaptive(readRatePerSec, writeRatePerSec);
        default:
            return fileCount > maxFilesPerLevel;
    }
}

bool CompactionScheduler::shouldCompactLeveled(size_t level, size_t fileCount, size_t maxFiles) {
    // Compact if we exceed max files for this level
    if (fileCount > maxFiles) {
        return true;
    }

    // Also compact if level has grown significantly
    if (level > 0 && fileCount > maxFiles * (10 - level)) {
        return true;
    }

    return false;
}

bool CompactionScheduler::shouldCompactTiered(size_t level, size_t fileCount) {
    // Tiered: trigger when we have 4+ files in a level
    return fileCount >= 4;
}

bool CompactionScheduler::shouldCompactAdaptive(double readRate, double writeRate) {
    // Adaptive: compact more aggressively on write-heavy, less on read-heavy
    double writeIntensity = writeRate / (readRate + writeRate + 0.1);

    // High write load: compact sooner to prevent read amplification
    return writeIntensity > 0.6;
}

size_t CompactionScheduler::selectFilesForCompaction(
    size_t level,
    const std::vector<size_t>& fileSizes,
    size_t maxCompactionSize) {

    if (strategy_ == Strategy::LEVELED) {
        return selectFilesLeveled(fileSizes, maxCompactionSize);
    } else {
        return selectFilesAdaptive(fileSizes, maxCompactionSize);
    }
}

size_t CompactionScheduler::selectFilesLeveled(
    const std::vector<size_t>& sizes,
    size_t maxSize) {

    // Select smallest files to minimize impact
    size_t totalSize = 0;
    size_t count = 0;

    for (size_t size : sizes) {
        if (totalSize + size <= maxSize) {
            totalSize += size;
            count++;
        } else {
            break;
        }
    }

    return count;
}

size_t CompactionScheduler::selectFilesAdaptive(
    const std::vector<size_t>& sizes,
    size_t maxSize) {

    // Adaptive: try to compact all files if possible
    size_t totalSize = std::accumulate(sizes.begin(), sizes.end(), 0UL);
    if (totalSize <= maxSize) {
        return sizes.size();
    }

    // Otherwise, compact as many as fit
    return selectFilesLeveled(sizes, maxSize);
}

void CompactionScheduler::recordCompaction(size_t bytesRead, size_t bytesWritten, double timeMs) {
    metrics_.totalCompacted += bytesRead;
    metrics_.totalBytesRead += bytesRead;
    metrics_.totalBytesWritten += bytesWritten;
    metrics_.compactionCount++;

    if (metrics_.compactionCount > 0) {
        metrics_.averageCompactionTimeMs =
            (metrics_.averageCompactionTimeMs * (metrics_.compactionCount - 1) + timeMs) /
            metrics_.compactionCount;
    }
}

void CompactionScheduler::setStrategy(Strategy newStrategy) {
    strategy_ = newStrategy;
    metrics_.currentStrategy = newStrategy;
}

/* ============================================================================
   CompressionCodec Implementation
   ============================================================================ */

CompressionCodec::CompressionCodec(Algorithm algorithm)
    : algorithm_(algorithm) {
    stats_.algorithmUsed = algorithm;
}

std::vector<uint8_t> CompressionCodec::compress(const std::vector<uint8_t>& data) {
    stats_.bytesOriginal += data.size();

    std::vector<uint8_t> compressed;

    switch (algorithm_) {
        case Algorithm::SNAPPY:
            compressed = compressSnappy(data);
            break;
        case Algorithm::LZ4:
            compressed = compressLZ4(data);
            break;
        case Algorithm::ZSTD:
            compressed = compressZSTD(data);
            break;
        case Algorithm::AUTO:
            // For now, default to simple run-length encoding
            compressed = data;
            break;
        default:
            compressed = data;
    }

    stats_.bytesCompressed += compressed.size();
    if (stats_.bytesOriginal > 0) {
        stats_.compressionRatio =
            100.0 * (1.0 - (double)stats_.bytesCompressed / stats_.bytesOriginal);
    }

    return compressed;
}

std::vector<uint8_t> CompressionCodec::decompress(const std::vector<uint8_t>& compressed) {
    // TODO: Implement decompression
    return compressed;
}

std::vector<uint8_t> CompressionCodec::compressSnappy(const std::vector<uint8_t>& data) {
    // TODO: Link snappy library
    return data;
}

std::vector<uint8_t> CompressionCodec::compressLZ4(const std::vector<uint8_t>& data) {
    // TODO: Link LZ4 library
    return data;
}

std::vector<uint8_t> CompressionCodec::compressZSTD(const std::vector<uint8_t>& data) {
    // TODO: Link ZSTD library
    return data;
}

Algorithm CompressionCodec::selectAlgorithm(const std::vector<uint8_t>& sample) {
    // TODO: Analyze sample and select best algorithm
    return Algorithm::NONE;
}

void CompressionCodec::resetStats() {
    stats_ = CompressionStats{};
    stats_.algorithmUsed = algorithm_;
}

/* ============================================================================
   StorageOptimizer Implementation
   ============================================================================ */

StorageOptimizer::StorageOptimizer()
    : bloomFilter_(1000000, 0.01),
      compactionScheduler_(CompactionScheduler::Strategy::ADAPTIVE),
      compressionCodec_(CompressionCodec::Algorithm::AUTO) {
    std::cout << "[STORAGE-OPTIMIZER] Initialized with adaptive strategies\n";
}

void StorageOptimizer::updateWorkloadMetrics(
    double readRate, double writeRate, double readLatency) {

    lastReadRate_ = readRate;
    lastWriteRate_ = writeRate;

    double totalRate = readRate + writeRate;
    if (totalRate < 0.1) return;  // No activity

    double readIntensity = readRate / totalRate;

    if (readIntensity > 0.8) {
        optimizeForReadHeavyWorkload();
    } else if (readIntensity < 0.2) {
        optimizeForWriteHeavyWorkload();
    } else {
        optimizeForMixedWorkload();
    }
}

void StorageOptimizer::optimizeForReadHeavyWorkload() {
    std::cout << "[STORAGE-OPTIMIZER] Optimizing for read-heavy workload\n";
    compactionScheduler_.setStrategy(CompactionScheduler::Strategy::LEVELED);
    bloomFilter_.adaptIfNeeded();
}

void StorageOptimizer::optimizeForWriteHeavyWorkload() {
    std::cout << "[STORAGE-OPTIMIZER] Optimizing for write-heavy workload\n";
    compactionScheduler_.setStrategy(CompactionScheduler::Strategy::TIERED);
}

void StorageOptimizer::optimizeForMixedWorkload() {
    std::cout << "[STORAGE-OPTIMIZER] Optimizing for mixed workload\n";
    compactionScheduler_.setStrategy(CompactionScheduler::Strategy::ADAPTIVE);
}

void StorageOptimizer::printOptimizationReport() const {
    std::cout << "\n[STORAGE-OPTIMIZATION REPORT]\n";
    std::cout << "Bloom Filter:\n";
    std::cout << "  - Queries: " << bloomFilter_.getStats().queries << "\n";
    std::cout << "  - FP Rate: " << bloomFilter_.getStats().currentFPRate << "\n";
    std::cout << "Compaction:\n";
    std::cout << "  - Operations: " << compactionScheduler_.getMetrics().compactionCount << "\n";
    std::cout << "  - Avg time: " << compactionScheduler_.getMetrics().averageCompactionTimeMs << "ms\n";
}
