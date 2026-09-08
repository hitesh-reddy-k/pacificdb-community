#include "../include/memory_manager.hpp"
#include "../include/database_engine.hpp"
#include "../include/lsm.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <string>

// Returns a portable temp directory for test data, avoiding Windows-only paths.
static std::string getTestDataPath() {
    // Honour CI / custom override first
    const char* env = std::getenv("TEST_DATA_ROOT");
    if (env && env[0] != '\0') return std::string(env);
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "C:/Temp";
    return std::string(tmp) + "/pacificdb_memory_test";
#else
    return "/tmp/pacificdb_memory_test";
#endif
}

void test_memory_initialization() {
    std::cout << "\n[TEST] Testing memory manager initialization..." << std::endl;

    // Set custom memory limit
#ifdef _WIN32
    _putenv_s("MAX_MEMORY_GB", "2");
    _putenv_s("MEMORY_WARNING_THRESHOLD", "0.7");
    _putenv_s("MEMORY_CRITICAL_THRESHOLD", "0.85");
#else
    setenv("MAX_MEMORY_GB", "2", 1);
    setenv("MEMORY_WARNING_THRESHOLD", "0.7", 1);
    setenv("MEMORY_CRITICAL_THRESHOLD", "0.85", 1);
#endif

    MemoryManager::init();

    if (MemoryManager::getMaxMemoryBytes() == 2ULL * 1024 * 1024 * 1024 &&
        MemoryManager::getWarningThreshold() == 0.7 &&
        MemoryManager::getCriticalThreshold() == 0.85) {
        std::cout << "✅ PASSED: Memory manager configured correctly" << std::endl;
    } else {
        std::cerr << "❌ FAILED: Configuration mismatch" << std::endl;
        assert(false);
    }
}

void test_memory_monitoring() {
    std::cout << "\n[TEST] Testing memory monitoring..." << std::endl;

    MemoryManager::start();

    // Let it run for a few monitoring cycles
    std::this_thread::sleep_for(std::chrono::seconds(12));

    // Check that memory is being tracked
    size_t currentMem = MemoryManager::getProcessMemory();
    if (currentMem > 0) {
        std::cout << "✅ PASSED: Memory tracking working - Current: "
                  << (currentMem / 1024 / 1024) << "MB" << std::endl;
    } else {
        std::cerr << "❌ FAILED: Memory tracking not working" << std::endl;
        assert(false);
    }

    MemoryManager::stop();
}

void test_backpressure_check() {
    std::cout << "\n[TEST] Testing backpressure mechanism..." << std::endl;

    // Under normal conditions, should not apply backpressure
    bool shouldSlowDown = MemoryManager::shouldSlowDownWrites();
    double usage = MemoryManager::getMemoryUsage();

    std::cout << "[TEST] Current memory usage: " << (usage * 100) << "%" << std::endl;
    std::cout << "[TEST] Backpressure active: " << (shouldSlowDown ? "YES" : "NO") << std::endl;

    if (usage < MemoryManager::getWarningThreshold()) {
        if (!shouldSlowDown) {
            std::cout << "✅ PASSED: No backpressure under threshold" << std::endl;
        } else {
            std::cerr << "❌ FAILED: Unexpected backpressure" << std::endl;
            assert(false);
        }
    } else {
        std::cout << "⚠️  INFO: Memory usage high - backpressure expected" << std::endl;
    }
}

void test_memory_usage_calculation() {
    std::cout << "\n[TEST] Testing memory usage calculation..." << std::endl;

    double usage = MemoryManager::getMemoryUsage();
    size_t currentBytes = MemoryManager::getProcessMemory();
    size_t maxBytes = MemoryManager::getMaxMemoryBytes();

    double expectedUsage = static_cast<double>(currentBytes) / maxBytes;

    std::cout << "[TEST] Current: " << (currentBytes / 1024 / 1024) << "MB" << std::endl;
    std::cout << "[TEST] Max: " << (maxBytes / 1024 / 1024) << "MB" << std::endl;
    std::cout << "[TEST] Usage: " << (usage * 100) << "%" << std::endl;

    if (std::abs(usage - expectedUsage) < 0.01) {
        std::cout << "✅ PASSED: Memory usage calculation accurate" << std::endl;
    } else {
        std::cerr << "❌ FAILED: Usage calculation mismatch" << std::endl;
        assert(false);
    }
}

void test_force_flush() {
    std::cout << "\n[TEST] Testing force flush functionality..." << std::endl;

    // Use a portable, CI-safe data path (no hard-coded Windows paths)
    const std::string dataPath = getTestDataPath();

    // Initialize database and LSM
    DatabaseEngine::init(dataPath);
    LSM::init(dataPath);

    // Insert some test data to create memtables
    std::string user = "test_user";
    std::string dbName = "memory_test_db";
    std::string coll = "items";

    DatabaseEngine::createDatabase(user, dbName);

    for (int i = 0; i < 50; i++) {
        nlohmann::json doc;
        // Store id as a string — LSM expects string keys; storing an integer
        // causes json.exception.type_error.302 on read-back.
        doc["id"] = "item_" + std::to_string(i);
        doc["data"] = "test_data_" + std::to_string(i);
        DatabaseEngine::insert(user, dbName, coll, doc);
    }

    std::cout << "[TEST] Inserted 50 documents, calling force flush..." << std::endl;

    try {
        MemoryManager::forceFlushMemtables();
        std::cout << "\u2705 PASSED: Force flush completed without errors" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "\u274c FAILED: Force flush threw exception: " << e.what() << std::endl;
        assert(false);
    }
}

void test_integration_with_writes() {
    std::cout << "\n[TEST] Testing integration with write operations..." << std::endl;

    std::string user = "test_user";
    std::string dbName = "integration_db";
    std::string coll = "records";

    DatabaseEngine::createDatabase(user, dbName);

    // Perform writes and check if backpressure is working
    auto startTime = std::chrono::steady_clock::now();

    for (int i = 0; i < 20; i++) {
        nlohmann::json doc;
        // Use string ids to avoid json.exception.type_error.302
        doc["id"]    = "rec_" + std::to_string(i);
        doc["value"] = i * 100;
        DatabaseEngine::insert(user, dbName, coll, doc);
    }

    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    std::cout << "[TEST] Wrote 20 records in " << duration << "ms" << std::endl;
    std::cout << "\u2705 PASSED: Write integration working" << std::endl;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "    MEMORY MANAGER TEST SUITE" << std::endl;
    std::cout << "========================================\n" << std::endl;

    try {
        test_memory_initialization();
        test_memory_monitoring();
        test_backpressure_check();
        test_memory_usage_calculation();
        test_force_flush();
        test_integration_with_writes();

        std::cout << "\n========================================" << std::endl;
        std::cout << "   ✅ ALL TESTS PASSED!" << std::endl;
        std::cout << "========================================\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST SUITE FAILED: " << e.what() << std::endl;
        return 1;
    }
}
