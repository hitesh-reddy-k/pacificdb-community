#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_cancellation.hpp"
#include "query_limiter.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <set>
#include <thread>

template<class F> void fails(F function, const std::string& expected) {
    bool failed = false;
    try { function(); } catch (const std::exception& e) { failed = std::string(e.what()).find(expected) != std::string::npos; }
    assert(failed);
}
void ready(const std::string& user = "alice", const std::string& collection = "vectors") {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        const auto state = DatabaseEngine::nativeVectorIndexStatus(user, "app", collection, "native");
        if (state.at("state") == "READY") return;
        if (state.at("state") == "FAILED") throw std::runtime_error(state.dump());
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main() {
    OwnedTestRoot root("native-vector-storage");
    DatabaseEngine::init(root.dataRoot().string());
    struct Stop { ~Stop() { DatabaseEngine::stopNativeVectorIndexes(); } } stop;
    for (const std::string user : {"alice", "bob"}) {
        DatabaseEngine::ensureUserRoot(user); assert(DatabaseEngine::createDatabase(user, "app", "vector"));
        assert(!DatabaseEngine::createCollection(user, "app", "vectors").empty());
    }
    std::mt19937 random(1701); std::uniform_real_distribution<double> value(-1, 1);
    std::vector<json> documents;
    for (int i = 0; i < 1000; ++i) {
        std::vector<double> vector(8); for (auto& number : vector) number = value(random);
        documents.push_back({{"id", std::to_string(i)}, {"vector", vector}, {"group", i%2}});
    }
    DatabaseEngine::insertMany("alice", "app", "vectors", documents);
    DatabaseEngine::insertVector("bob", "app", "vectors", {{"id", "private"}, {"vector", {1.,0.,0.,0.,0.,0.,0.,0.}}});
    const json options{{"name", "native"}, {"dimensions", 8}, {"metric", "cosine"},
        {"ef_search", 200}, {"max_elements", 2000}, {"max_scan_rows", 3000},
        {"max_source_bytes", 4*1024*1024}, {"max_index_bytes", 16*1024*1024}};
    DatabaseEngine::buildNativeVectorIndex("alice", "app", "vectors", options); ready();
    DatabaseEngine::buildNativeVectorIndex("bob", "app", "vectors", options); ready("bob");
    json request{{"vector", {1.,0.,0.,0.,0.,0.,0.,0.}}, {"metric", "cosine"}, {"k", 10}};
    size_t matches = 0;
    for (int q = 0; q < 20; ++q) {
        std::vector<double> query(8); double norm = 0;
        for (auto& number : query) { number = value(random); norm += number*number; }
        request["vector"] = query;
        std::vector<std::pair<double,std::string>> truth;
        for (const auto& document : documents) {
            double dot = 0, length = 0;
            for (int d = 0; d < 8; ++d) {
                const auto number = document.at("vector")[d].get<double>();
                dot += number*query[d]; length += number*number;
            }
            truth.emplace_back(-dot/std::sqrt(length*norm), document.at("id").get<std::string>());
        }
        std::partial_sort(truth.begin(), truth.begin()+10, truth.end());
        json execution;
        const auto hits = DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", request, &execution);
        assert(hits.size() == 10 && execution.at("executed_mode") == "ann");
        for (const auto& hit : hits) {
            assert(hit.at("tenant_id") == "alice");
            for (int k = 0; k < 10; ++k) if (hit.at("id") == truth[k].second) {
                ++matches; assert(std::abs(hit.at("score").get<double>() + truth[k].first) < 1e-12);
            }
        }
    }
    assert(double(matches)/200 >= .95);
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int round = 0; round < 20; ++round)
            assert(DatabaseEngine::updateOne("alice", "app", "vectors", {{"id", "0"}},
                {{"$set", {{"vector", {double(round+1),0.,0.,0.,0.,0.,0.,0.}}}}}));
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) readers.emplace_back([&] {
        QueryLimiter::Scope scope;
        start.store(true);
        for (int round = 0; round < 30; ++round) {
            try {
                const auto hits = DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", request);
                assert(hits.size() == 10);
                for (const auto& hit : hits) assert(hit.at("tenant_id") == "alice");
            } catch (const std::runtime_error& e) {
                const std::string message = e.what();
                assert(message.find("unavailable") != std::string::npos ||
                       message.find("freshness_changed") != std::string::npos ||
                       message.find("visibility_changed") != std::string::npos ||
                       message.find("source_changing") != std::string::npos);
            }
        }
    });
    writer.join(); for (auto& reader : readers) reader.join();
    DatabaseEngine::rebuildNativeVectorIndex("alice", "app", "vectors", "native"); ready();
    auto filtered = request; filtered["filter"] = {{"group", 1}};
    fails([&] { DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", filtered); }, "requires_exact");
    auto mismatch = request; mismatch["metric"] = "dot";
    fails([&] { DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", mismatch); }, "mismatch");
    DatabaseEngine::setReadContext("strong", 0, 100);
    fails([&] { DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", request); }, "requires_exact");
    DatabaseEngine::clearReadContext();
    {
        auto token = QueryCancel::makeToken(); token->cancelled.store(true); QueryCancel::Scope cancelled(token);
        fails([&] { DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", request); }, "cancelled");
    }
    const auto snapshot = LSM::exportSnapshotJson(1);
    assert(DatabaseEngine::updateOne("alice", "app", "vectors", {{"id", "0"}},
        {{"$set", {{"vector", {1.,0.,0.,0.,0.,0.,0.,0.}}}}}));
    assert(DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native").at("state") == "STALE");
    fails([&] { DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", request); }, "unavailable");
    DatabaseEngine::rebuildNativeVectorIndex("alice", "app", "vectors", "native"); ready();
    assert(DatabaseEngine::deleteOne("alice", "app", "vectors", {{"id", "1"}}));
    assert(DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native").at("state") == "STALE");
    DatabaseEngine::rebuildNativeVectorIndex("alice", "app", "vectors", "native"); ready();
    assert(LSM::applySnapshot(snapshot));
    assert(DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native").at("state") == "STALE");
    DatabaseEngine::rebuildNativeVectorIndex("alice", "app", "vectors", "native"); ready();
    DatabaseEngine::init(root.dataRoot().string());
    assert(DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native").at("state") == "ABSENT");
    DatabaseEngine::rebuildNativeVectorIndex("alice", "app", "vectors", "native"); ready();
    assert(DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", request).size() == 10);
    assert(DatabaseEngine::dropNativeVectorIndex("alice", "app", "vectors", "native"));
    assert(DatabaseEngine::count("alice", "app", "vectors") == 1000);
    fails([&] { DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native"); }, "ABSENT");
    DatabaseEngine::createCollection("alice", "app", "bad");
    DatabaseEngine::insertVector("alice", "app", "bad", {{"id", "sparse"}, {"vector", {
        {"format", "sparse-v1"}, {"precision", "float64"}, {"dimensions", 8}, {"indices", {0}}, {"values", {1.}}}}});
    DatabaseEngine::buildNativeVectorIndex("alice", "app", "bad", options);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while (DatabaseEngine::nativeVectorIndexStatus("alice", "app", "bad", "native").at("state") != "FAILED") {
        assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
    fails([&] { DatabaseEngine::queryNativeVectorIndex("alice", "app", "bad", "native", request); }, "unavailable");
    assert(DatabaseEngine::queryVector("alice", "app", "bad", request).size() == 1);
    std::cout << "NATIVE_VECTOR_STORAGE_PASS recall=" << double(matches)/200 << '\n';
}
