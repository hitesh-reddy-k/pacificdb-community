#include "data_durability.hpp"
#include "storage_path.hpp"
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>

// Use built-in SHA-256 implementation instead of OpenSSL
#ifdef HAS_OPENSSL
#include <openssl/sha.h>
#include <openssl/evp.h>
#else
// Simple SHA-256 implementation for standalone builds
#define SHA256_DIGEST_LENGTH 32
namespace {
    // SHA-256 constants
    static const uint32_t sha256_k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    inline uint32_t rotr(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }
}
#endif

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace pacificdb {
namespace durability {

namespace fs = std::filesystem;

// ============================================================================
// CRC32 TABLE
// ============================================================================

static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void initCRC32Table() {
    if (crc32_table_initialized) return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

// ============================================================================
// CHECKSUM CALCULATOR IMPLEMENTATION
// ============================================================================

uint32_t ChecksumCalculator::crc32(const void* data, size_t length) {
    initCRC32Table();

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ bytes[i]) & 0xFF];
    }

    return crc ^ 0xFFFFFFFF;
}

uint32_t ChecksumCalculator::crc32c(const void* data, size_t length) {
    // TODO: Use hardware-accelerated CRC32C if available
    // For now, fall back to regular CRC32
    return crc32(data, length);
}

uint64_t ChecksumCalculator::xxhash64(const void* data, size_t length, uint64_t seed) {
    // Simplified xxHash64 implementation
    const uint64_t PRIME64_1 = 11400714785074694791ULL;
    const uint64_t PRIME64_2 = 14029467366897019727ULL;
    const uint64_t PRIME64_3 = 1609587929392839161ULL;
    const uint64_t PRIME64_4 = 9650029242287828579ULL;
    const uint64_t PRIME64_5 = 2870177450012600261ULL;

    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + length;
    uint64_t h64;

    if (length >= 32) {
        const uint8_t* limit = end - 32;
        uint64_t v1 = seed + PRIME64_1 + PRIME64_2;
        uint64_t v2 = seed + PRIME64_2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - PRIME64_1;

        do {
            uint64_t k1, k2, k3, k4;
            memcpy(&k1, p, 8); p += 8;
            memcpy(&k2, p, 8); p += 8;
            memcpy(&k3, p, 8); p += 8;
            memcpy(&k4, p, 8); p += 8;

            v1 += k1 * PRIME64_2;
            v1 = (v1 << 31) | (v1 >> 33);
            v1 *= PRIME64_1;

            v2 += k2 * PRIME64_2;
            v2 = (v2 << 31) | (v2 >> 33);
            v2 *= PRIME64_1;

            v3 += k3 * PRIME64_2;
            v3 = (v3 << 31) | (v3 >> 33);
            v3 *= PRIME64_1;

            v4 += k4 * PRIME64_2;
            v4 = (v4 << 31) | (v4 >> 33);
            v4 *= PRIME64_1;
        } while (p <= limit);

        h64 = ((v1 << 1) | (v1 >> 63)) + ((v2 << 7) | (v2 >> 57)) +
              ((v3 << 12) | (v3 >> 52)) + ((v4 << 18) | (v4 >> 46));

        // Merge state
        auto mergeState = [&](uint64_t v) {
            v *= PRIME64_2;
            v = (v << 31) | (v >> 33);
            v *= PRIME64_1;
            h64 ^= v;
            h64 = h64 * PRIME64_1 + PRIME64_4;
        };

        mergeState(v1);
        mergeState(v2);
        mergeState(v3);
        mergeState(v4);
    } else {
        h64 = seed + PRIME64_5;
    }

    h64 += length;

    // Process remaining bytes
    while (p + 8 <= end) {
        uint64_t k;
        memcpy(&k, p, 8);
        k *= PRIME64_2;
        k = (k << 31) | (k >> 33);
        k *= PRIME64_1;
        h64 ^= k;
        h64 = ((h64 << 27) | (h64 >> 37)) * PRIME64_1 + PRIME64_4;
        p += 8;
    }

    while (p + 4 <= end) {
        uint32_t k;
        memcpy(&k, p, 4);
        h64 ^= k * PRIME64_1;
        h64 = ((h64 << 23) | (h64 >> 41)) * PRIME64_2 + PRIME64_3;
        p += 4;
    }

    while (p < end) {
        h64 ^= (*p++) * PRIME64_5;
        h64 = ((h64 << 11) | (h64 >> 53)) * PRIME64_1;
    }

    // Final mix
    h64 ^= h64 >> 33;
    h64 *= PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= PRIME64_3;
    h64 ^= h64 >> 32;

    return h64;
}

// Standalone SHA-256 implementation (no OpenSSL required)
namespace {
    void sha256_transform_block(uint32_t* state, const uint8_t* block) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
        auto ch = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); };
        auto maj = [](uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); };
        auto sig0 = [rotr](uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); };
        auto sig1 = [rotr](uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); };
        auto gam0 = [rotr](uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); };
        auto gam1 = [rotr](uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); };

        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(block[i*4]) << 24) |
                   (static_cast<uint32_t>(block[i*4+1]) << 16) |
                   (static_cast<uint32_t>(block[i*4+2]) << 8) |
                   (static_cast<uint32_t>(block[i*4+3]));
        }
        for (int i = 16; i < 64; i++) {
            w[i] = gam1(w[i-2]) + w[i-7] + gam0(w[i-15]) + w[i-16];
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + sig1(e) + ch(e, f, g) + k[i] + w[i];
            uint32_t t2 = sig0(a) + maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
}

std::string ChecksumCalculator::sha256(const void* data, size_t length) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t bitlen = length * 8;

    // Prepare padded buffer
    size_t padlen = ((length + 8) / 64 + 1) * 64;
    std::vector<uint8_t> padded(padlen, 0);
    memcpy(padded.data(), bytes, length);
    padded[length] = 0x80;

    // Append length in big-endian
    for (int i = 0; i < 8; i++) {
        padded[padlen - 1 - i] = static_cast<uint8_t>((bitlen >> (i * 8)) & 0xFF);
    }

    // Process blocks
    for (size_t i = 0; i < padlen; i += 64) {
        sha256_transform_block(state, padded.data() + i);
    }

    // Convert to hex string
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 8; i++) {
        char buf[9];
        snprintf(buf, sizeof(buf), "%08x", state[i]);
        result += buf;
    }

    return result;
}

std::string ChecksumCalculator::sha256File(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) return {};
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    char buffer[1024 * 1024];
    while (ok && input) {
        input.read(buffer, sizeof(buffer));
        const auto bytes = input.gcount();
        if (bytes > 0) ok = EVP_DigestUpdate(context, buffer, static_cast<size_t>(bytes)) == 1;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;
    ok = ok && input.eof() &&
        EVP_DigestFinal_ex(context, digest, &digestLength) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(digestLength * 2, '0');
    for (unsigned int i = 0; i < digestLength; ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return result;
}

uint64_t ChecksumCalculator::murmur3(const void* data, size_t length, uint32_t seed) {
    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const size_t nblocks = length / 16;

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    // Body
    const uint64_t* blocks = reinterpret_cast<const uint64_t*>(bytes);
    for (size_t i = 0; i < nblocks; i++) {
        uint64_t k1 = blocks[i * 2];
        uint64_t k2 = blocks[i * 2 + 1];

        k1 *= c1;
        k1 = (k1 << 31) | (k1 >> 33);
        k1 *= c2;
        h1 ^= k1;

        h1 = (h1 << 27) | (h1 >> 37);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2 = (k2 << 33) | (k2 >> 31);
        k2 *= c1;
        h2 ^= k2;

        h2 = (h2 << 31) | (h2 >> 33);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }

    // Tail
    const uint8_t* tail = bytes + nblocks * 16;
    uint64_t k1 = 0;
    uint64_t k2 = 0;

    switch (length & 15) {
        case 15: k2 ^= uint64_t(tail[14]) << 48; [[fallthrough]];
        case 14: k2 ^= uint64_t(tail[13]) << 40; [[fallthrough]];
        case 13: k2 ^= uint64_t(tail[12]) << 32; [[fallthrough]];
        case 12: k2 ^= uint64_t(tail[11]) << 24; [[fallthrough]];
        case 11: k2 ^= uint64_t(tail[10]) << 16; [[fallthrough]];
        case 10: k2 ^= uint64_t(tail[9]) << 8; [[fallthrough]];
        case 9: k2 ^= uint64_t(tail[8]);
            k2 *= c2;
            k2 = (k2 << 33) | (k2 >> 31);
            k2 *= c1;
            h2 ^= k2;
            [[fallthrough]];
        case 8: k1 ^= uint64_t(tail[7]) << 56; [[fallthrough]];
        case 7: k1 ^= uint64_t(tail[6]) << 48; [[fallthrough]];
        case 6: k1 ^= uint64_t(tail[5]) << 40; [[fallthrough]];
        case 5: k1 ^= uint64_t(tail[4]) << 32; [[fallthrough]];
        case 4: k1 ^= uint64_t(tail[3]) << 24; [[fallthrough]];
        case 3: k1 ^= uint64_t(tail[2]) << 16; [[fallthrough]];
        case 2: k1 ^= uint64_t(tail[1]) << 8; [[fallthrough]];
        case 1: k1 ^= uint64_t(tail[0]);
            k1 *= c1;
            k1 = (k1 << 31) | (k1 >> 33);
            k1 *= c2;
            h1 ^= k1;
    }

    // Finalization
    h1 ^= length;
    h2 ^= length;
    h1 += h2;
    h2 += h1;

    auto fmix64 = [](uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    };

    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;

    return h1;
}

uint32_t ChecksumCalculator::crc32(const std::string& data) {
    return crc32(data.data(), data.size());
}

uint64_t ChecksumCalculator::xxhash64(const std::string& data) {
    return xxhash64(data.data(), data.size());
}

ChecksumCalculator::IncrementalCRC32::IncrementalCRC32() : state_(0xFFFFFFFF) {
    initCRC32Table();
}

void ChecksumCalculator::IncrementalCRC32::update(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; i++) {
        state_ = (state_ >> 8) ^ crc32_table[(state_ ^ bytes[i]) & 0xFF];
    }
}

uint32_t ChecksumCalculator::IncrementalCRC32::finalize() {
    return state_ ^ 0xFFFFFFFF;
}

void ChecksumCalculator::IncrementalCRC32::reset() {
    state_ = 0xFFFFFFFF;
}

// ============================================================================
// DATA BLOCK IMPLEMENTATION
// ============================================================================

DataBlock::DataBlock(ChecksumType checksumType) : checksumType_(checksumType) {
    memset(&header_, 0, sizeof(header_));
    header_.magic = BLOCK_MAGIC;
    header_.version = BLOCK_VERSION;
    header_.checksumType = static_cast<uint8_t>(checksumType);
}

bool DataBlock::write(const void* data, size_t length) {
    data_.resize(length);
    memcpy(data_.data(), data, length);

    header_.dataSize = static_cast<uint32_t>(length);
    header_.blockSize = static_cast<uint32_t>(sizeof(BlockHeader) + length);
    header_.checksum = calculateChecksum();

    return true;
}

bool DataBlock::write(const std::string& data) {
    return write(data.data(), data.size());
}

std::optional<std::string> DataBlock::read() {
    if (!verify()) {
        return std::nullopt;
    }
    return std::string(data_.begin(), data_.end());
}

bool DataBlock::verify() const {
    if (header_.magic != BLOCK_MAGIC) return false;
    if (header_.version != BLOCK_VERSION) return false;
    return verifyChecksum();
}

bool DataBlock::verifyChecksum() const {
    uint32_t computed = const_cast<DataBlock*>(this)->calculateChecksum();
    return computed == header_.checksum;
}

uint32_t DataBlock::calculateChecksum() const {
    switch (checksumType_) {
        case ChecksumType::CRC32:
        case ChecksumType::CRC32C:
            return ChecksumCalculator::crc32(data_.data(), data_.size());
        case ChecksumType::XXHASH64:
            return static_cast<uint32_t>(
                ChecksumCalculator::xxhash64(data_.data(), data_.size()) & 0xFFFFFFFF);
        case ChecksumType::MURMUR3:
            return static_cast<uint32_t>(
                ChecksumCalculator::murmur3(data_.data(), data_.size()) & 0xFFFFFFFF);
        default:
            return ChecksumCalculator::crc32(data_.data(), data_.size());
    }
}

std::vector<uint8_t> DataBlock::serialize() const {
    std::vector<uint8_t> result(sizeof(BlockHeader) + data_.size());
    memcpy(result.data(), &header_, sizeof(BlockHeader));
    if (!data_.empty()) {
        memcpy(result.data() + sizeof(BlockHeader), data_.data(), data_.size());
    }
    return result;
}

bool DataBlock::deserialize(const std::vector<uint8_t>& bytes) {
    return deserialize(bytes.data(), bytes.size());
}

bool DataBlock::deserialize(const void* data, size_t length) {
    if (length < sizeof(BlockHeader)) return false;

    memcpy(&header_, data, sizeof(BlockHeader));

    if (header_.magic != BLOCK_MAGIC) return false;
    if (length < sizeof(BlockHeader) + header_.dataSize) return false;

    data_.resize(header_.dataSize);
    memcpy(data_.data(), static_cast<const uint8_t*>(data) + sizeof(BlockHeader),
           header_.dataSize);

    checksumType_ = static_cast<ChecksumType>(header_.checksumType);

    return verify();
}

// ============================================================================
// SSTABLE VERIFIER IMPLEMENTATION
// ============================================================================

SSTableVerifier::SSTableVerifier(const std::string& sstablePath)
    : sstablePath_(sstablePath) {}

SSTableVerifier::VerificationResult SSTableVerifier::verify() {
    VerificationResult result;
    result.valid = true;
    result.blocksChecked = 0;
    result.blocksCorrupted = 0;

    auto start = std::chrono::steady_clock::now();

    std::ifstream file(sstablePath_, std::ios::binary);
    if (!file) {
        result.valid = false;
        result.error = "Cannot open file: " + sstablePath_;
        return result;
    }

    // Read and verify each block
    while (file) {
        BlockHeader header;
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) {
            break;  // EOF
        }

        if (header.magic != BLOCK_MAGIC) {
            result.valid = false;
            result.blocksCorrupted++;
            continue;
        }

        std::vector<uint8_t> data(header.dataSize);
        if (!file.read(reinterpret_cast<char*>(data.data()), header.dataSize)) {
            result.valid = false;
            result.error = "Truncated block";
            break;
        }

        uint32_t computed = ChecksumCalculator::crc32(data.data(), data.size());
        if (computed != header.checksum) {
            result.valid = false;
            result.blocksCorrupted++;

            SSTableBlockInfo info;
            info.offset = file.tellg();
            info.size = header.blockSize;
            info.checksum = header.checksum;
            result.corruptedBlocks.push_back(info);
        }

        result.blocksChecked++;
    }

    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return result;
}

SSTableVerifier::VerificationResult SSTableVerifier::quickVerify(double sampleRate) {
    VerificationResult result;
    result.valid = true;
    result.blocksChecked = 0;
    result.blocksCorrupted = 0;

    // TODO: Sample random blocks instead of all blocks
    return verify();  // For now, do full verification
}

bool SSTableVerifier::repairFromReplica(const std::string& replicaPath) {
    // TODO: Implement block-level repair from replica
    return false;
}

// ============================================================================
// DURABLE FILE WRITER IMPLEMENTATION
// ============================================================================

DurableFileWriter::DurableFileWriter(const std::string& path,
                                       const DurabilityConfig& config)
    : path_(path), config_(config) {}

DurableFileWriter::~DurableFileWriter() {
    close();
}

bool DurableFileWriter::open() {
    if (isOpen_) return true;

#ifdef _WIN32
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (config_.directIO) {
        flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
    }

    HANDLE h = CreateFileA(path_.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    fd_ = _open_osfhandle(reinterpret_cast<intptr_t>(h), 0);
#else
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (config_.directIO) {
        flags |= O_SYNC;
#ifdef O_DIRECT
        flags |= O_DIRECT;
#endif
    }

    fd_ = ::open(path_.c_str(), flags, 0644);
    if (fd_ < 0) return false;
#endif

    isOpen_ = true;
    position_ = 0;

    // Start background sync thread if needed
    if (config_.mode == DurabilityMode::PERIODIC) {
        running_ = true;
        syncThread_ = std::thread(&DurableFileWriter::backgroundSync, this);
    }

    return true;
}

void DurableFileWriter::close() {
    if (!isOpen_) return;

    running_ = false;
    if (syncThread_.joinable()) {
        syncThread_.join();
    }

    sync();  // Final sync

#ifdef _WIN32
    _close(fd_);
#else
    ::close(fd_);
#endif

    fd_ = -1;
    isOpen_ = false;
}

bool DurableFileWriter::write(const void* data, size_t length) {
    if (!isOpen_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

#ifdef _WIN32
    DWORD written;
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
    if (!WriteFile(h, data, static_cast<DWORD>(length), &written, NULL)) {
        return false;
    }
#else
    ssize_t written = ::write(fd_, data, length);
    if (written < 0) return false;
#endif

    position_ += written;
    bytesWritten_ += written;
    pendingWrites_++;

    // Handle different durability modes
    switch (config_.mode) {
        case DurabilityMode::EVERY_WRITE:
            sync();
            break;
        case DurabilityMode::BATCH:
            checkBatchSync();
            break;
        default:
            break;
    }

    return true;
}

bool DurableFileWriter::write(const std::string& data) {
    return write(data.data(), data.size());
}

bool DurableFileWriter::writeBlock(const DataBlock& block) {
    auto serialized = block.serialize();
    return write(serialized.data(), serialized.size());
}

bool DurableFileWriter::sync() {
    if (!isOpen_) return false;

#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
    if (!FlushFileBuffers(h)) return false;
#else
    if (fsync(fd_) != 0) return false;
#endif

    pendingWrites_ = 0;
    syncsPerformed_++;
    return true;
}

bool DurableFileWriter::datasync() {
    if (!isOpen_) return false;

#ifdef _WIN32
    return sync();  // Windows doesn't have fdatasync
#else
    if (fdatasync(fd_) != 0) return false;
    pendingWrites_ = 0;
    syncsPerformed_++;
    return true;
#endif
}

void DurableFileWriter::backgroundSync() {
    while (running_) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.periodicIntervalMs));

        if (pendingWrites_ > 0) {
            sync();
        }
    }
}

void DurableFileWriter::checkBatchSync() {
    if (pendingWrites_ >= config_.batchSize) {
        sync();
    }
}

json DurableFileWriter::getStats() const {
    return {
        {"path", path_},
        {"isOpen", isOpen_},
        {"position", position_},
        {"bytesWritten", bytesWritten_.load()},
        {"syncsPerformed", syncsPerformed_.load()},
        {"pendingWrites", pendingWrites_.load()},
        {"mode", static_cast<int>(config_.mode)}
    };
}

// ============================================================================
// CRASH RECOVERY MANAGER IMPLEMENTATION
// ============================================================================

void CrashRecoveryManager::initialize(const std::string& dataDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    const fs::path configured(dataDir);
    if (dataDir.empty() || !configured.is_absolute()) {
        throw std::invalid_argument(
            "CrashRecoveryManager requires an explicit absolute DATA_ROOT");
    }
    std::error_code ec;
    fs::create_directories(configured, ec);
    if (ec) {
        throw std::runtime_error(
            "CrashRecoveryManager cannot create DATA_ROOT: " + ec.message());
    }
    dataDir_ = fs::canonical(configured, ec).string();
    if (ec || dataDir_.empty()) {
        throw std::runtime_error(
            "CrashRecoveryManager cannot canonicalize DATA_ROOT");
    }

    // Create checkpoint directory if needed
    fs::path checkpointDir = createContainedStorageDirectories(
        fs::path(dataDir_), fs::path(dataDir_) / "checkpoints");

    // Load last checkpoint info
    fs::path lastCheckpointFile = checkpointDir / "last_checkpoint.json";
    if (fs::exists(lastCheckpointFile)) {
        std::ifstream f(lastCheckpointFile);
        json j = json::parse(f);
        lastCheckpoint_.walSequence = j.value("walSequence", 0ULL);
        lastCheckpoint_.lastCheckpoint = j.value("lastCheckpoint", 0ULL);
        lastCheckpoint_.checkpointPath = j.value("checkpointPath", "");
    }
}

bool CrashRecoveryManager::createCheckpoint() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Create checkpoint directory
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    fs::path checkpointPath = fs::path(dataDir_) / "checkpoints" /
                               ("checkpoint_" + std::to_string(epoch));

    fs::create_directories(checkpointPath);

    // Copy SSTable files to checkpoint
    // TODO: Implement actual file copying

    // Update checkpoint info
    lastCheckpoint_.timestamp = now;
    lastCheckpoint_.checkpointPath = checkpointPath.string();

    // Save checkpoint metadata
    json meta = lastCheckpoint_.toJson();
    std::ofstream f(fs::path(dataDir_) / "checkpoints" / "last_checkpoint.json");
    f << meta.dump(2);

    return true;
}

RecoveryPoint CrashRecoveryManager::getLastCheckpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastCheckpoint_;
}

std::vector<RecoveryPoint> CrashRecoveryManager::listCheckpoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecoveryPoint> result;

    fs::path checkpointDir = fs::path(dataDir_) / "checkpoints";
    if (!fs::exists(checkpointDir)) return result;

    for (const auto& entry : fs::directory_iterator(checkpointDir)) {
        if (entry.is_directory() &&
            entry.path().filename().string().find("checkpoint_") == 0) {
            RecoveryPoint rp;
            rp.checkpointPath = entry.path().string();
            result.push_back(rp);
        }
    }

    return result;
}

bool CrashRecoveryManager::deleteOldCheckpoints(int keepCount) {
    auto checkpoints = listCheckpoints();

    if ((int)checkpoints.size() <= keepCount) return true;

    // Sort by path (which includes timestamp)
    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const RecoveryPoint& a, const RecoveryPoint& b) {
                  return a.checkpointPath < b.checkpointPath;
              });

    // Delete oldest checkpoints
    int toDelete = checkpoints.size() - keepCount;
    for (int i = 0; i < toDelete; i++) {
        fs::remove_all(checkpoints[i].checkpointPath);
    }

    return true;
}

CrashRecoveryManager::RecoveryResult CrashRecoveryManager::recover() {
    RecoveryResult result;
    result.success = true;
    result.entriesRecovered = 0;
    result.entriesSkipped = 0;

    auto start = std::chrono::steady_clock::now();

    // Recover from last checkpoint + WAL
    if (!lastCheckpoint_.checkpointPath.empty()) {
        result = recoverFromCheckpoint(lastCheckpoint_);
    }

    // Replay WAL from checkpoint sequence
    auto walResult = replayWAL(lastCheckpoint_.walSequence);
    result.entriesRecovered += walResult.entriesRecovered;
    result.entriesSkipped += walResult.entriesSkipped;
    result.success = result.success && walResult.success;

    auto end = std::chrono::steady_clock::now();
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return result;
}

CrashRecoveryManager::RecoveryResult CrashRecoveryManager::recoverFromCheckpoint(
    const RecoveryPoint& checkpoint) {
    RecoveryResult result;
    result.success = true;
    result.entriesRecovered = 0;
    result.entriesSkipped = 0;

    // TODO: Restore SSTable files from checkpoint

    return result;
}

CrashRecoveryManager::RecoveryResult CrashRecoveryManager::replayWAL(uint64_t fromSequence) {
    RecoveryResult result;
    result.success = true;
    result.entriesRecovered = 0;
    result.entriesSkipped = 0;

    fs::path walDir = fs::path(dataDir_) / "wal";
    if (!fs::exists(walDir)) {
        return result;
    }

    // Find and replay WAL files
    for (const auto& entry : fs::directory_iterator(walDir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".wal") continue;

        std::ifstream file(entry.path(), std::ios::binary);
        std::string line;

        while (std::getline(file, line)) {
            if (entryProcessor_) {
                if (entryProcessor_(line)) {
                    result.entriesRecovered++;
                } else {
                    result.entriesSkipped++;
                }
            }
        }
    }

    return result;
}

bool CrashRecoveryManager::validateDataIntegrity() {
    // Verify all SSTable files
    fs::path dataPath = fs::path(dataDir_) / "data";
    if (!fs::exists(dataPath)) return true;

    for (const auto& entry : fs::directory_iterator(dataPath)) {
        if (entry.path().extension() == ".sst") {
            SSTableVerifier verifier(entry.path().string());
            auto result = verifier.verify();
            if (!result.valid) {
                return false;
            }
        }
    }

    return true;
}

bool CrashRecoveryManager::validateWALIntegrity() {
    // Verify WAL files have valid checksums
    // TODO: Implement
    return true;
}

json CrashRecoveryManager::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return {
        {"dataDir", dataDir_},
        {"lastCheckpoint", lastCheckpoint_.toJson()},
        {"checkpointCount", listCheckpoints().size()}
    };
}

// ============================================================================
// DATA INTEGRITY MONITOR IMPLEMENTATION
// ============================================================================

void DataIntegrityMonitor::start(int intervalSeconds) {
    if (running_) return;

    intervalSeconds_ = intervalSeconds;
    running_ = true;
    checkThread_ = std::thread(&DataIntegrityMonitor::backgroundCheck, this);
}

void DataIntegrityMonitor::stop() {
    running_ = false;
    if (checkThread_.joinable()) {
        checkThread_.join();
    }
}

void DataIntegrityMonitor::backgroundCheck() {
    while (running_) {
        for (int i = 0; i < intervalSeconds_ && running_; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!running_) break;

        // Perform check
        lastReport_ = checkAll();
        totalChecks_++;
    }
}

DataIntegrityMonitor::IntegrityReport DataIntegrityMonitor::checkAll() {
    IntegrityReport report;
    report.checkTime = std::chrono::system_clock::now();
    report.passed = true;
    report.filesChecked = 0;
    report.filesCorrupted = 0;
    report.blocksChecked = 0;
    report.blocksCorrupted = 0;

    // Would iterate over all data files
    // TODO: Implement

    return report;
}

DataIntegrityMonitor::IntegrityReport DataIntegrityMonitor::checkFile(
    const std::string& path) {
    IntegrityReport report;
    report.checkTime = std::chrono::system_clock::now();

    auto start = std::chrono::steady_clock::now();

    SSTableVerifier verifier(path);
    auto result = verifier.verify();

    report.passed = result.valid;
    report.filesChecked = 1;
    report.filesCorrupted = result.valid ? 0 : 1;
    report.blocksChecked = result.blocksChecked;
    report.blocksCorrupted = result.blocksCorrupted;

    if (!result.valid) {
        report.corruptedFiles.push_back(path);
        corruptionsDetected_++;

        if (corruptionHandler_) {
            corruptionHandler_(path, result.error);
        }
    }

    auto end = std::chrono::steady_clock::now();
    report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return report;
}

DataIntegrityMonitor::IntegrityReport DataIntegrityMonitor::checkDirectory(
    const std::string& dir) {
    IntegrityReport report;
    report.checkTime = std::chrono::system_clock::now();
    report.passed = true;
    report.filesChecked = 0;
    report.filesCorrupted = 0;
    report.blocksChecked = 0;
    report.blocksCorrupted = 0;

    auto start = std::chrono::steady_clock::now();

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".sst") continue;

        auto fileReport = checkFile(entry.path().string());

        report.filesChecked++;
        if (!fileReport.passed) {
            report.passed = false;
            report.filesCorrupted++;
            report.corruptedFiles.insert(report.corruptedFiles.end(),
                                          fileReport.corruptedFiles.begin(),
                                          fileReport.corruptedFiles.end());
        }
        report.blocksChecked += fileReport.blocksChecked;
        report.blocksCorrupted += fileReport.blocksCorrupted;
    }

    auto end = std::chrono::steady_clock::now();
    report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    return report;
}

DataIntegrityMonitor::IntegrityReport DataIntegrityMonitor::getLastReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastReport_;
}

json DataIntegrityMonitor::IntegrityReport::toJson() const {
    return {
        {"passed", passed},
        {"filesChecked", filesChecked},
        {"filesCorrupted", filesCorrupted},
        {"blocksChecked", blocksChecked},
        {"blocksCorrupted", blocksCorrupted},
        {"corruptedFiles", corruptedFiles},
        {"durationMs", duration.count()}
    };
}

json DataIntegrityMonitor::getMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return {
        {"running", running_.load()},
        {"intervalSeconds", intervalSeconds_},
        {"totalChecks", totalChecks_.load()},
        {"corruptionsDetected", corruptionsDetected_.load()},
        {"lastReport", lastReport_.toJson()}
    };
}

} // namespace durability
} // namespace pacificdb
