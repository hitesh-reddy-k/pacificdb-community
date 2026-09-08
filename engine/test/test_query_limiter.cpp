#include "../include/query_limiter.hpp"
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <chrono>

void test_timeout() {
    std::cout << "\n[TEST] Testing query timeout..." << std::endl;

    // Set a very short timeout for testing (2 seconds)
#ifdef _WIN32
    _putenv_s("MAX_QUERY_TIMEOUT_SEC", "2");
#else
    setenv("MAX_QUERY_TIMEOUT_SEC", "2", 1);
#endif

    QueryLimiter::init();
    QueryLimiter::startQuery("test_timeout");

    // Sleep longer than timeout
    std::this_thread::sleep_for(std::chrono::seconds(3));

    try {
        QueryLimiter::checkTimeout("test_timeout");
        std::cerr << "❌ FAILED: Timeout should have been triggered!" << std::endl;
        assert(false);
    } catch (const std::exception& e) {
        std::cout << "✅ PASSED: Timeout caught: " << e.what() << std::endl;
    }
}

void test_timeout_not_exceeded() {
    std::cout << "\n[TEST] Testing query within timeout..." << std::endl;

    QueryLimiter::startQuery("test_no_timeout");

    // Sleep shorter than timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    try {
        QueryLimiter::checkTimeout("test_no_timeout");
        std::cout << "✅ PASSED: Query within timeout limit" << std::endl;
        QueryLimiter::endQuery("test_no_timeout");
    } catch (const std::exception& e) {
        std::cerr << "❌ FAILED: Should not have timed out: " << e.what() << std::endl;
        assert(false);
    }
}

void test_scan_limit() {
    std::cout << "\n[TEST] Testing scan limit..." << std::endl;

    try {
        QueryLimiter::checkScanLimit(2000000); // Exceeds default 1M
        std::cerr << "❌ FAILED: Scan limit should have been triggered!" << std::endl;
        assert(false);
    } catch (const std::exception& e) {
        std::cout << "✅ PASSED: Scan limit caught: " << e.what() << std::endl;
    }
}

void test_scan_within_limit() {
    std::cout << "\n[TEST] Testing scan within limit..." << std::endl;

    try {
        QueryLimiter::checkScanLimit(500000); // Within 1M limit
        std::cout << "✅ PASSED: Scan within limit" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "❌ FAILED: Should not have exceeded limit: " << e.what() << std::endl;
        assert(false);
    }
}

void test_result_size_limit() {
    std::cout << "\n[TEST] Testing result size limit..." << std::endl;

    // Create a large result set
    std::vector<nlohmann::json> largeResults;
    for (int i = 0; i < 200000; i++) {
        nlohmann::json doc;
        doc["id"] = i;
        doc["data"] = std::string(1024, 'X'); // 1KB per document
        largeResults.push_back(doc);
    }

    try {
        QueryLimiter::checkResultSize(largeResults);
        std::cerr << "❌ FAILED: Result size limit should have been triggered!" << std::endl;
        assert(false);
    } catch (const std::exception& e) {
        std::cout << "✅ PASSED: Result size limit caught: " << e.what() << std::endl;
    }
}

void test_result_size_within_limit() {
    std::cout << "\n[TEST] Testing result size within limit..." << std::endl;

    std::vector<nlohmann::json> smallResults;
    for (int i = 0; i < 100; i++) {
        nlohmann::json doc;
        doc["id"] = i;
        doc["name"] = "test_" + std::to_string(i);
        smallResults.push_back(doc);
    }

    try {
        QueryLimiter::checkResultSize(smallResults);
        std::cout << "✅ PASSED: Result size within limit" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "❌ FAILED: Should not have exceeded limit: " << e.what() << std::endl;
        assert(false);
    }
}

void test_concurrent_queries() {
    std::cout << "\n[TEST] Testing concurrent query tracking..." << std::endl;

    // Start multiple queries
    for (int i = 0; i < 10; i++) {
        QueryLimiter::startQuery("concurrent_" + std::to_string(i));
    }

    // Check they don't timeout immediately
    for (int i = 0; i < 10; i++) {
        try {
            QueryLimiter::checkTimeout("concurrent_" + std::to_string(i));
        } catch (const std::exception& e) {
            std::cerr << "❌ FAILED: Query " << i << " should not timeout: " << e.what() << std::endl;
            assert(false);
        }
    }

    // End all queries
    for (int i = 0; i < 10; i++) {
        QueryLimiter::endQuery("concurrent_" + std::to_string(i));
    }

    std::cout << "✅ PASSED: Concurrent query tracking works" << std::endl;
}

void test_configuration_from_env() {
    std::cout << "\n[TEST] Testing configuration from environment..." << std::endl;

    // Set custom values
#ifdef _WIN32
    _putenv_s("MAX_QUERY_TIMEOUT_SEC", "60");
    _putenv_s("MAX_RESULT_SIZE_MB", "200");
    _putenv_s("MAX_SCAN_ROWS", "2000000");
#else
    setenv("MAX_QUERY_TIMEOUT_SEC", "60", 1);
    setenv("MAX_RESULT_SIZE_MB", "200", 1);
    setenv("MAX_SCAN_ROWS", "2000000", 1);
#endif

    QueryLimiter::init();

    // Verify configuration was loaded
    if (QueryLimiter::getMaxQueryTime().count() == 60 &&
        QueryLimiter::getMaxResultSize() == 200 * 1024 * 1024 &&
        QueryLimiter::getMaxScanRows() == 2000000) {
        std::cout << "✅ PASSED: Configuration loaded from environment" << std::endl;
    } else {
        std::cerr << "❌ FAILED: Configuration not loaded correctly" << std::endl;
        assert(false);
    }
}

void print_summary() {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "   QUERY LIMITER TEST SUITE - ALL TESTS PASSED ✅" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "\n[SUMMARY] Tested features:" << std::endl;
    std::cout << "  ✅ Query timeout enforcement" << std::endl;
    std::cout << "  ✅ Query within timeout handling" << std::endl;
    std::cout << "  ✅ Scan limit enforcement" << std::endl;
    std::cout << "  ✅ Scan within limit handling" << std::endl;
    std::cout << "  ✅ Result size limit enforcement" << std::endl;
    std::cout << "  ✅ Result size within limit handling" << std::endl;
    std::cout << "  ✅ Concurrent query tracking" << std::endl;
    std::cout << "  ✅ Environment variable configuration" << std::endl;
    std::cout << "\n[RESULT] Query Resource Limits Implementation: COMPLETE ✅\n" << std::endl;
}

int main() {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "   RUNNING QUERY LIMITER TEST SUITE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    try {
        test_configuration_from_env();
        test_timeout();
        test_timeout_not_exceeded();
        test_scan_limit();
        test_scan_within_limit();
        test_result_size_limit();
        test_result_size_within_limit();
        test_concurrent_queries();

        print_summary();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST SUITE FAILED: " << e.what() << std::endl;
        return 1;
    }
}
