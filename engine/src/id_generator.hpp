#ifndef ID_GENERATOR_HPP
#define ID_GENERATOR_HPP

#include <string>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <vector>
#include <cstdint>


class IDGenerator {
private:
    inline static std::mutex counterMutex;
    inline static uint32_t counter = 0;
    inline static std::vector<unsigned char> randomBytes;
    inline static bool initialized = false;

    static void initialize() {
        if (!initialized) {
            std::lock_guard<std::mutex> lock(counterMutex);
            if (!initialized) {
                // Generate 5 random bytes for machine/process identifier
                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_int_distribution<> dis(0, 255);

                randomBytes.resize(5);
                for (int i = 0; i < 5; i++) {
                    randomBytes[i] = static_cast<unsigned char>(dis(gen));
                }

                // Initialize counter with random value
                counter = dis(gen) | (dis(gen) << 8) | (dis(gen) << 16);
                counter &= 0xFFFFFF; // 3 bytes max

                initialized = true;
            }
        }
    }

public:
    /**
     * Generate MongoDB-style ObjectID (24 hex characters)
     * Example: 67a4e8f3c2d1b9a5e6f4g7h8
     */
    static std::string generateObjectId() {
        initialize();

        std::lock_guard<std::mutex> lock(counterMutex);

        // Increment counter (wrap at 0xFFFFFF)
        counter = (counter + 1) & 0xFFFFFF;

        // Get current timestamp (seconds since epoch)
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        uint32_t timestamp = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(epoch).count()
        );

        // Build 12-byte array
        unsigned char bytes[12];

        // Timestamp (4 bytes, big-endian)
        bytes[0] = (timestamp >> 24) & 0xFF;
        bytes[1] = (timestamp >> 16) & 0xFF;
        bytes[2] = (timestamp >> 8) & 0xFF;
        bytes[3] = timestamp & 0xFF;

        // Random bytes (5 bytes)
        for (int i = 0; i < 5; i++) {
            bytes[4 + i] = randomBytes[i];
        }

        // Counter (3 bytes, big-endian)
        bytes[9] = (counter >> 16) & 0xFF;
        bytes[10] = (counter >> 8) & 0xFF;
        bytes[11] = counter & 0xFF;

        // Convert to hex string
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 12; i++) {
            ss << std::setw(2) << static_cast<int>(bytes[i]);
        }

        return ss.str();
    }

    /**
     * Generate UUID v4 style ID (32 hex characters without dashes)
     * Example: 550e8400e29b41d4a716446655440000
     */
    static std::string generateUUID() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        unsigned char bytes[16];
        for (int i = 0; i < 16; i++) {
            bytes[i] = static_cast<unsigned char>(dis(gen));
        }

        // Set version (4) and variant bits
        bytes[6] = (bytes[6] & 0x0F) | 0x40;  // Version 4
        bytes[8] = (bytes[8] & 0x3F) | 0x80;  // Variant

        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (int i = 0; i < 16; i++) {
            ss << std::setw(2) << static_cast<int>(bytes[i]);
        }

        return ss.str();
    }

    /**
     * Generate short alphanumeric ID (12 characters by default)
     * Example: x4k9m2n7p5q8
     */
    static std::string generateShortId(int length = 12) {
        const char* chars = "abcdefghijklmnopqrstuvwxyz0123456789";
        const int charLen = 36;

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, charLen - 1);

        std::string result;
        result.reserve(length);

        for (int i = 0; i < length; i++) {
            result += chars[dis(gen)];
        }

        return result;
    }

    /**
     * Generate prefixed ID
     * Example: media_67a4e8f3c2d1b9a5e6f4g7h8
     */
    static std::string generatePrefixedId(const std::string& prefix) {
        return prefix + "_" + generateObjectId();
    }

    /**
     * Validate ObjectID format (24 hex characters)
     */
    static bool isValidObjectId(const std::string& id) {
        if (id.length() != 24) return false;

        for (char c : id) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }

        return true;
    }
};

// No separate static member definitions required (inline statics in header)

#endif // ID_GENERATOR_HPP
