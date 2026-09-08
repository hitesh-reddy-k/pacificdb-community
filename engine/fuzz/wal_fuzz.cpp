#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wal.hpp"
#include "wal_integrity.hpp"

using json = nlohmann::json;

static uint32_t readU32LE(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

static void fuzzBinaryWal(const uint8_t* data, size_t size) {
    size_t off = 0;
    size_t entries = 0;

    while (off + 5 <= size && entries < 128) {
        uint8_t op = data[off];
        (void)op;
        off += 1;
        uint32_t payloadSize = readU32LE(data + off);
        off += 4;
        if (payloadSize > (size - off)) break;

        std::string payload(reinterpret_cast<const char*>(data + off), payloadSize);
        off += payloadSize;
        entries += 1;

        try {
            if (!payload.empty() && (payload[0] == '{' || payload[0] == '[')) {
                json::parse(payload);
            } else {
                std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
                json::from_msgpack(bytes);
            }
        } catch (...) {
            // swallow parse errors
        }
    }
}

static void fuzzIntegrityWal(const uint8_t* data, size_t size) {
    if (size == 0) return;
    std::string raw(reinterpret_cast<const char*>(data), size);
    try {
        auto j = json::parse(raw, nullptr, false);
        if (j.is_object()) {
            WALRecord rec = WALRecord::fromJson(j);
            rec.verifyChecksum();
        }
    } catch (...) {
        // ignore
    }
}

static void fuzzCompression(const uint8_t* data, size_t size) {
    if (size > (1u << 20)) return; // avoid excessive memory
    std::string input(reinterpret_cast<const char*>(data), size);
    try {
        std::string compressed = WAL::compressPayload(input);
        WAL::decompressPayload(compressed);
    } catch (...) {
        // ignore
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzzBinaryWal(data, size);
    fuzzIntegrityWal(data, size);
    fuzzCompression(data, size);
    return 0;
}
