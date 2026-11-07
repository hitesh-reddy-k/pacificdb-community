#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_cancellation.hpp"
#include "query_limiter.hpp"
#include "vector_top_k.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <thread>

using json = nlohmann::json;

template<class F> void rejects(F&& f, const std::string& message) {
    bool rejected = false;
    try { f(); } catch (const std::exception& e) {
        rejected = std::string(e.what()).find(message) != std::string::npos;
    }
    assert(rejected);
}

// Independently calculates a complete ordering (no production distance/top-K code).
std::vector<json> reference(const std::vector<json>& docs, const std::string& metric) {
    std::vector<json> expected;
    for (auto doc : docs) {
        const auto& v = doc.at("vector");
        const long double x = v[0].get<long double>();
        const long double y = v[1].get<long double>();
        long double score;
        if (metric == "dot") score = x * 0.25L + y * 0.75L;
        else if (metric == "l2") score = -std::sqrt((x - 0.25L)*(x - 0.25L) + (y - 0.75L)*(y - 0.75L));
        else {
            const auto norm = std::sqrt(x*x + y*y) * std::sqrt(0.25L*0.25L + 0.75L*0.75L);
            score = norm < 1e-12L ? 0 : (x*0.25L + y*0.75L) / norm;
        }
        doc["score"] = static_cast<double>(score);
        expected.push_back(std::move(doc));
    }
    std::sort(expected.begin(), expected.end(), [](const json& a, const json& b) {
        return a.at("score") > b.at("score") ||
            (a.at("score") == b.at("score") && a.at("id") < b.at("id"));
    });
    expected.resize(3);
    return expected;
}

int main() {
    // Verify retained bound and lazy materialization independently of storage.
    pacificdb::vector_search::TopK heap(3, 1024);
    std::size_t materializations = 0;
    for (int i = 10000; i >= 0; --i) {
        heap.consider(i, std::to_string(i), [&] {
            ++materializations;
            return json{{"id", std::to_string(i)}};
        });
        assert(heap.size() <= 3);
    }
    assert(materializations == 3);
    assert(heap.finish().front().at("id") == "10000");
    pacificdb::vector_search::TopK tiny(1, 1);
    rejects([&] { tiny.consider(1, "x", [] { return json{{"id", "x"}}; }); }, "bytes_exceeded");

    const OwnedTestRoot root("vector-exact-v12");
    DatabaseEngine::init(root.dataRoot().string());
    const std::string user = "exact_owner", db = "vectors", collection = "records";
    DatabaseEngine::ensureUserRoot(user);
    DatabaseEngine::createDatabase(user, db, "vector");
    DatabaseEngine::createCollection(user, db, collection);
    std::vector<json> docs;
    std::mt19937 random(20261029);
    std::uniform_real_distribution<double> value(-1, 1);
    for (int i = 0; i < 350; ++i) {
        docs.push_back({{"id", "v" + std::to_string(i)}, {"vector", {value(random), value(random)}},
                        {"group", i % 2}, {"version", i + 1}, {"payload", "unchanged"}});
    }
    assert(DatabaseEngine::insertMany(user, db, collection, docs).at("inserted") == docs.size());
    const auto request = [](const std::string& metric) {
        return json{{"vector", {0.25, 0.75}}, {"metric", metric}, {"k", 3}, {"search_mode", "exact"}};
    };
    QueryLimiter::setMaxScanRows(10000);
    QueryLimiter::setMaxResultDocs(3); // limit the output, never the candidates
    for (const bool flushed : {false, true}) {
        if (flushed) LSM::flush(user, db, collection);
        for (const std::string metric : {"cosine", "l2", "dot"}) {
            const auto expected = reference(docs, metric);
            const auto actual = DatabaseEngine::queryVector(user, db, collection, request(metric));
            assert(actual.size() == 3);
            for (std::size_t i = 0; i < actual.size(); ++i) {
                assert(actual[i].at("id") == expected[i].at("id"));
                assert(std::abs(actual[i].at("score").get<double>() - expected[i].at("score").get<double>()) < 1e-12);
                assert(actual[i].at("payload") == "unchanged");
            }
        }
    }
    // All eligible rows after applying the shared predicate, not first 3 scanned.
    auto filtered = request("dot");
    filtered["filter"] = {{"$and", json::array({json{{"group", 1}}, json{{"version", {{"$gt", 100}}}}})}};
    auto selected = docs;
    selected.erase(std::remove_if(selected.begin(), selected.end(), [](const json& d) {
        return d.at("group") != 1 || d.at("version").get<int>() <= 100;
    }), selected.end());
    const auto filteredRows = DatabaseEngine::queryVector(user, db, collection, filtered);
    const auto expectedFiltered = reference(selected, "dot");
    for (std::size_t i = 0; i < 3; ++i) assert(filteredRows[i].at("id") == expectedFiltered[i].at("id"));

    // Inject a misplaced foreign row below the API to exercise defense in depth.
    LSM::put(user, db, collection, {{"id", "foreign"}, {"tenant_id", "another_owner"}, {"vector", {999., 999.}}});
    for (const auto& row : DatabaseEngine::queryVector(user, db, collection, request("dot"))) assert(row.at("id") != "foreign");

    assert(DatabaseEngine::updateOne(user, db, collection, {{"id", "v0"}}, {{"$set", {{"vector", {100., 100.}}}}}));
    auto updated = DatabaseEngine::queryVector(user, db, collection, request("dot"));
    assert(updated.front().at("id") == "v0");
    assert(DatabaseEngine::deleteOne(user, db, collection, {{"id", "v0"}}));
    for (const bool flushed : {false, true}) {
        if (flushed) LSM::flush(user, db, collection);
        for (const auto& row : DatabaseEngine::queryVector(user, db, collection, request("dot"))) assert(row.at("id") != "v0");
    }
    // Explicit per-key/version floor matches document read behavior.
    DatabaseEngine::setReadContext("strong", 0, 1000);
    assert(DatabaseEngine::queryVector(user, db, collection, request("dot")).empty());
    DatabaseEngine::clearReadContext();
    auto zeroQuery = request("cosine");
    zeroQuery["vector"] = {0., 0.};
    const auto zeroScores = DatabaseEngine::queryVector(user, db, collection, zeroQuery);
    assert(zeroScores[0].at("id") == "v1");
    assert(zeroScores[1].at("id") == "v10");
    assert(zeroScores[2].at("id") == "v100");
    for (const auto& row : zeroScores) assert(row.at("score") == 0.0);

    // A returned payload and score must describe the same pinned row even when
    // another thread overwrites it. This does not assert historical snapshots.
    std::thread writer([&] {
        for (int i = 0; i < 30; ++i) {
            assert(DatabaseEngine::updateOne(user, db, collection, {{"id", "v1"}},
                {{"$set", {{"vector", {10.0 + i, 20.0 + i}}}}}));
        }
    });
    for (int i = 0; i < 30; ++i) {
        const auto rows = DatabaseEngine::queryVector(user, db, collection, request("dot"));
        assert(rows.size() == 3);
        for (const auto& row : rows) {
            const auto& vector = row.at("vector");
            const double score = vector[0].get<double>() * .25 + vector[1].get<double>() * .75;
            assert(std::abs(score - row.at("score").get<double>()) < 1e-12);
        }
    }
    writer.join();
    LSM::flush(user, db, collection);
    LSM::compact(user, db, collection);
    assert(DatabaseEngine::queryVector(user, db, collection, request("dot")).front().at("id") == "v1");

    // Recovery can replay an older version into memory while a newer value or
    // tombstone is already durable in an SST. Layer order must not defeat MVCC.
    const std::string replayCollection = "replay_versions";
    DatabaseEngine::createCollection(user, db, replayCollection);
    LSM::put(user, db, replayCollection, {{"id", "live"}, {"_mvcc_version", 20}, {"vector", {1., 1.}}});
    LSM::put(user, db, replayCollection, {{"id", "gone"}, {"_mvcc_version", 20}, {"_deleted", true}});
    LSM::flush(user, db, replayCollection);
    for (const std::string id : {"live", "gone"})
        LSM::put(user, db, replayCollection, {{"id", id}, {"_mvcc_version", 10}, {"vector", {999., 999.}}});
    const auto replayRows = DatabaseEngine::queryVector(user, db, replayCollection, request("dot"));
    assert(replayRows.size() == 1);
    assert(replayRows.front().at("id") == "live");
    assert(replayRows.front().at("_mvcc_version") == 20);
    assert(replayRows.front().at("score") == 1.);

    QueryLimiter::setMaxScanRows(1);
    rejects([&] { DatabaseEngine::queryVector(user, db, collection, request("dot")); }, "scan_limit_exceeded");
    QueryLimiter::setMaxScanRows(10000);
    auto token = QueryCancel::makeToken();
    token->cancelled.store(true);
    QueryCancel::setToken(token);
    rejects([&] { DatabaseEngine::queryVector(user, db, collection, request("dot")); }, "cancelled");
    QueryCancel::clearToken();
    auto invalid = request("dot");
    invalid["k"] = 4;
    rejects([&] { DatabaseEngine::queryVector(user, db, collection, invalid); }, "MAX_RESULT_DOCS");
    invalid = request("dot");
    invalid["search_mode"] = "invalid";
    rejects([&] { DatabaseEngine::queryVector(user, db, collection, invalid); }, "search_mode");
    invalid = request("dot");
    invalid["k"] = uint64_t{4294967297};
    rejects([&] { DatabaseEngine::queryVector(user, db, collection, invalid); }, "supported range");

    rejects([&] { DatabaseEngine::queryVector(user, db, collection, request("dot"), nullptr,
        std::chrono::steady_clock::now() - std::chrono::seconds(1)); }, "timeout");
    assert(DatabaseEngine::queryVector(user, db, collection, request("dot")).size() == 3);
    // Stable tie ordering regardless of arrival order, all supported scores.
    pacificdb::vector_search::TopK ties(2, 1024);
    for (const std::string id : {"z", "b", "a"}) ties.consider(1, id, [&] { return json{{"id", id}}; });
    const auto tied = ties.finish();
    assert(tied[0].at("id") == "a" && tied[1].at("id") == "b");
    // A finite norm must not silently become zero similarity after float64
    // intermediate overflow; verify memory and packed SST execution paths.
    DatabaseEngine::createCollection(user,db,"numeric_range");
    DatabaseEngine::insertVector(user,db,"numeric_range",{{"id","huge"},{"vector",{1e300,1e300}}});
    for(int stored=0;stored<2;++stored){
        const auto huge=DatabaseEngine::queryVector(user,db,"numeric_range",{{"vector",{1.,0.}},{"metric","cosine"},{"k",1},{"search_mode","exact"}});
        assert(huge.size()==1&&std::abs(huge.front().at("score").get<double>()-std::sqrt(.5))<1e-14);
        LSM::flush(user,db,"numeric_range");
    }
    // MAX_QUERY_TIMEOUT_SEC applies even when the worker has no cancellation token.
#ifdef _WIN32
    _putenv_s("MAX_QUERY_TIMEOUT_SEC", "0");
#else
    setenv("MAX_QUERY_TIMEOUT_SEC", "0", 1);
#endif
    QueryLimiter::init();
    rejects([&] { DatabaseEngine::queryVector(user, db, collection, request("dot")); }, "vector_query_timeout");
    std::cout << "VECTOR_EXACT_V12_PASS\n";
}
