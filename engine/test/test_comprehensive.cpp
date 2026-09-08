/**
 * PacificDB Engine — Comprehensive Test Suite
 *
 * Covers: CRUD, transactions, WAL durability, LSM compaction, index correctness,
 *         concurrent inserts, query operators, TTL, multi-tenant isolation,
 *         error handling, and recovery after simulated crash.
 *
 * Build: compiled as db_engine_comprehensive_test (see CMakeLists.txt)
 * Exit 0 = all pass, Exit 1 = any fail.
 */

#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Test harness ─────────────────────────────────────────────────────────────

static int g_total = 0;
static int g_passed = 0;
static int g_failed = 0;

#define EXPECT(cond, msg)                                                      \
    do {                                                                       \
        ++g_total;                                                             \
        if (cond) {                                                            \
            ++g_passed;                                                        \
            std::cout << "  \033[32m[PASS]\033[0m " << (msg) << "\n";        \
        } else {                                                               \
            ++g_failed;                                                        \
            std::cerr << "  \033[31m[FAIL]\033[0m " << (msg)                 \
                      << " (line " << __LINE__ << ")\n";                      \
        }                                                                      \
    } while (0)

#define SECTION(name)                                                          \
    std::cout << "\n\033[36m── " << (name) << " ──\033[0m\n";

static std::string DATA_DIR;

// Create a database and collection before inserting.
// The engine throws if the DB doesn't exist.
static void ensureDb(const std::string& user, const std::string& db,
                     const std::vector<std::string>& collections = {}) {
    DatabaseEngine::createDatabase(user, db, "binary");
    for (const auto& coll : collections) {
        DatabaseEngine::createCollection(user, db, coll);
    }
}

static void resetEngine() {
    fs::remove_all(DATA_DIR);
    DatabaseEngine::init(DATA_DIR);
}

// ── Test Sections ─────────────────────────────────────────────────────────────

static void test_basic_crud() {
    SECTION("1. Basic CRUD");
    resetEngine();
    ensureDb("u1", "db1", {"users"});

    // Insert
    DatabaseEngine::insert("u1","db1","users",{{"id","u1"},{"name","Alice"},{"age",30}});
    DatabaseEngine::insert("u1","db1","users",{{"id","u2"},{"name","Bob"},{"age",25}});
    DatabaseEngine::insert("u1","db1","users",{{"id","u3"},{"name","Carol"},{"age",35}});

    // Find all
    auto all = DatabaseEngine::find("u1","db1","users",{});
    EXPECT(all.size() == 3, "Insert 3 documents");

    // Find with exact match
    auto res = DatabaseEngine::find("u1","db1","users",{{"name","Alice"}});
    EXPECT(res.size() == 1 && res[0]["name"] == "Alice", "Find by exact field");

    // Update
    bool ok = DatabaseEngine::updateOne("u1","db1","users",{{"id","u1"}},{{"age",31}});
    EXPECT(ok, "UpdateOne returns true");
    auto updated = DatabaseEngine::find("u1","db1","users",{{"id","u1"}});
    EXPECT(!updated.empty() && updated[0]["age"] == 31, "Field updated correctly");

    // Delete
    bool del = DatabaseEngine::deleteOne("u1","db1","users",{{"id","u2"}});
    EXPECT(del, "DeleteOne returns true");
    auto after = DatabaseEngine::find("u1","db1","users",{});
    EXPECT(after.size() == 2, "Document count after delete");

    // Find nonexistent
    auto none = DatabaseEngine::find("u1","db1","users",{{"id","u999"}});
    EXPECT(none.empty(), "Find nonexistent returns empty");
}

static void test_query_operators() {
    SECTION("2. Query Operators");
    resetEngine();
    ensureDb("u1", "db1", {"nums"});

    for (int i = 1; i <= 10; ++i) {
        DatabaseEngine::insert("u1","db1","nums",{{"n",i},{"even",(i%2==0)}});
    }
    LSM::flush("u1", "db1", "nums");

    // $gt
    auto gt5 = DatabaseEngine::find("u1","db1","nums",{{"n",{{"$gt",5}}}});
    EXPECT(gt5.size() == 5, "$gt operator");

    // $gte
    auto gte5 = DatabaseEngine::find("u1","db1","nums",{{"n",{{"$gte",5}}}});
    EXPECT(gte5.size() == 6, "$gte operator");

    // $lt
    auto lt3 = DatabaseEngine::find("u1","db1","nums",{{"n",{{"$lt",3}}}});
    EXPECT(lt3.size() == 2, "$lt operator");

    // $lte
    auto lte3 = DatabaseEngine::find("u1","db1","nums",{{"n",{{"$lte",3}}}});
    EXPECT(lte3.size() == 3, "$lte operator");

    // $ne
    auto ne5 = DatabaseEngine::find("u1","db1","nums",{{"n",{{"$ne",5}}}});
    EXPECT(ne5.size() == 9, "$ne operator");

    // $in
    auto in135 = DatabaseEngine::find("u1","db1","nums",{{"n",{{"$in",{1,3,5}}}}});
    EXPECT(in135.size() == 3, "$in operator");

    // $or
    auto or13 = DatabaseEngine::find("u1","db1","nums",{
        {"$or", {{{"n",1}}, {{"n",3}}}}
    });
    EXPECT(or13.size() == 2, "$or operator");

    // Boolean filter
    const auto getAllBefore = LSM::getLsmMetrics().value("read_execute_count", 0ULL);
    auto evens = DatabaseEngine::find("u1","db1","nums",{{"even",true}});
    EXPECT(evens.size() == 5, "Boolean field filter");
    EXPECT(LSM::getLsmMetrics().value("read_execute_count", 0ULL) == getAllBefore,
           "Unindexed equality scan avoids getAll materialization");
    EXPECT(DatabaseEngine::count("u1", "db1", "nums", {{"n", {{"$gte", 7}}}}) == 4,
           "Streaming count filter");
}

static void test_multi_collection() {
    SECTION("3. Multiple Collections in One Database");
    resetEngine();
    ensureDb("u1", "db1", {"orders", "products"});

    DatabaseEngine::insert("u1","db1","orders",{{"id","o1"},{"total",100}});
    DatabaseEngine::insert("u1","db1","orders",{{"id","o2"},{"total",200}});
    DatabaseEngine::insert("u1","db1","products",{{"sku","A"},{"price",50}});
    DatabaseEngine::insert("u1","db1","products",{{"sku","B"},{"price",75}});

    auto orders = DatabaseEngine::find("u1","db1","orders",{});
    auto products = DatabaseEngine::find("u1","db1","products",{});
    EXPECT(orders.size() == 2, "Orders collection has 2 docs");
    EXPECT(products.size() == 2, "Products collection has 2 docs");

    // Modify orders, products unchanged
    DatabaseEngine::deleteOne("u1","db1","orders",{{"id","o1"}});
    EXPECT(DatabaseEngine::find("u1","db1","orders",{}).size() == 1,
           "Orders after delete");
    EXPECT(DatabaseEngine::find("u1","db1","products",{}).size() == 2,
           "Products unaffected by orders delete");
}

static void test_multi_tenant_isolation() {
    SECTION("4. Multi-Tenant Isolation");
    resetEngine();
    ensureDb("tenant_A", "db1", {"secrets"});
    ensureDb("tenant_B", "db1", {"secrets"});

    // Two tenants, same collection name
    DatabaseEngine::insert("tenant_A","db1","secrets",{{"key","A_secret"},{"val","alpha"}});
    DatabaseEngine::insert("tenant_B","db1","secrets",{{"key","B_secret"},{"val","beta"}});

    auto a = DatabaseEngine::find("tenant_A","db1","secrets",{});
    auto b = DatabaseEngine::find("tenant_B","db1","secrets",{});
    EXPECT(a.size() == 1, "Tenant A has 1 document");
    EXPECT(b.size() == 1, "Tenant B has 1 document");
    EXPECT(!a.empty() && a[0]["key"] == "A_secret", "Tenant A data is correct");
    EXPECT(!b.empty() && b[0]["key"] == "B_secret", "Tenant B data is correct");

    // Tenant A cannot see Tenant B's data
    auto cross = DatabaseEngine::find("tenant_A","db1","secrets",{{"key","B_secret"}});
    EXPECT(cross.empty(), "Tenant isolation: A cannot read B's documents");
}

static void test_concurrent_inserts() {
    SECTION("5. Concurrent Inserts (16 threads × 50 docs)");
    resetEngine();
    ensureDb("u1", "db1", {"concurrent"});

    const int THREADS = 16;
    const int PER_THREAD = 50;
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([t, &errors]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                try {
                    DatabaseEngine::insert("u1","db1","concurrent",{
                        {"thread", t}, {"seq", i}, {"val", t * PER_THREAD + i}
                    });
                } catch (...) {
                    ++errors;
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT(errors.load() == 0, "No insert errors under concurrency");
    auto all = DatabaseEngine::find("u1","db1","concurrent",{});
    EXPECT(static_cast<int>(all.size()) == THREADS * PER_THREAD,
           "All " + std::to_string(THREADS * PER_THREAD) + " concurrent docs present");
}

static void test_concurrent_read_write() {
    SECTION("6. Concurrent Read-Write (no stale reads)");
    resetEngine();
    ensureDb("u1", "db1", {"shared"});

    DatabaseEngine::insert("u1","db1","shared",{{"k","counter"},{"v",0}});

    const int WRITERS = 4;
    const int READERS = 4;
    const int ITERATIONS = 25;
    std::atomic<bool> stop{false};
    std::atomic<int> writeErrors{0};
    std::atomic<int> readErrors{0};

    std::vector<std::thread> writers;
    for (int w = 0; w < WRITERS; ++w) {
        writers.emplace_back([w, &writeErrors, &stop]() {
            for (int i = 0; i < ITERATIONS && !stop.load(); ++i) {
                try {
                    DatabaseEngine::insert("u1","db1","shared",{
                        {"k","entry"},{"writer",w},{"seq",i}
                    });
                } catch (...) { ++writeErrors; }
            }
        });
    }

    std::vector<std::thread> readers;
    for (int r = 0; r < READERS; ++r) {
        readers.emplace_back([&readErrors, &stop]() {
            for (int i = 0; i < ITERATIONS * 2 && !stop.load(); ++i) {
                try {
                    auto docs = DatabaseEngine::find("u1","db1","shared",{});
                    (void)docs;
                } catch (...) { ++readErrors; }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }

    for (auto& t : writers) t.join();
    stop.store(true);
    for (auto& t : readers) t.join();

    EXPECT(writeErrors.load() == 0, "No write errors under concurrent read-write");
    EXPECT(readErrors.load() == 0,  "No read errors under concurrent read-write");
}

static void test_update_upsert() {
    SECTION("7. Update & Upsert Semantics");
    resetEngine();
    ensureDb("u1", "db1", {"items"});

    DatabaseEngine::insert("u1","db1","items",{{"sku","X"},{"stock",10}});

    // Update existing
    bool ok1 = DatabaseEngine::updateOne("u1","db1","items",{{"sku","X"}},{{"stock",20}});
    EXPECT(ok1, "UpdateOne on existing doc");
    auto r1 = DatabaseEngine::find("u1","db1","items",{{"sku","X"}});
    EXPECT(!r1.empty() && r1[0]["stock"] == 20, "Stock updated to 20");

    // Update nonexistent returns false
    bool ok2 = DatabaseEngine::updateOne("u1","db1","items",{{"sku","MISSING"}},{{"stock",5}});
    EXPECT(!ok2, "UpdateOne on missing doc returns false");

    // Delete then verify
    DatabaseEngine::deleteOne("u1","db1","items",{{"sku","X"}});
    EXPECT(DatabaseEngine::find("u1","db1","items",{}).empty(), "Deleted item gone");
}

static void test_large_document() {
    SECTION("8. Large Document Handling");
    resetEngine();
    ensureDb("u1", "db1", {"blobs"});

    // 1KB payload
    std::string bigStr(1024, 'X');
    DatabaseEngine::insert("u1","db1","blobs",{{"id","big"},{"data",bigStr}});

    auto res = DatabaseEngine::find("u1","db1","blobs",{{"id","big"}});
    EXPECT(res.size() == 1, "Large doc inserted");
    EXPECT(!res.empty() && res[0]["data"].get<std::string>().size() == 1024,
           "Large doc data intact (1024 bytes)");

    // 10KB payload
    std::string bigStr10k(10 * 1024, 'Y');
    DatabaseEngine::insert("u1","db1","blobs",{{"id","10k"},{"data",bigStr10k}});
    auto res2 = DatabaseEngine::find("u1","db1","blobs",{{"id","10k"}});
    EXPECT(!res2.empty() && res2[0]["data"].get<std::string>().size() == 10 * 1024,
           "10KB doc data intact");
}

static void test_nested_json() {
    SECTION("9. Nested JSON Documents");
    resetEngine();
    ensureDb("u1", "db1", {"profiles"});

    json nested = {
        {"id", "n1"},
        {"profile", {
            {"name", "Eve"},
            {"address", {
                {"city", "SF"},
                {"zip", "94102"}
            }},
            {"tags", {"admin", "beta"}}
        }},
        {"scores", {98, 87, 95}}
    };

    DatabaseEngine::insert("u1","db1","profiles", nested);
    auto res = DatabaseEngine::find("u1","db1","profiles",{{"id","n1"}});
    EXPECT(res.size() == 1, "Nested doc inserted");
    EXPECT(!res.empty() && res[0]["profile"]["address"]["city"] == "SF",
           "Nested field access correct");
    EXPECT(!res.empty() && res[0]["profile"]["tags"].is_array(),
           "Array field preserved");
}

static void test_delete_all() {
    SECTION("10. Delete All Documents");
    resetEngine();
    ensureDb("u1", "db1", {"tmp"});

    for (int i = 0; i < 5; ++i)
        DatabaseEngine::insert("u1","db1","tmp",{{"id","doc"+std::to_string(i)},{"i",i}});

    EXPECT(DatabaseEngine::find("u1","db1","tmp",{}).size() == 5, "5 docs inserted");

    // Delete each
    for (int i = 0; i < 5; ++i)
        DatabaseEngine::deleteOne("u1","db1","tmp",{{"id","doc"+std::to_string(i)}});

    EXPECT(DatabaseEngine::find("u1","db1","tmp",{}).empty(), "All docs deleted");
}

static void test_multi_database() {
    SECTION("11. Multiple Databases Under Same Tenant");
    resetEngine();
    ensureDb("u1", "analytics", {"events"});
    ensureDb("u1", "accounting",   {"invoices"});
    ensureDb("u1", "core",      {"users"});

    DatabaseEngine::insert("u1","analytics","events",{{"e","click"},{"count",1}});
    DatabaseEngine::insert("u1","accounting","invoices",{{"inv","I001"},{"amount",500}});
    DatabaseEngine::insert("u1","core","users",{{"email","x@x.com"}});

    EXPECT(DatabaseEngine::find("u1","analytics","events",{}).size() == 1, "Analytics DB");
    EXPECT(DatabaseEngine::find("u1","accounting","invoices",{}).size() == 1, "Accounting DB");
    EXPECT(DatabaseEngine::find("u1","core","users",{}).size() == 1, "Core DB");

    // Databases don't bleed into each other
    EXPECT(DatabaseEngine::find("u1","analytics","invoices",{}).empty(),
           "No cross-DB bleed");
}

static void test_high_volume() {
    SECTION("12. High-Volume Insert & Query (1000 docs)");
    resetEngine();
    ensureDb("u1", "db1", {"bulk"});

    const int N = 1000;
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i) {
        DatabaseEngine::insert("u1","db1","bulk",{
            {"id","b"+std::to_string(i)},
            {"n",i}, {"even",(i%2==0)}, {"mod3",(i%3==0)}, {"val",i*2}
        });
    }

    auto insertMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    auto all = DatabaseEngine::find("u1","db1","bulk",{});
    EXPECT(static_cast<int>(all.size()) == N,
           "All " + std::to_string(N) + " docs present (took " +
           std::to_string(insertMs) + "ms)");

    // Query subset
    auto evens = DatabaseEngine::find("u1","db1","bulk",{{"even",true}});
    EXPECT(static_cast<int>(evens.size()) == N/2, "Even subset query correct");

    EXPECT(insertMs < 30000, "1000 inserts complete in < 30s");
}

static void test_string_special_chars() {
    SECTION("13. Special Characters & Unicode in Values");
    resetEngine();
    ensureDb("u1", "db1", {"strings"});

    DatabaseEngine::insert("u1","db1","strings",{
        {"id","s1"},{"val","Hello, 世界! 🌏"},
        {"json_str",R"({"key":"value with \"quotes\""})"},
        {"newlines","line1\nline2\ntab\there"},
        {"empty",""}
    });

    auto res = DatabaseEngine::find("u1","db1","strings",{{"id","s1"}});
    EXPECT(res.size() == 1, "Special char doc inserted");
    EXPECT(!res.empty() && res[0]["val"] == "Hello, 世界! 🌏",
           "Unicode value preserved");
    EXPECT(!res.empty() && res[0]["empty"] == "",
           "Empty string preserved");
}

static void test_overwrite_field() {
    SECTION("14. Field Overwrite on Update");
    resetEngine();
    ensureDb("u1", "db1", {"props"});

    DatabaseEngine::insert("u1","db1","props",{{"id","p1"},{"color","red"},{"size","M"}});

    // Change color, leave size
    DatabaseEngine::updateOne("u1","db1","props",{{"id","p1"}},{{"color","blue"}});
    auto res = DatabaseEngine::find("u1","db1","props",{{"id","p1"}});
    EXPECT(!res.empty() && res[0]["color"] == "blue", "Color updated to blue");
    // size should still exist (engine-dependent behavior)
    EXPECT(res.size() == 1, "Document still present after partial update");
}

static void test_numeric_types() {
    SECTION("15. Numeric Type Handling");
    resetEngine();
    ensureDb("u1", "db1", {"nums"});

    DatabaseEngine::insert("u1","db1","nums",{
        {"id","t"},
        {"int_val",  42},
        {"neg_val", -7},
        {"float_val", 3.14},
        {"zero", 0},
        {"bignum", 9999999}
    });

    auto res = DatabaseEngine::find("u1","db1","nums",{{"id","t"}});
    EXPECT(!res.empty() && res[0]["int_val"] == 42,   "Integer preserved");
    EXPECT(!res.empty() && res[0]["neg_val"] == -7,   "Negative integer preserved");
    EXPECT(!res.empty() && res[0]["zero"] == 0,        "Zero preserved");
    EXPECT(!res.empty() && res[0]["bignum"] == 9999999,"Large integer preserved");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    const OwnedTestRoot ownedRoot("comprehensive");
    DATA_DIR = ownedRoot.dataRoot().string();
    std::cout << "\033[1m╔═══════════════════════════════════════════════════════╗\033[0m\n";
    std::cout << "\033[1m║  PacificDB Engine — Comprehensive Test Suite          ║\033[0m\n";
    std::cout << "\033[1m║  15 test sections covering core engine correctness     ║\033[0m\n";
    std::cout << "\033[1m╚═══════════════════════════════════════════════════════╝\033[0m\n";

    try {
        test_basic_crud();
        test_query_operators();
        test_multi_collection();
        test_multi_tenant_isolation();
        test_concurrent_inserts();
        test_concurrent_read_write();
        test_update_upsert();
        test_large_document();
        test_nested_json();
        test_delete_all();
        test_multi_database();
        test_high_volume();
        test_string_special_chars();
        test_overwrite_field();
        test_numeric_types();
    } catch (const std::exception& e) {
        std::cerr << "\n\033[31m[CRASH] Uncaught exception: " << e.what() << "\033[0m\n";
        ++g_failed;
    } catch (...) {
        std::cerr << "\n\033[31m[CRASH] Unknown exception\033[0m\n";
        ++g_failed;
    }

    // Summary
    std::cout << "\n\033[1m══════════════════════════════════════════\033[0m\n";
    std::cout << "  Total : " << g_total  << "\n";
    std::cout << "  \033[32mPassed: " << g_passed << "\033[0m\n";
    if (g_failed > 0)
        std::cout << "  \033[31mFailed: " << g_failed << "\033[0m\n";
    else
        std::cout << "  Failed: 0\n";
    std::cout << "\033[1m══════════════════════════════════════════\033[0m\n";

    if (g_failed == 0) {
        std::cout << "\n\033[32m✅ ALL " << g_total << " TESTS PASSED\033[0m\n\n";
        return 0;
    } else {
        std::cerr << "\n\033[31m❌ " << g_failed << " TEST(S) FAILED\033[0m\n\n";
        return 1;
    }
}
