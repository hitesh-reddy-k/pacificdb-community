#include "durable_file_ops.hpp"
#include "storage_format_v2.hpp"

#include "data_durability.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" {
int LZ4_compressBound(int inputSize);
int LZ4_compress_default(const char* source, char* dest, int sourceSize, int maxDestSize);
int LZ4_decompress_safe(const char* source, char* dest, int compressedSize, int destCapacity);
}

namespace pacificdb::storage_v2 {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::array<char, 8> kIndexMagic{{'P','D','B','C','I','X','2','\0'}};
constexpr std::array<char, 8> kSstMagic{{'P','D','B','S','S','T','2','\0'}};
constexpr std::array<char, 8> kSstIndexMagic{{'P','D','B','S','I','X','2','\0'}};
constexpr std::array<char, 8> kBloomMagic{{'P','D','B','B','L','M','2','\0'}};
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kBlockMagic = 0x324b4c42U;  // BLK2
constexpr std::size_t kSstHeaderBytes = 64;
constexpr std::size_t kBlockHeaderBytes = 24;
constexpr std::size_t kBloomHeaderBytes = 48;
constexpr std::uint32_t kMaxFieldBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaxBlocks = 16U * 1024U * 1024U;

void setError(std::string* error, const std::string& value) {
    if (error) *error = value;
}

void appendU32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (i * 8)) & 0xffU));
}

void appendU64(std::string& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((value >> (i * 8)) & 0xffU));
}

bool readU32(const char*& cursor, const char* end, std::uint32_t& value) {
    if (end - cursor < 4) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(
        static_cast<unsigned char>(cursor[i])) << (i * 8);
    cursor += 4;
    return true;
}

bool readU64(const char*& cursor, const char* end, std::uint64_t& value) {
    if (end - cursor < 8) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(
        static_cast<unsigned char>(cursor[i])) << (i * 8);
    cursor += 8;
    return true;
}

std::uint32_t crc32c(const void* data, std::size_t size) {
    return pacificdb::durability::ChecksumCalculator::crc32c(data, size);
}

bool writeAll(std::ofstream& out, const std::string& bytes) {
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool syncFile(const fs::path& path, bool directory = false) {
    return directory ? pacificdb::durable_file::syncDirectory(path)
                     : pacificdb::durable_file::syncFile(path);
}

std::string rowId(const json& row) {
    if (!row.is_object() || !row.contains("id")) return {};
    try {
        return row["id"].is_string() ? row["id"].get<std::string>() : row["id"].dump();
    } catch (...) {
        return {};
    }
}

std::string nextSegmentName(const fs::path& directory) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::uint64_t> lastStamp;
    std::lock_guard<std::mutex> lock(mutex);
    auto& previous = lastStamp[directory.string()];
    if (previous == 0) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            if (ec) break;
            const auto filename = entry.path().filename().string();
            if (filename.size() < 20 || entry.path().extension() != ".seg") continue;
            try {
                previous = std::max(previous, static_cast<std::uint64_t>(
                    std::stoull(filename.substr(0, 20))));
            } catch (...) {
                // Ignore pre-v2 or manually named files; the reader validates their contents.
            }
        }
    }
    const auto wallClock = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    previous = std::max(wallClock, previous + 1);
    std::ostringstream name;
    name << std::setw(20) << std::setfill('0') << previous << ".seg";
    return name.str();
}

struct BlockIndexEntry {
    std::uint64_t offset{0};
    std::uint32_t storedBytes{0};
    std::uint32_t originalBytes{0};
    std::uint32_t rows{0};
    std::string firstKey;
    std::string lastKey;
};

struct SstMetadata {
    std::uint64_t fileSize{0};
    std::int64_t mtime{0};
    std::uint64_t rowCount{0};
    std::vector<BlockIndexEntry> blocks;
};

std::mutex g_sstCacheMutex;
std::unordered_map<std::string, std::shared_ptr<const SstMetadata>> g_sstCache;

std::int64_t mtimeOf(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    return ec ? -1 : static_cast<std::int64_t>(time.time_since_epoch().count());
}

bool parseSstHeader(const std::array<char, kSstHeaderBytes>& header,
                    std::uint64_t& rowCount,
                    std::uint64_t& indexOffset,
                    std::uint32_t& blockCount,
                    std::string* error) {
    if (!std::equal(kSstMagic.begin(), kSstMagic.end(), header.begin())) {
        setError(error, "not a PacificDB v2 SST");
        return false;
    }
    const char* cursor = header.data() + kSstMagic.size();
    const char* end = header.data() + header.size();
    std::uint32_t version = 0, headerBytes = 0, targetBytes = 0, reserved = 0;
    std::uint32_t flags = 0, storedCrc = 0;
    if (!readU32(cursor, end, version) || !readU32(cursor, end, headerBytes) ||
        !readU32(cursor, end, targetBytes) || !readU32(cursor, end, reserved) ||
        !readU64(cursor, end, rowCount) || !readU64(cursor, end, indexOffset) ||
        !readU32(cursor, end, blockCount) || !readU32(cursor, end, flags) ||
        !readU32(cursor, end, storedCrc)) {
        setError(error, "truncated SST header");
        return false;
    }
    (void)targetBytes;
    (void)reserved;
    (void)flags;
    if (version != kVersion || headerBytes != kSstHeaderBytes || blockCount > kMaxBlocks) {
        setError(error, "unsupported SST header");
        return false;
    }
    auto copy = header;
    std::fill(copy.begin() + 48, copy.begin() + 52, 0);
    if (crc32c(copy.data(), copy.size()) != storedCrc) {
        setError(error, "SST header checksum mismatch");
        return false;
    }
    return true;
}

std::shared_ptr<const SstMetadata> loadMetadata(const fs::path& path, std::string* error) {
    std::error_code ec;
    const std::uint64_t size = fs::file_size(path, ec);
    if (ec || size < kSstHeaderBytes) {
        setError(error, "cannot stat SST");
        return {};
    }
    const std::int64_t mtime = mtimeOf(path);
    const std::string key = path.string();
    {
        std::lock_guard<std::mutex> lock(g_sstCacheMutex);
        auto found = g_sstCache.find(key);
        if (found != g_sstCache.end() && found->second->fileSize == size &&
            found->second->mtime == mtime) return found->second;
    }

    std::ifstream in(path, std::ios::binary);
    std::array<char, kSstHeaderBytes> header{};
    if (!in.read(header.data(), static_cast<std::streamsize>(header.size()))) {
        setError(error, "cannot read SST header");
        return {};
    }
    std::uint64_t rowCount = 0, indexOffset = 0;
    std::uint32_t blockCount = 0;
    if (!parseSstHeader(header, rowCount, indexOffset, blockCount, error)) return {};
    if (indexOffset < kSstHeaderBytes || indexOffset >= size) {
        setError(error, "invalid SST index offset");
        return {};
    }
    in.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
    std::string indexBytes(static_cast<std::size_t>(size - indexOffset), '\0');
    if (!in.read(indexBytes.data(), static_cast<std::streamsize>(indexBytes.size()))) {
        setError(error, "truncated SST block index");
        return {};
    }
    if (indexBytes.size() < kSstIndexMagic.size() + 8 ||
        !std::equal(kSstIndexMagic.begin(), kSstIndexMagic.end(), indexBytes.begin())) {
        setError(error, "invalid SST block index magic");
        return {};
    }
    const std::uint32_t expectedIndexCrc = [&]() {
        const char* p = indexBytes.data() + indexBytes.size() - 4;
        const char* e = indexBytes.data() + indexBytes.size();
        std::uint32_t value = 0;
        readU32(p, e, value);
        return value;
    }();
    if (crc32c(indexBytes.data(), indexBytes.size() - 4) != expectedIndexCrc) {
        setError(error, "SST block index checksum mismatch");
        return {};
    }
    const char* cursor = indexBytes.data() + kSstIndexMagic.size();
    const char* end = indexBytes.data() + indexBytes.size() - 4;
    std::uint32_t count = 0;
    if (!readU32(cursor, end, count) || count != blockCount || count > kMaxBlocks) {
        setError(error, "invalid SST block count");
        return {};
    }
    auto metadata = std::make_shared<SstMetadata>();
    metadata->fileSize = size;
    metadata->mtime = mtime;
    metadata->rowCount = rowCount;
    metadata->blocks.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        BlockIndexEntry entry;
        std::uint32_t firstSize = 0, lastSize = 0;
        if (!readU64(cursor, end, entry.offset) ||
            !readU32(cursor, end, entry.storedBytes) ||
            !readU32(cursor, end, entry.originalBytes) ||
            !readU32(cursor, end, entry.rows) ||
            !readU32(cursor, end, firstSize) || firstSize > kMaxFieldBytes ||
            end - cursor < static_cast<std::ptrdiff_t>(firstSize)) {
            setError(error, "truncated SST block index entry");
            return {};
        }
        entry.firstKey.assign(cursor, cursor + firstSize);
        cursor += firstSize;
        if (!readU32(cursor, end, lastSize) || lastSize > kMaxFieldBytes ||
            end - cursor < static_cast<std::ptrdiff_t>(lastSize)) {
            setError(error, "truncated SST block key range");
            return {};
        }
        entry.lastKey.assign(cursor, cursor + lastSize);
        cursor += lastSize;
        if (entry.offset < kSstHeaderBytes || entry.offset + kBlockHeaderBytes +
            entry.storedBytes > indexOffset) {
            setError(error, "SST block points outside data region");
            return {};
        }
        metadata->blocks.push_back(std::move(entry));
    }
    if (cursor != end) {
        setError(error, "unexpected bytes in SST block index");
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(g_sstCacheMutex);
        g_sstCache[key] = metadata;
    }
    return metadata;
}

bool readBlockPayload(std::ifstream& in, const BlockIndexEntry& expected,
                      std::string& plain, std::uint32_t& rowCount,
                      std::string* error) {
    in.seekg(static_cast<std::streamoff>(expected.offset), std::ios::beg);
    std::array<char, kBlockHeaderBytes> header{};
    if (!in.read(header.data(), static_cast<std::streamsize>(header.size()))) {
        setError(error, "truncated SST block header");
        return false;
    }
    const char* cursor = header.data();
    const char* end = header.data() + header.size();
    std::uint32_t magic = 0, stored = 0, original = 0, count = 0, checksum = 0;
    if (!readU32(cursor, end, magic) || magic != kBlockMagic || end - cursor < 4) {
        setError(error, "invalid SST block header");
        return false;
    }
    const std::uint8_t compression = static_cast<std::uint8_t>(*cursor++);
    cursor += 3;
    if (!readU32(cursor, end, stored) || !readU32(cursor, end, original) ||
        !readU32(cursor, end, count) || !readU32(cursor, end, checksum) ||
        stored != expected.storedBytes || original != expected.originalBytes ||
        count != expected.rows || original > kMaxFieldBytes * 4U) {
        setError(error, "SST block metadata mismatch");
        return false;
    }
    std::string encoded(stored, '\0');
    if (!in.read(encoded.data(), static_cast<std::streamsize>(encoded.size()))) {
        setError(error, "truncated SST block payload");
        return false;
    }
    if (compression == 0) {
        if (stored != original) {
            setError(error, "invalid uncompressed SST block length");
            return false;
        }
        plain = std::move(encoded);
    } else if (compression == 1) {
        plain.resize(original);
        const int decoded = LZ4_decompress_safe(encoded.data(), plain.data(),
            static_cast<int>(encoded.size()), static_cast<int>(plain.size()));
        if (decoded != static_cast<int>(plain.size())) {
            setError(error, "LZ4 SST block decompression failed");
            return false;
        }
    } else {
        setError(error, "unsupported SST compression");
        return false;
    }
    if (crc32c(plain.data(), plain.size()) != checksum) {
        setError(error, "SST block checksum mismatch");
        return false;
    }
    rowCount = count;
    return true;
}

bool readSequentialBlock(std::ifstream& in, std::uint64_t indexOffset,
                         std::string& plain, std::uint32_t& rowCount,
                         std::string* error) {
    const auto offset = in.tellg();
    if (offset < 0 || static_cast<std::uint64_t>(offset) + kBlockHeaderBytes > indexOffset) {
        setError(error, "SST block crosses block-index boundary");
        return false;
    }
    std::array<char, kBlockHeaderBytes> header{};
    if (!in.read(header.data(), static_cast<std::streamsize>(header.size()))) {
        setError(error, "truncated SST block header");
        return false;
    }
    const char* cursor = header.data();
    const char* end = header.data() + header.size();
    std::uint32_t magic = 0, stored = 0, original = 0, count = 0, checksum = 0;
    if (!readU32(cursor, end, magic) || magic != kBlockMagic || end - cursor < 4) {
        setError(error, "invalid SST block header");
        return false;
    }
    const std::uint8_t compression = static_cast<std::uint8_t>(*cursor++);
    cursor += 3;
    if (!readU32(cursor, end, stored) || !readU32(cursor, end, original) ||
        !readU32(cursor, end, count) || !readU32(cursor, end, checksum) ||
        original > kMaxFieldBytes * 4U ||
        static_cast<std::uint64_t>(in.tellg()) + stored > indexOffset) {
        setError(error, "invalid sequential SST block metadata");
        return false;
    }
    std::string encoded(stored, '\0');
    if (!in.read(encoded.data(), static_cast<std::streamsize>(encoded.size()))) {
        setError(error, "truncated SST block payload");
        return false;
    }
    if (compression == 0 && stored == original) {
        plain = std::move(encoded);
    } else if (compression == 1) {
        plain.resize(original);
        if (LZ4_decompress_safe(encoded.data(), plain.data(), static_cast<int>(stored),
                                static_cast<int>(original)) != static_cast<int>(original)) {
            setError(error, "LZ4 SST block decompression failed");
            return false;
        }
    } else {
        setError(error, "invalid SST block compression or length");
        return false;
    }
    if (crc32c(plain.data(), plain.size()) != checksum) {
        setError(error, "SST block checksum mismatch");
        return false;
    }
    rowCount = count;
    return true;
}

bool visitBlockRows(const std::string& plain, std::uint32_t count,
                    const std::function<bool(const std::uint8_t*, std::size_t)>& visitor,
                    std::string* error, bool* stopped = nullptr) {
    const char* payload = plain.data();
    const char* payloadEnd = plain.data() + plain.size();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t size = 0;
        if (!readU32(payload, payloadEnd, size) || size == 0 || size > kMaxFieldBytes ||
            payloadEnd - payload < static_cast<std::ptrdiff_t>(size)) {
            setError(error, "invalid SST row length");
            return false;
        }
        if (!visitor(reinterpret_cast<const std::uint8_t*>(payload), size)) {
            if (stopped) *stopped = true;
            return true;
        }
        payload += size;
    }
    if (payload != payloadEnd) {
        setError(error, "trailing bytes in SST block");
        return false;
    }
    return true;
}

bool readBigEndian(const std::uint8_t*& cursor, const std::uint8_t* end,
                   std::size_t bytes, std::uint64_t& value) {
    if (static_cast<std::size_t>(end - cursor) < bytes) return false;
    value = 0;
    for (std::size_t i = 0; i < bytes; ++i) value = (value << 8) | *cursor++;
    return true;
}

bool skipMsgpack(const std::uint8_t*& cursor, const std::uint8_t* end, int depth = 0) {
    if (cursor == end || depth > 64) return false;
    const std::uint8_t tag = *cursor++;
    if (tag <= 0x7f || tag >= 0xe0 || tag == 0xc0 || tag == 0xc2 || tag == 0xc3) return true;

    std::uint64_t count = 0;
    if ((tag & 0xe0) == 0xa0) {
        count = tag & 0x1f;
        if (static_cast<std::uint64_t>(end - cursor) < count) return false;
        cursor += count;
        return true;
    }
    if ((tag & 0xf0) == 0x90) {
        count = tag & 0x0f;
        for (std::uint64_t i = 0; i < count; ++i) if (!skipMsgpack(cursor, end, depth + 1)) return false;
        return true;
    }
    if ((tag & 0xf0) == 0x80) {
        count = tag & 0x0f;
        for (std::uint64_t i = 0; i < count; ++i) {
            if (!skipMsgpack(cursor, end, depth + 1) || !skipMsgpack(cursor, end, depth + 1)) return false;
        }
        return true;
    }

    std::size_t fixed = 0;
    switch (tag) {
        case 0xc4: case 0xd9: fixed = 1; break;
        case 0xc5: case 0xda: fixed = 2; break;
        case 0xc6: case 0xdb: fixed = 4; break;
        case 0xc7: fixed = 1; break;
        case 0xc8: fixed = 2; break;
        case 0xc9: fixed = 4; break;
        case 0xca: case 0xce: case 0xd2: fixed = 4; count = 0; break;
        case 0xcb: case 0xcf: case 0xd3: fixed = 8; count = 0; break;
        case 0xcc: case 0xd0: fixed = 1; count = 0; break;
        case 0xcd: case 0xd1: fixed = 2; count = 0; break;
        case 0xd4: fixed = 2; count = 0; break;
        case 0xd5: fixed = 3; count = 0; break;
        case 0xd6: fixed = 5; count = 0; break;
        case 0xd7: fixed = 9; count = 0; break;
        case 0xd8: fixed = 17; count = 0; break;
        case 0xdc: fixed = 2; break;
        case 0xdd: fixed = 4; break;
        case 0xde: fixed = 2; break;
        case 0xdf: fixed = 4; break;
        default: return false;
    }

    if ((tag >= 0xca && tag <= 0xd8) &&
        tag != 0xdc && tag != 0xdd && tag != 0xde && tag != 0xdf && count == 0) {
        if (static_cast<std::size_t>(end - cursor) < fixed) return false;
        cursor += fixed;
        return true;
    }
    if (!readBigEndian(cursor, end, fixed, count)) return false;
    if (tag == 0xc7 || tag == 0xc8 || tag == 0xc9) ++count;  // extension type byte
    if (tag == 0xdc || tag == 0xdd) {
        for (std::uint64_t i = 0; i < count; ++i) if (!skipMsgpack(cursor, end, depth + 1)) return false;
        return true;
    }
    if (tag == 0xde || tag == 0xdf) {
        for (std::uint64_t i = 0; i < count; ++i) {
            if (!skipMsgpack(cursor, end, depth + 1) || !skipMsgpack(cursor, end, depth + 1)) return false;
        }
        return true;
    }
    if (static_cast<std::uint64_t>(end - cursor) < count) return false;
    cursor += count;
    return true;
}

bool readMsgpackString(const std::uint8_t*& cursor, const std::uint8_t* end,
                       std::string_view& value) {
    if (cursor == end) return false;
    const std::uint8_t tag = *cursor++;
    std::uint64_t size = 0;
    if ((tag & 0xe0) == 0xa0) size = tag & 0x1f;
    else if (tag == 0xd9) { if (!readBigEndian(cursor, end, 1, size)) return false; }
    else if (tag == 0xda) { if (!readBigEndian(cursor, end, 2, size)) return false; }
    else if (tag == 0xdb) { if (!readBigEndian(cursor, end, 4, size)) return false; }
    else return false;
    if (static_cast<std::uint64_t>(end - cursor) < size) return false;
    value = std::string_view(reinterpret_cast<const char*>(cursor), static_cast<std::size_t>(size));
    cursor += size;
    return true;
}

// Field spans have already been bounded by skipMsgpack. Scalar metadata is the
// common scan case; constructing a full binary reader/SAX adapter per value is
// unnecessary. Keep exactly the reference decoder's signed/unsigned/float types.
// Containers, binary and extension values retain the existing generic decoder.
json decodeMsgpackField(const std::uint8_t* begin, const std::uint8_t* end) {
    if (begin != end) {
        const auto tag = *begin;
        auto cursor = begin + 1;
        if (cursor == end) {
            if (tag <= 0x7f) return json(static_cast<json::number_unsigned_t>(tag));
            if (tag >= 0xe0) return json(static_cast<json::number_integer_t>(tag) - 256);
            if (tag == 0xc0) return nullptr;
            if (tag == 0xc2 || tag == 0xc3) return tag == 0xc3;
        }
        if ((tag >= 0xcc && tag <= 0xcf) || (tag >= 0xd0 && tag <= 0xd3)) {
            const bool isSigned = tag >= 0xd0;
            const auto width = size_t{1} << (tag - (isSigned ? 0xd0 : 0xcc));
            uint64_t bits = 0;
            if (readBigEndian(cursor, end, width, bits) && cursor == end) {
                if (!isSigned) return json(static_cast<json::number_unsigned_t>(bits));
                if (width < 8 && (bits & (uint64_t{1} << (width * 8 - 1))))
                    bits |= std::numeric_limits<uint64_t>::max() << (width * 8);
                int64_t number = 0;
                std::memcpy(&number, &bits, sizeof(number));
                return json(static_cast<json::number_integer_t>(number));
            }
        } else if (tag == 0xca || tag == 0xcb) {
            uint64_t bits = 0;
            if (readBigEndian(cursor, end, tag == 0xca ? 4 : 8, bits) && cursor == end) {
                double number = 0;
                if (tag == 0xca) {
                    const uint32_t encoded = static_cast<uint32_t>(bits);
                    float value = 0;
                    std::memcpy(&value, &encoded, sizeof(value));
                    number = value;
                } else std::memcpy(&number, &bits, sizeof(number));
                return json(static_cast<json::number_float_t>(number));
            }
        } else if ((tag & 0xe0) == 0xa0 || tag == 0xd9 || tag == 0xda || tag == 0xdb) {
            cursor = begin;
            std::string_view value;
            if (readMsgpackString(cursor, end, value) && cursor == end)
                return json(std::string(value));
        }
    }
    return json::from_msgpack(begin, end, true, true);
}

std::uint64_t fnv1a(const std::string& value, std::uint64_t seed) {
    std::uint64_t hash = 1469598103934665603ULL ^ seed;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    return hash;
}

struct BloomView {
    std::uint64_t fileSize{0};
    std::int64_t mtime{0};
    std::uint64_t bitCount{0};
    std::uint32_t hashes{0};
    const std::uint64_t* words{nullptr};
    std::size_t wordCount{0};
#ifndef _WIN32
    int fd{-1};
    void* mapping{MAP_FAILED};
    ~BloomView() {
        if (mapping != MAP_FAILED) ::munmap(mapping, static_cast<std::size_t>(fileSize));
        if (fd >= 0) ::close(fd);
    }
#else
    std::vector<std::uint64_t> owned;
#endif
};

std::mutex g_bloomCacheMutex;
std::unordered_map<std::string, std::shared_ptr<const BloomView>> g_bloomCache;

std::shared_ptr<const BloomView> mapBloom(const fs::path& path) {
    std::error_code ec;
    const std::uint64_t size = fs::file_size(path, ec);
    if (ec || size < kBloomHeaderBytes + sizeof(std::uint64_t)) return {};
    const auto mtime = mtimeOf(path);
    const std::string key = path.string();
    {
        std::lock_guard<std::mutex> lock(g_bloomCacheMutex);
        auto found = g_bloomCache.find(key);
        if (found != g_bloomCache.end() && found->second->fileSize == size &&
            found->second->mtime == mtime) return found->second;
    }

    auto view = std::make_shared<BloomView>();
    view->fileSize = size;
    view->mtime = mtime;
#ifndef _WIN32
    view->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (view->fd < 0) return {};
    view->mapping = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_SHARED, view->fd, 0);
    if (view->mapping == MAP_FAILED) return {};
    const char* data = static_cast<const char*>(view->mapping);
#else
    std::ifstream in(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return {};
    const char* data = bytes.data();
#endif
    if (!std::equal(kBloomMagic.begin(), kBloomMagic.end(), data)) return {};
    const char* cursor = data + kBloomMagic.size();
    const char* end = data + size;
    std::uint32_t version = 0, hashes = 0, checksum = 0, reserved = 0;
    std::uint64_t bitCount = 0, keyCount = 0, wordCount = 0;
    if (!readU32(cursor, end, version) || !readU32(cursor, end, hashes) ||
        !readU64(cursor, end, bitCount) || !readU64(cursor, end, keyCount) ||
        !readU64(cursor, end, wordCount) || !readU32(cursor, end, checksum) ||
        !readU32(cursor, end, reserved)) return {};
    (void)keyCount;
    (void)reserved;
    if (version != kVersion || hashes == 0 || hashes > 16 || bitCount < 64 ||
        bitCount % 64 != 0 || wordCount != bitCount / 64 ||
        wordCount > std::numeric_limits<std::size_t>::max() ||
        kBloomHeaderBytes + wordCount * sizeof(std::uint64_t) != size) return {};
    const char* wordBytes = data + kBloomHeaderBytes;
    if (crc32c(wordBytes, static_cast<std::size_t>(wordCount * sizeof(std::uint64_t))) != checksum) return {};
    view->bitCount = bitCount;
    view->hashes = hashes;
    view->wordCount = static_cast<std::size_t>(wordCount);
#ifndef _WIN32
    view->words = reinterpret_cast<const std::uint64_t*>(wordBytes);
#else
    view->owned.resize(view->wordCount);
    std::memcpy(view->owned.data(), wordBytes, view->wordCount * sizeof(std::uint64_t));
    view->words = view->owned.data();
#endif
    {
        std::lock_guard<std::mutex> lock(g_bloomCacheMutex);
        g_bloomCache[key] = view;
    }
    return view;
}

std::optional<long long> rowVersion(const MsgpackRowView& row) {
        for (const char* name : {"_mvcc_version", "version", "deleted_txn",
                                 "created_txn", "_timestamp"}) {
            const auto value = row.field(name);
            if (!value) continue;
            try {
                if (value->is_number_integer() || value->is_number_unsigned()) {
                    return value->get<long long>();
                }
                if (value->is_string()) return std::stoll(value->get<std::string>());
            } catch (...) {}
        }
        return std::nullopt;
}

}  // namespace

void MsgpackRowView::buildFieldIndex() {
    if (!data_ || !size_) return;
    const auto* cursor = data_;
    const auto* end = data_ + size_;
    std::uint64_t count = 0;
    const auto tag = *cursor++;
    if ((tag & 0xf0) == 0x80) count = tag & 0x0f;
    else if (tag == 0xde || tag == 0xdf) {
        if (!readBigEndian(cursor, end, tag == 0xde ? 2 : 4, count)) return;
    } else return;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string_view name;
        if (!readMsgpackString(cursor, end, name)) return;
        const auto* begin = cursor;
        if (!skipMsgpack(cursor, end)) return;
        if (fieldCount_ < fields_.size()) fields_[fieldCount_++] = {name, begin, cursor};
    }
    indexValid_ = cursor == end;
}

std::optional<json> MsgpackRowView::field(std::string_view name) const {
    if (indexed_) {
        if (!indexValid_) return std::nullopt;
        for (std::size_t i = 0; i < fieldCount_; ++i) {
            const auto& span = fields_[i];
            if (span.name == name) {
                try { return decodeMsgpackField(span.begin, span.end); }
                catch (...) { return std::nullopt; }
            }
        }
        if (fieldCount_ < fields_.size()) return std::nullopt;
    }
    if (!data_ || size_ == 0) return std::nullopt;
    const std::uint8_t* cursor = data_;
    const std::uint8_t* end = data_ + size_;
    std::uint64_t count = 0;
    const std::uint8_t tag = *cursor++;
    if ((tag & 0xf0) == 0x80) count = tag & 0x0f;
    else if (tag == 0xde) {
        if (!readBigEndian(cursor, end, 2, count)) return std::nullopt;
    } else if (tag == 0xdf) {
        if (!readBigEndian(cursor, end, 4, count)) return std::nullopt;
    } else {
        return std::nullopt;
    }

    for (std::uint64_t i = 0; i < count; ++i) {
        std::string_view key;
        if (!readMsgpackString(cursor, end, key)) return std::nullopt;
        const std::uint8_t* valueBegin = cursor;
        if (!skipMsgpack(cursor, end)) return std::nullopt;
        if (key == name) {
            try {
                return decodeMsgpackField(valueBegin, cursor);
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

json MsgpackRowView::project(const std::vector<std::string>& names) const {
    if (indexed_) {
        if (!indexValid_) throw std::runtime_error("invalid indexed MessagePack row");
        json selected = json::object();
        for (const auto& name : names) {
            if (auto value = field(name)) selected[name] = std::move(*value);
        }
        return selected;
    }
    if (!data_ || size_ == 0) throw std::runtime_error("empty MessagePack row");
    const std::uint8_t* cursor = data_;
    const std::uint8_t* end = data_ + size_;
    std::uint64_t count = 0;
    const std::uint8_t tag = *cursor++;
    if ((tag & 0xf0) == 0x80) count = tag & 0x0f;
    else if (tag == 0xde || tag == 0xdf) {
        if (!readBigEndian(cursor, end, tag == 0xde ? 2 : 4, count)) {
            throw std::runtime_error("invalid MessagePack row map");
        }
    } else throw std::runtime_error("MessagePack row must be an object");
    json selected = json::object();
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string_view key;
        if (!readMsgpackString(cursor, end, key)) throw std::runtime_error("invalid MessagePack field name");
        const auto* value = cursor;
        if (!skipMsgpack(cursor, end)) throw std::runtime_error("invalid MessagePack field value");
        const auto found = std::find_if(names.begin(), names.end(), [&](const std::string& name) { return name == key; });
        if (found != names.end() && !selected.contains(*found)) {
            selected[*found] = decodeMsgpackField(value, cursor);
        }
    }
    if (cursor != end) throw std::runtime_error("trailing MessagePack row bytes");
    return selected;
}

template<class Accumulator>
std::optional<double> MsgpackRowView::denseScoreImpl(std::string_view name,
        const std::vector<double>& query, const std::string& metric, double queryNorm) const {
    if (!indexed_ || !indexValid_ || query.empty()) return std::nullopt;
    const FieldSpan* selected = nullptr;
    for (std::size_t i = 0; i < fieldCount_; ++i)
        if (fields_[i].name == name) { selected = &fields_[i]; break; }
    if (!selected) return std::nullopt;
    const auto* cursor = selected->begin;
    const auto* end = selected->end;
    if (cursor == end) return std::nullopt;
    const auto tag = *cursor++;
    uint64_t count = 0;
    if ((tag & 0xf0) == 0x90) count = tag & 15;
    else if (tag == 0xdc || tag == 0xdd) {
        if (!readBigEndian(cursor, end, tag == 0xdc ? 2 : 4, count)) return std::nullopt;
    } else return std::nullopt;
    if (count != query.size()) return std::nullopt;
    const bool euclidean = metric == "l2" || metric == "euclidean";
    const bool dot = metric == "dot" || metric == "dot_product";
    Accumulator sum = 0, norm = 0, querySquare = 0;
    for (std::size_t i = 0; i < query.size(); ++i) {
        if (cursor == end) return std::nullopt;
        const uint8_t kind = *cursor++;
        double number;
        if (kind <= 0x7f) number = kind;
        else if (kind >= 0xe0) number = -double(256 - kind);
        else {
            uint64_t bits = 0;
            if (kind == 0xca || kind == 0xcb) {
                if (!readBigEndian(cursor, end, kind == 0xca ? 4 : 8, bits)) return std::nullopt;
                if (kind == 0xca) {
                    uint32_t raw = static_cast<uint32_t>(bits); float value;
                    std::memcpy(&value, &raw, sizeof(value)); number = value;
                } else std::memcpy(&number, &bits, sizeof(number));
            } else if (kind >= 0xcc && kind <= 0xd3) {
                const bool negativeType = kind >= 0xd0;
                const std::size_t width = std::size_t(1) << (negativeType ? kind - 0xd0 : kind - 0xcc);
                if (!readBigEndian(cursor, end, width, bits)) return std::nullopt;
                if (negativeType && (bits & (uint64_t(1) << (width * 8 - 1)))) {
                    const uint64_t mask = width == 8 ? UINT64_MAX : (uint64_t(1) << (width * 8)) - 1;
                    number = -static_cast<double>((~bits & mask) + 1);
                } else number = static_cast<double>(bits);
            } else return std::nullopt;
        }
        const Accumulator x=query[i],y=number;
        if (euclidean) { const Accumulator difference = x-y; sum += difference * difference; }
        else { sum += x*y; if (!dot) {norm += y*y;querySquare += x*x;} }
    }
    if (cursor != end) return std::nullopt;
    if constexpr(std::is_same_v<Accumulator,long double>){
        const long double result=euclidean?-std::sqrt(sum):dot?sum:
            querySquare==0||norm==0?0.L:std::clamp(sum/std::sqrt(querySquare*norm),-1.L,1.L);
        if(!std::isfinite(result)||std::abs(result)>std::numeric_limits<double>::max())throw std::runtime_error("vector_score_out_of_range");
        return static_cast<double>(result);
    }
    // Let the selective JSON path use wider intermediates for rare ranges;
    // never report a finite zero caused by an overflowing norm product.
    if(!std::isfinite(sum))return std::nullopt;
    if (euclidean) return -std::sqrt(sum);
    if (dot) return sum;
    const double denominator = queryNorm * std::sqrt(norm);
    if(!std::isfinite(denominator))return std::nullopt;
    return denominator < 1e-12 ? 0.0 : sum / denominator;
}

std::optional<double> MsgpackRowView::denseScore(std::string_view name,const std::vector<double>& query,
    const std::string& metric,double queryNorm) const {
    return denseScoreImpl<double>(name,query,metric,queryNorm);
}
std::optional<double> MsgpackRowView::densePreciseScore(std::string_view name,const std::vector<double>& query,
    const std::string& metric) const {
    return denseScoreImpl<long double>(name,query,metric,0.);
}

json MsgpackRowView::materialize() const {
    if (!data_ || size_ == 0) throw std::runtime_error("empty MessagePack row");
    return json::from_msgpack(data_, data_ + size_, true, true);
}

std::vector<fs::path> listIndexSegments(const fs::path& directory) {
    std::vector<fs::path> paths;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return paths;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (entry.is_regular_file() && entry.path().extension() == ".seg") {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

bool writeIndexSegment(const fs::path& directory, std::vector<IndexMutation> mutations,
                       bool snapshot, fs::path* writtenPath, std::string* error) {
    try {
        fs::create_directories(directory);
        std::sort(mutations.begin(), mutations.end(), [](const auto& left, const auto& right) {
            if (left.value != right.value) return left.value < right.value;
            if (left.id != right.id) return left.id < right.id;
            return static_cast<unsigned>(left.op) < static_cast<unsigned>(right.op);
        });
        std::string payload;
        for (const auto& mutation : mutations) {
            if (mutation.id.empty() || mutation.id.size() > kMaxFieldBytes ||
                mutation.value.size() > kMaxFieldBytes) {
                setError(error, "column-index mutation exceeds format limit");
                return false;
            }
            payload.push_back(static_cast<char>(mutation.op));
            payload.append(3, '\0');
            appendU32(payload, static_cast<std::uint32_t>(mutation.value.size()));
            appendU32(payload, static_cast<std::uint32_t>(mutation.id.size()));
            payload.append(mutation.value);
            payload.append(mutation.id);
        }
        std::string header(kIndexMagic.begin(), kIndexMagic.end());
        appendU32(header, kVersion);
        appendU32(header, snapshot ? 1U : 0U);
        appendU64(header, mutations.size());
        appendU64(header, payload.size());
        appendU32(header, crc32c(payload.data(), payload.size()));
        appendU32(header, 0);
        // ponytail: one process owns a storage root; add an inter-process lock only if that invariant changes.
        const auto finalPath = directory / nextSegmentName(directory);
        const auto temporary = fs::path(finalPath.string() + ".tmp");
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out || !writeAll(out, header) || !writeAll(out, payload)) {
                setError(error, "cannot write column-index segment");
                return false;
            }
            out.flush();
            if (!out) {
                setError(error, "cannot flush column-index segment");
                return false;
            }
        }
        if (!syncFile(temporary)) {
            setError(error, "cannot fdatasync column-index segment");
            return false;
        }
        std::error_code ec;
        pacificdb::durable_file::rename(temporary, finalPath, ec);
        if (ec || !syncFile(directory, true)) {
            setError(error, "cannot publish column-index segment: " + ec.message());
            return false;
        }
        if (writtenPath) *writtenPath = finalPath;
        return true;
    } catch (const std::exception& e) {
        setError(error, e.what());
        return false;
    }
}

bool readIndexSegment(const fs::path& path, IndexSegment& segment, std::string* error) {
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            setError(error, "cannot open column-index segment");
            return false;
        }
        std::string header(40, '\0');
        if (!in.read(header.data(), static_cast<std::streamsize>(header.size())) ||
            !std::equal(kIndexMagic.begin(), kIndexMagic.end(), header.begin())) {
            setError(error, "invalid column-index segment header");
            return false;
        }
        const char* cursor = header.data() + kIndexMagic.size();
        const char* end = header.data() + header.size();
        std::uint32_t version = 0, flags = 0, checksum = 0, reserved = 0;
        std::uint64_t count = 0, payloadSize = 0;
        if (!readU32(cursor, end, version) || !readU32(cursor, end, flags) ||
            !readU64(cursor, end, count) || !readU64(cursor, end, payloadSize) ||
            !readU32(cursor, end, checksum) || !readU32(cursor, end, reserved) ||
            version != kVersion || count > kMaxFieldBytes || payloadSize > kMaxFieldBytes * 16ULL) {
            setError(error, "unsupported column-index segment");
            return false;
        }
        (void)reserved;
        std::string payload(static_cast<std::size_t>(payloadSize), '\0');
        if (!in.read(payload.data(), static_cast<std::streamsize>(payload.size())) ||
            in.peek() != std::char_traits<char>::eof() ||
            crc32c(payload.data(), payload.size()) != checksum) {
            setError(error, "column-index segment checksum mismatch or truncation");
            return false;
        }
        segment.snapshot = (flags & 1U) != 0;
        segment.mutations.clear();
        segment.mutations.reserve(static_cast<std::size_t>(count));
        cursor = payload.data();
        end = payload.data() + payload.size();
        for (std::uint64_t i = 0; i < count; ++i) {
            if (end - cursor < 4) {
                setError(error, "truncated column-index mutation");
                return false;
            }
            const auto op = static_cast<IndexMutation::Op>(static_cast<std::uint8_t>(*cursor++));
            cursor += 3;
            std::uint32_t valueSize = 0, idSize = 0;
            if ((op != IndexMutation::Op::Clear && op != IndexMutation::Op::Put) ||
                !readU32(cursor, end, valueSize) || !readU32(cursor, end, idSize) ||
                valueSize > kMaxFieldBytes || idSize == 0 || idSize > kMaxFieldBytes ||
                end - cursor < static_cast<std::ptrdiff_t>(valueSize + idSize)) {
                setError(error, "invalid column-index mutation");
                return false;
            }
            IndexMutation mutation;
            mutation.op = op;
            mutation.value.assign(cursor, cursor + valueSize);
            cursor += valueSize;
            mutation.id.assign(cursor, cursor + idSize);
            cursor += idSize;
            segment.mutations.push_back(std::move(mutation));
        }
        if (cursor != end) {
            setError(error, "trailing bytes in column-index segment");
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        setError(error, e.what());
        return false;
    }
}

bool isBinarySst(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::array<char, 8> magic{};
    return in.read(magic.data(), static_cast<std::streamsize>(magic.size())) && magic == kSstMagic;
}

bool writeSst(const fs::path& path,
              const std::vector<std::reference_wrapper<const json>>& rows,
              std::size_t targetBlockBytes, SstWriteStats* stats, std::string* error,
              const std::function<void()>& beforeIo) {
    try {
        targetBlockBytes = std::clamp<std::size_t>(targetBlockBytes, 4096, 16384);
        const fs::path temporary = path.string() + ".writing";
        std::error_code ec;
        fs::remove(temporary, ec);
        std::fstream out(temporary, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!out) {
            setError(error, "cannot open SST for writing");
            return false;
        }
        std::array<char, kSstHeaderBytes> emptyHeader{};
        if (beforeIo) beforeIo();
        out.write(emptyHeader.data(), static_cast<std::streamsize>(emptyHeader.size()));
        std::vector<BlockIndexEntry> index;
        std::string block;
        std::string firstKey, lastKey;
        std::uint32_t blockRows = 0;
        SstWriteStats local;

        auto flushBlock = [&]() -> bool {
            if (blockRows == 0) return true;
            BlockIndexEntry entry;
            entry.offset = static_cast<std::uint64_t>(out.tellp());
            entry.originalBytes = static_cast<std::uint32_t>(block.size());
            entry.rows = blockRows;
            entry.firstKey = firstKey;
            entry.lastKey = lastKey;
            const int bound = LZ4_compressBound(static_cast<int>(block.size()));
            std::string compressed(static_cast<std::size_t>(std::max(0, bound)), '\0');
            const int compressedSize = bound > 0 ? LZ4_compress_default(
                block.data(), compressed.data(), static_cast<int>(block.size()), bound) : 0;
            const bool useCompression = compressedSize > 0 &&
                static_cast<std::size_t>(compressedSize + 8) < block.size();
            const std::string& storedData = useCompression ? compressed : block;
            entry.storedBytes = static_cast<std::uint32_t>(
                useCompression ? compressedSize : storedData.size());
            std::string blockHeader;
            appendU32(blockHeader, kBlockMagic);
            blockHeader.push_back(useCompression ? 1 : 0);
            blockHeader.append(3, '\0');
            appendU32(blockHeader, entry.storedBytes);
            appendU32(blockHeader, entry.originalBytes);
            appendU32(blockHeader, entry.rows);
            appendU32(blockHeader, crc32c(block.data(), block.size()));
            if (beforeIo) beforeIo();
            out.write(blockHeader.data(), static_cast<std::streamsize>(blockHeader.size()));
            out.write(storedData.data(), static_cast<std::streamsize>(entry.storedBytes));
            if (!out) return false;
            local.blocks++;
            local.uncompressedBytes += block.size();
            local.storedBytes += blockHeader.size() + entry.storedBytes;
            index.push_back(std::move(entry));
            block.clear();
            firstKey.clear();
            lastKey.clear();
            blockRows = 0;
            return true;
        };

        for (const auto& reference : rows) {
            const json& row = reference.get();
            const auto packed = json::to_msgpack(row);
            if (packed.empty() || packed.size() > kMaxFieldBytes) {
                setError(error, "SST row exceeds format limit");
                return false;
            }
            const std::size_t encodedBytes = 4 + packed.size();
            if (blockRows > 0 && block.size() + encodedBytes > targetBlockBytes && !flushBlock()) {
                setError(error, "cannot write SST block");
                return false;
            }
            const std::string id = rowId(row);
            if (blockRows == 0) firstKey = id;
            lastKey = id;
            appendU32(block, static_cast<std::uint32_t>(packed.size()));
            block.append(reinterpret_cast<const char*>(packed.data()), packed.size());
            blockRows++;
            local.rows++;
        }
        if (!flushBlock()) {
            setError(error, "cannot write final SST block");
            return false;
        }
        const std::uint64_t indexOffset = static_cast<std::uint64_t>(out.tellp());
        std::string indexBytes(kSstIndexMagic.begin(), kSstIndexMagic.end());
        appendU32(indexBytes, static_cast<std::uint32_t>(index.size()));
        for (const auto& entry : index) {
            appendU64(indexBytes, entry.offset);
            appendU32(indexBytes, entry.storedBytes);
            appendU32(indexBytes, entry.originalBytes);
            appendU32(indexBytes, entry.rows);
            appendU32(indexBytes, static_cast<std::uint32_t>(entry.firstKey.size()));
            indexBytes.append(entry.firstKey);
            appendU32(indexBytes, static_cast<std::uint32_t>(entry.lastKey.size()));
            indexBytes.append(entry.lastKey);
        }
        appendU32(indexBytes, crc32c(indexBytes.data(), indexBytes.size()));
        if (beforeIo) beforeIo();
        out.write(indexBytes.data(), static_cast<std::streamsize>(indexBytes.size()));
        if (!out) {
            setError(error, "cannot write SST block index");
            return false;
        }
        local.storedBytes += kSstHeaderBytes + indexBytes.size();

        std::string header(kSstMagic.begin(), kSstMagic.end());
        appendU32(header, kVersion);
        appendU32(header, kSstHeaderBytes);
        appendU32(header, static_cast<std::uint32_t>(targetBlockBytes));
        appendU32(header, 0);
        appendU64(header, local.rows);
        appendU64(header, indexOffset);
        appendU32(header, static_cast<std::uint32_t>(index.size()));
        appendU32(header, 0);
        appendU32(header, 0);
        header.resize(kSstHeaderBytes, '\0');
        const std::uint32_t headerCrc = crc32c(header.data(), header.size());
        for (int i = 0; i < 4; ++i) header[48 + i] = static_cast<char>((headerCrc >> (i * 8)) & 0xffU);
        out.seekp(0, std::ios::beg);
        if (beforeIo) beforeIo();
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.flush();
        if (!out) {
            setError(error, "cannot finalize SST header");
            return false;
        }
        out.close();
        if (beforeIo) beforeIo();
        if (!syncFile(temporary)) {
            setError(error, "cannot fdatasync SST");
            return false;
        }
        pacificdb::durable_file::rename(temporary, path, ec);
        if (ec || !syncFile(path.parent_path(), true)) {
            setError(error, "cannot publish SST: " + ec.message());
            return false;
        }
        invalidateCaches(path);
        if (stats) *stats = local;
        return true;
    } catch (const std::exception& e) {
        setError(error, e.what());
        return false;
    }
}

bool scanSst(const fs::path& path, const std::function<bool(json&&)>& visitor,
             std::string* error) {
    try {
        return scanSstViews(path, [&](const MsgpackRowView& row) {
            return visitor(row.materialize());
        }, error);
    } catch (const std::exception& e) {
        setError(error, std::string("invalid SST MessagePack row: ") + e.what());
        return false;
    }
}

bool scanSstViews(const fs::path& path,
                  const std::function<bool(const MsgpackRowView&)>& visitor,
                  std::string* error) {
    auto metadata = loadMetadata(path, error);
    if (!metadata) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        setError(error, "cannot open SST");
        return false;
    }
    std::string plain;
    std::uint32_t count = 0;
    for (const auto& block : metadata->blocks) {
        if (!readBlockPayload(in, block, plain, count, error)) return false;
        bool stopped = false;
        if (!visitBlockRows(plain, count, [&](const std::uint8_t* data, std::size_t size) {
                return visitor(MsgpackRowView(data, size));
            }, error, &stopped)) return false;
        if (stopped) return true;
    }
    return true;
}

struct SstCursor::Impl {
    fs::path path;
    std::ifstream input;
    bool binary{true};
    bool indexFields{false};
    std::uint64_t rowCount{0};
    std::uint64_t rowsRead{0};
    std::uint64_t indexOffset{0};
    std::uint32_t blockCount{0};
    std::uint32_t blocksRead{0};
    std::uint32_t rowsInBlock{0};
    std::uint32_t rowIndex{0};
    std::string block;
    std::vector<std::uint8_t> legacyRow;
    const char* cursor{nullptr};
    const char* end{nullptr};
    std::string currentId;
    std::string previousId;
    MsgpackRowView current{nullptr, 0};
    bool valid{false};

    bool loadBlock(std::string* error) {
        if (blocksRead >= blockCount) return false;
        if (!readSequentialBlock(input, indexOffset, block, rowsInBlock, error)) return false;
        ++blocksRead;
        rowIndex = 0;
        cursor = block.data();
        end = block.data() + block.size();
        return true;
    }

    bool advance(std::string* error) {
        if (!binary) {
            std::string line;
            while (std::getline(input, line)) {
                if (line.empty()) continue;
                try {
                    legacyRow = json::to_msgpack(json::parse(line));
                } catch (const std::exception& exception) {
                    setError(error, std::string("invalid legacy SST row: ") + exception.what());
                    return false;
                }
                current = MsgpackRowView(legacyRow.data(), legacyRow.size(), indexFields);
                break;
            }
            if (input.bad()) {
                setError(error, "cannot read legacy SST row");
                return false;
            }
            if (line.empty()) {
                valid = false;
                return true;
            }
        } else {
        while (rowIndex >= rowsInBlock) {
            if (blocksRead >= blockCount) {
                if (rowsRead != rowCount || static_cast<std::uint64_t>(input.tellg()) != indexOffset) {
                    setError(error, "SST row count or block-index offset mismatch");
                    return false;
                }
                valid = false;
                return true;
            }
            if (!loadBlock(error)) return false;
        }
        std::uint32_t size = 0;
        if (!readU32(cursor, end, size) || size == 0 || size > kMaxFieldBytes ||
            end - cursor < static_cast<std::ptrdiff_t>(size)) {
            setError(error, "invalid SST cursor row");
            return false;
        }
        current = MsgpackRowView(reinterpret_cast<const std::uint8_t*>(cursor), size, indexFields);
        cursor += size;
        ++rowIndex;
        ++rowsRead;
        }
        const auto rawId = current.field("id");
        if (!rawId) {
            setError(error, "SST cursor row has no id");
            return false;
        }
        currentId = rawId->is_string() ? rawId->get<std::string>() : rawId->dump();
        if (currentId.empty() || (!previousId.empty() && currentId < previousId)) {
            setError(error, "SST cursor ids are empty or unsorted");
            return false;
        }
        previousId = currentId;
        valid = true;
        return true;
    }
};

SstCursor::SstCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SstCursor::SstCursor(SstCursor&&) noexcept = default;
SstCursor& SstCursor::operator=(SstCursor&&) noexcept = default;
SstCursor::~SstCursor() = default;

std::optional<SstCursor> SstCursor::open(const fs::path& path, std::string* error, bool indexFields) {
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->indexFields = indexFields;
    impl->input.open(path, std::ios::binary);
    if (!impl->input) {
        setError(error, "cannot open SST cursor");
        return std::nullopt;
    }
    std::array<char, kSstHeaderBytes> header{};
    impl->input.read(header.data(), static_cast<std::streamsize>(header.size()));
    impl->binary = impl->input && std::equal(kSstMagic.begin(), kSstMagic.end(), header.begin());
    impl->input.clear();
    impl->input.seekg(0, std::ios::beg);
    if (impl->binary) {
        if (!impl->input.read(header.data(), static_cast<std::streamsize>(header.size())) ||
            !parseSstHeader(header, impl->rowCount, impl->indexOffset,
                            impl->blockCount, error)) return std::nullopt;
    }
    if (!impl->advance(error)) return std::nullopt;
    return SstCursor(std::move(impl));
}

bool SstCursor::valid() const { return impl_ && impl_->valid; }
std::string_view SstCursor::id() const { return valid() ? impl_->currentId : std::string_view{}; }
const MsgpackRowView& SstCursor::row() const { return impl_->current; }
bool SstCursor::next(std::string* error) { return impl_ && impl_->advance(error); }

bool visitLatestSstRows(const std::vector<fs::path>& oldestToNewest,
                        const std::function<bool(const MsgpackRowView&)>& visitor,
                        std::string* error, bool indexFields, bool includeDeleted,
                        const std::function<void()>& check,
                        const std::function<bool(const MsgpackRowView&)>& eligible) {
    if (check) check();
    struct Item { std::string id; std::size_t index; };
    struct LaterId {
        bool operator()(const Item& left, const Item& right) const {
            if (left.id != right.id) return left.id > right.id;
            return left.index > right.index;
        }
    };
    std::vector<SstCursor> cursors;
    cursors.reserve(oldestToNewest.size());
    std::priority_queue<Item, std::vector<Item>, LaterId> heap;
    for (const auto& path : oldestToNewest) {
        if (check) check();
        auto cursor = SstCursor::open(path, error, indexFields);
        if (!cursor) return false;
        cursors.push_back(std::move(*cursor));
        if (cursors.back().valid()) {
            heap.push({std::string(cursors.back().id()), cursors.size() - 1});
        }
    }
    while (!heap.empty()) {
        if (check) check();
        const std::string id = heap.top().id;
        std::vector<std::size_t> matching;
        while (!heap.empty() && heap.top().id == id) {
            matching.push_back(heap.top().index);
            heap.pop();
        }
        bool found = false;
        std::optional<long long> newestVersion;
        std::size_t newestFile = 0;
        std::vector<std::uint8_t> newestBytes;
        // Consume the entire equal-ID run in every SST, including runs spanning
        // compressed blocks. Retain only one winning encoded row, never a
        // collection-sized document map or references into advanced cursors.
        for (const auto file : matching) {
            while (cursors[file].valid() && cursors[file].id() == id) {
                if (check) check();
                const auto& row = cursors[file].row();
                if (!eligible || eligible(row)) {
                    const auto version = rowVersion(row);
                    const bool replace = !found ||
                        (version && newestVersion && *version > *newestVersion) ||
                        (version.has_value() != newestVersion.has_value() && version) ||
                        (version == newestVersion && file >= newestFile);
                    if (replace) {
                        newestBytes.assign(row.data(), row.data() + row.size());
                        newestVersion = version; newestFile = file; found = true;
                    }
                }
                if (!cursors[file].next(error)) return false;
            }
            if (cursors[file].valid()) heap.push({std::string(cursors[file].id()), file});
        }
        if (found) {
            const MsgpackRowView selected(newestBytes.data(), newestBytes.size(), indexFields);
            const auto deleted = selected.field("_deleted");
            if ((includeDeleted || !(deleted && deleted->is_boolean() && deleted->get<bool>())) &&
                !visitor(selected)) return true;
        }
    }
    if (check) check();
    return true;
}

bool visitIndexMutations(const fs::path& directory,
                         const std::function<bool(const IndexMutation&)>& visitor,
                         std::string* error) {
    for (const auto& path : listIndexSegments(directory)) {
        IndexSegment segment;
        if (!readIndexSegment(path, segment, error)) return false;
        for (const auto& mutation : segment.mutations) {
            if (!visitor(mutation)) return true;
        }
    }
    return true;
}

std::optional<json> findInSst(const fs::path& path, const std::string& id,
                              std::string* error,
                              const std::function<bool(const MsgpackRowView&)>& eligible) {
    auto metadata = loadMetadata(path, error);
    if (!metadata) return std::nullopt;
    auto block = std::lower_bound(metadata->blocks.begin(), metadata->blocks.end(), id,
        [](const BlockIndexEntry& entry, const std::string& key) { return entry.lastKey < key; });
    std::ifstream input(path, std::ios::binary);
    if (!input) { setError(error, "cannot open binary SST"); return std::nullopt; }
    std::optional<json> found;
    std::optional<long long> newestVersion;
    std::string plain; std::uint32_t count = 0;
    try {
        // An equal-ID run may cross block boundaries. Inspect every matching
        // block, preserving the engine-version/later-row tie-breaking rule.
        for (; block != metadata->blocks.end() && block->firstKey <= id; ++block) {
            if (!readBlockPayload(input, *block, plain, count, error)) return std::nullopt;
            bool stopped = false;
            if (!visitBlockRows(plain, count, [&](const std::uint8_t* data, std::size_t size) {
                    const MsgpackRowView row(data, size, true);
                    const auto idValue = row.field("id");
                    const std::string key = idValue
                        ? (idValue->is_string() ? idValue->get<std::string>() : idValue->dump())
                        : rowId(row.materialize());
                    if (key == id && (!eligible || eligible(row))) {
                        const auto version = rowVersion(row);
                        if (!found || (version && newestVersion && *version >= *newestVersion) ||
                            (version.has_value() != newestVersion.has_value() && version) ||
                            version == newestVersion) {
                            found = row.materialize(); newestVersion = version;
                        }
                    }
                    return key <= id;
                }, error, &stopped)) return std::nullopt;
        }
    } catch (const std::exception& e) {
        setError(error, std::string("invalid SST MessagePack row: ") + e.what());
        return std::nullopt;
    }
    return found;
}

bool verifySst(const fs::path& path, std::string* error) {
    auto cursor = SstCursor::open(path, error);
    if (!cursor) return false;
    while (cursor->valid()) {
        try { (void)cursor->row().materialize(); }
        catch (const std::exception& exception) {
            setError(error, std::string("invalid SST row: ") + exception.what());
            return false;
        }
        if (!cursor->next(error)) return false;
    }
    if (!isBinarySst(path)) return true;

    std::ifstream input(path, std::ios::binary);
    std::array<char, kSstHeaderBytes> header{};
    std::uint64_t rows = 0, indexOffset = 0;
    std::uint32_t blocks = 0;
    if (!input.read(header.data(), static_cast<std::streamsize>(header.size())) ||
        !parseSstHeader(header, rows, indexOffset, blocks, error)) return false;
    std::error_code ec;
    const auto fileSize = fs::file_size(path, ec);
    if (ec || fileSize < indexOffset + kSstIndexMagic.size() + 8) {
        setError(error, "invalid SST block-index size");
        return false;
    }
    input.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
    const std::uint64_t checksumBytes = fileSize - indexOffset - 4;
    pacificdb::durability::ChecksumCalculator::IncrementalCRC32 checksum;
    std::array<char, 64 * 1024> buffer{};
    std::string prefix;
    prefix.reserve(kSstIndexMagic.size() + 4);
    std::uint64_t remaining = checksumBytes;
    while (remaining > 0) {
        const auto amount = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        if (!input.read(buffer.data(), amount)) {
            setError(error, "truncated SST block index");
            return false;
        }
        if (prefix.size() < kSstIndexMagic.size() + 4) {
            const size_t copy = std::min<size_t>(static_cast<size_t>(amount),
                kSstIndexMagic.size() + 4 - prefix.size());
            prefix.append(buffer.data(), copy);
        }
        checksum.update(buffer.data(), static_cast<size_t>(amount));
        remaining -= static_cast<std::uint64_t>(amount);
    }
    std::array<char, 4> storedBytes{};
    if (!input.read(storedBytes.data(), 4)) {
        setError(error, "missing SST block-index checksum");
        return false;
    }
    const char* storedCursor = storedBytes.data();
    const char* storedEnd = storedBytes.data() + storedBytes.size();
    std::uint32_t stored = 0, footerBlocks = 0;
    const char* prefixCursor = prefix.data() + kSstIndexMagic.size();
    const char* prefixEnd = prefix.data() + prefix.size();
    if (prefix.size() < kSstIndexMagic.size() + 4 ||
        !std::equal(kSstIndexMagic.begin(), kSstIndexMagic.end(), prefix.begin()) ||
        !readU32(prefixCursor, prefixEnd, footerBlocks) || footerBlocks != blocks ||
        !readU32(storedCursor, storedEnd, stored) || checksum.finalize() != stored) {
        setError(error, "SST block-index checksum or count mismatch");
        return false;
    }
    return true;
}

bool buildBloom(const fs::path& sstPath, const std::vector<std::string>& keys,
                std::string* error) {
    try {
        const std::uint64_t bitCount = std::max<std::uint64_t>(64,
            ((static_cast<std::uint64_t>(keys.size()) * 10 + 63) / 64) * 64);
        constexpr std::uint32_t hashes = 7;
        std::vector<std::uint64_t> words(static_cast<std::size_t>(bitCount / 64), 0);
        for (const auto& key : keys) {
            const std::uint64_t first = fnv1a(key, 0x9e3779b97f4a7c15ULL);
            const std::uint64_t second = fnv1a(key, 0xc2b2ae3d27d4eb4fULL) | 1ULL;
            for (std::uint32_t i = 0; i < hashes; ++i) {
                const std::uint64_t bit = (first + i * second) % bitCount;
                words[static_cast<std::size_t>(bit / 64)] |= 1ULL << (bit % 64);
            }
        }
        std::string header(kBloomMagic.begin(), kBloomMagic.end());
        appendU32(header, kVersion);
        appendU32(header, hashes);
        appendU64(header, bitCount);
        appendU64(header, keys.size());
        appendU64(header, words.size());
        appendU32(header, crc32c(words.data(), words.size() * sizeof(std::uint64_t)));
        appendU32(header, 0);
        if (header.size() != kBloomHeaderBytes) throw std::runtime_error("internal bloom header size");
        const fs::path finalPath = sstPath.string() + ".bloom";
        const fs::path temporary = finalPath.string() + ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(header.data(), static_cast<std::streamsize>(header.size()));
            out.write(reinterpret_cast<const char*>(words.data()),
                      static_cast<std::streamsize>(words.size() * sizeof(std::uint64_t)));
            out.flush();
            if (!out) {
                setError(error, "cannot write bloom sidecar");
                return false;
            }
        }
        if (!syncFile(temporary)) {
            setError(error, "cannot fdatasync bloom sidecar");
            return false;
        }
        std::error_code ec;
        pacificdb::durable_file::rename(temporary, finalPath, ec);
        if (ec) {
            fs::remove(finalPath, ec);
            ec.clear();
            pacificdb::durable_file::rename(temporary, finalPath, ec);
        }
        if (ec || !syncFile(finalPath.parent_path(), true)) {
            setError(error, "cannot publish bloom sidecar: " + ec.message());
            return false;
        }
        invalidateCaches(finalPath);
        return true;
    } catch (const std::exception& e) {
        setError(error, e.what());
        return false;
    }
}

std::optional<bool> bloomMayContain(const fs::path& sstPath, const std::string& key) {
    const auto view = mapBloom(sstPath.string() + ".bloom");
    if (!view) return std::nullopt;
    const std::uint64_t first = fnv1a(key, 0x9e3779b97f4a7c15ULL);
    const std::uint64_t second = fnv1a(key, 0xc2b2ae3d27d4eb4fULL) | 1ULL;
    for (std::uint32_t i = 0; i < view->hashes; ++i) {
        const std::uint64_t bit = (first + i * second) % view->bitCount;
        if ((view->words[static_cast<std::size_t>(bit / 64)] & (1ULL << (bit % 64))) == 0) {
            return false;
        }
    }
    return true;
}

void invalidateCaches(const fs::path& pathPrefix) {
    const std::string prefix = pathPrefix.string();
    {
        std::lock_guard<std::mutex> lock(g_sstCacheMutex);
        for (auto it = g_sstCache.begin(); it != g_sstCache.end();) {
            if (prefix.empty() || it->first.rfind(prefix, 0) == 0) it = g_sstCache.erase(it);
            else ++it;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_bloomCacheMutex);
        for (auto it = g_bloomCache.begin(); it != g_bloomCache.end();) {
            if (prefix.empty() || it->first.rfind(prefix, 0) == 0) it = g_bloomCache.erase(it);
            else ++it;
        }
    }
}

}  // namespace pacificdb::storage_v2
