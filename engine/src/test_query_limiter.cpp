#include <iostream>
#include <vector>
#include "database_engine.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
int main() {
    try {
        std::cout << "[QUERY-LIMIT-TEST] Initializing engine test data...\n";
        const OwnedTestRoot ownedRoot("query-limiter");
        DatabaseEngine::init(ownedRoot.dataRoot().string());

        std::string user = "system";
        std::string dbName = "ql_test_db";
        std::string coll = "items";

        DatabaseEngine::createDatabase(user, dbName, "binary");
        DatabaseEngine::createCollection(user, dbName, coll);

        // Insert 200 small documents
        for (int i = 0; i < 200; ++i) {
            // id must be string to satisfy LSM expectations
            json doc = { {"id", std::to_string(i)}, {"value", i} };
            DatabaseEngine::insert(user, dbName, coll, doc);
        }

        // Set tight limits to force truncation
        QueryLimiter::setMaxScanRows(50);
        QueryLimiter::setMaxResultDocs(20);

        auto res = DatabaseEngine::find(user, dbName, coll, json::object());

        std::string reason;
        bool truncated = QueryLimiter::consumeTruncatedFlag(reason);

        std::cout << "[QUERY-LIMIT-TEST] results=" << res.size() << " truncated=" << truncated << " reason=" << reason << "\n";

        if (truncated) {
            std::cout << "[QUERY-LIMIT-TEST] SUCCESS: Query was truncated as expected\n";
            return 0;
        }

        std::cerr << "[QUERY-LIMIT-TEST] FAILURE: Query was NOT truncated\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[QUERY-LIMIT-TEST] EXCEPTION: " << e.what() << "\n";
        return -1;
    } catch (...) {
        std::cerr << "[QUERY-LIMIT-TEST] UNKNOWN EXCEPTION\n";
        return -2;
    }
}
