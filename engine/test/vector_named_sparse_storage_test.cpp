#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>

static json sparse(const json& indices, const json& values) {
    return {{"format", "sparse-v1"}, {"precision", "float64"}, {"dimensions", 4},
            {"indices", indices}, {"values", values}};
}
int main() {
    OwnedTestRoot root("named-sparse-storage");
    DatabaseEngine::init(root.dataRoot().string());
    for (const std::string user : {"alice", "bob"}) {
        DatabaseEngine::ensureUserRoot(user);
        assert(DatabaseEngine::createDatabase(user, "app", "vector"));
        assert(!DatabaseEngine::createCollection(user, "app", "vectors").empty());
    }
    QueryLimiter::setMaxScanRows(10000);
    const json a{{"id", "a"}, {"vector", {1., 0., 0., 0.}}, {"category", "news"},
        {"vectors", {{"dense", {1., 0., 0., 0.}}, {"terms", sparse({0, 3}, {1., 2.})}}}};
    const json b{{"id", "b"}, {"category", "news"},
        {"vectors", {{"dense", {0., 1., 0., 0.}}, {"terms", sparse({1}, {3.})}}}};
    DatabaseEngine::insertVector("alice", "app", "vectors", a);
    DatabaseEngine::insertVector("alice", "app", "vectors", b);
    DatabaseEngine::insertVector("alice", "app", "vectors", {{"id", "default-sparse"},
        {"vector", sparse({3}, {4.})}, {"category", "other"}});
    DatabaseEngine::insertVector("bob", "app", "vectors", {{"id", "private"},
        {"vectors", {{"terms", sparse({3}, {100.})}}}});
    auto request = json{{"vector", sparse({3}, {1.})}, {"vector_name", "terms"},
                        {"metric", "dot"}, {"k", 10}, {"search_mode", "exact"}};
    const auto query = [&](const json& q) { return DatabaseEngine::queryVector("alice", "app", "vectors", q); };
    auto rows = query(request); assert(rows.size() == 2);
    assert(rows[0].at("id") == "a" && rows[0].at("score") == 2.);
    assert(rows[1].at("id") == "b" && rows[1].at("score") == 0.);
    assert(rows[0].at("vectors") == a.at("vectors"));
    auto filter = request; filter["filter"] = {{"category", "other"}}; assert(query(filter).empty());
    auto dense = request; dense["vector_name"] = "dense"; dense["vector"] = {0., 1., 0., 0.};
    assert(query(dense).front().at("id") == "b");
    auto defaultSpace = request; defaultSpace.erase("vector_name");
    assert(query(defaultSpace).front().at("id") == "default-sparse");
    auto missing = request; missing["vector_name"] = "missing"; assert(query(missing).empty());
    for (const std::string metric : {"dot", "cosine", "l2"}) {
        auto metricQuery = request; metricQuery["metric"] = metric;
        const auto memoryRows = query(metricQuery);
        LSM::flush("alice", "app", "vectors");
        assert(query(metricQuery) == memoryRows);
    }
    const auto snapshot = LSM::exportSnapshotJson(1);
    const auto baseline = query(request);
    auto updated = a.at("vectors"); updated["terms"] = sparse({3}, {-5.});
    assert(DatabaseEngine::updateOne("alice", "app", "vectors", {{"id", "a"}},
        {{"$set", {{"vectors", updated}}}}));
    assert(query(request).front().at("id") == "b");
    assert(DatabaseEngine::deleteOne("alice", "app", "vectors", {{"id", "b"}}));
    assert(query(request).size() == 1);
    assert(LSM::applySnapshot(snapshot)); assert(query(request) == baseline);
    LSM::restoreFromWal(); assert(query(request) == baseline);
    DatabaseEngine::setReadContext("strong", 0, std::numeric_limits<long long>::max());
    assert(query(request).empty()); DatabaseEngine::clearReadContext();
    assert(query(request) == baseline);
    bool rejected = false;
    try { DatabaseEngine::insertVector("alice", "app", "vectors", {{"id", "invalid"},
        {"vector", sparse({1, 1}, {1., 2.})}}); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected && DatabaseEngine::find("alice", "app", "vectors", {{"id", "invalid"}}).empty());
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int round = 0; round < 80; ++round) {
            auto changed = a.at("vectors"); changed["terms"] = sparse({3}, {double(round % 2 ? -5 : 2)});
            assert(DatabaseEngine::updateOne("alice", "app", "vectors", {{"id", "a"}},
                {{"$set", {{"vectors", changed}}}}));
        }
    });
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) readers.emplace_back([&] {
        start.store(true);
        for (int round = 0; round < 100; ++round) {
            QueryLimiter::Scope scope;
            const auto found = query(request); assert(found.size() == 2);
            for (const auto& row : found) {
                assert(row.at("tenant_id") == "alice");
                if (row.at("id") == "a") {
                    const auto& terms = row.at("vectors").at("terms");
                    assert(row.at("score") == terms.at("values").back());
                } else assert(row.at("id") == "b" && row.at("score") == 0.);
            }
        }
    });
    writer.join(); for (auto& reader : readers) reader.join();
    std::cout << "NAMED_SPARSE_STORAGE_PASS\n";
}
