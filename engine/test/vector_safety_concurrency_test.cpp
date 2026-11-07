#include "database_engine.hpp"
#include "db_task_queue_sharded.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include "query_cancellation.hpp"
#include <cassert>
#include <atomic>
#include <future>
#include <thread>

using json = nlohmann::json;
template<class F> void fails(F&& f, const std::string& message) {
    bool failed = false;
    try { f(); } catch (const std::exception& e) { failed = std::string(e.what()).find(message) != std::string::npos; }
    assert(failed);
}
int main() {
    OwnedTestRoot root("vector-safety-concurrency");
    DatabaseEngine::init(root.dataRoot().string());
    for (const std::string tenant : {"alice", "bob"}) {
        DatabaseEngine::ensureUserRoot(tenant);
        DatabaseEngine::createDatabase(tenant, "app", "vector");
        for (const std::string collection : {"short", "long"}) {
            DatabaseEngine::createCollection(tenant, "app", collection);
            std::vector<json> docs;
            const int n = collection == "short" ? 5 : 2500;
            for (int i = 0; i < n; ++i) docs.push_back({{"id", std::to_string(i)},
                {"vector", {double(i), 1.}}, {"value", i}, {"version", 1}});
            DatabaseEngine::insertMany(tenant, "app", collection, std::move(docs));
            LSM::flush(tenant, "app", collection);
        }
    }
    QueryLimiter::setMaxScanRows(50);
    QueryLimiter::setMaxResultDocs(10);
    json q{{"vector", {1., 0.}}, {"k", 3}, {"metric", "dot"}};
    json detail;
    auto reference = DatabaseEngine::queryVector("alice", "app", "short", q, &detail);
    assert(detail.at("executed_mode") == "exact" && detail.at("index_state") == "ABSENT");
    auto ann = q; ann["search_mode"] = "ann";
    fails([&] { DatabaseEngine::queryVector("alice", "app", "short", ann); }, "vector_index_unavailable");
    ann["allow_exact_fallback"] = true;
    assert(DatabaseEngine::queryVector("alice", "app", "short", ann) == reference);
    auto strictAuto = q; strictAuto["allow_exact_fallback"] = false;
    fails([&] { DatabaseEngine::queryVector("alice", "app", "short", strictAuto); }, "vector_index_unavailable");
    strictAuto["search_mode"] = "exact";
    assert(DatabaseEngine::queryVector("alice", "app", "short", strictAuto) == reference);
    std::vector<std::thread> threads;
    for (int t = 0; t < 12; ++t) threads.emplace_back([&, t] {
        const std::string tenant = t % 2 ? "alice" : "bob";
        for (int round = 0; round < 40; ++round) {
            QueryLimiter::Scope scope;
            QueryCancel::Scope cancel(QueryCancel::makeToken());
            std::string reason;
            if (t % 3 == 0) {
                auto docs = DatabaseEngine::find(tenant, "app", "long", {{"value", {{"$gt", 99999}}}});
                assert(docs.empty());
                assert(QueryLimiter::consumeTruncatedFlag(reason) && reason == "scan_limit");
            } else if (t % 3 == 1) {
                auto rows = DatabaseEngine::queryVector(tenant, "app", "short", q);
                assert(rows.size() == 3 && rows.front().at("id") == "4");
                for (const auto& row : rows) assert(row.at("tenant_id") == tenant);
                assert(!QueryLimiter::consumeTruncatedFlag(reason));
            } else {
                fails([&] { DatabaseEngine::queryVector(tenant, "app", "long", q); }, "vector_scan_limit_exceeded");
                assert(!QueryLimiter::consumeTruncatedFlag(reason));
            }
        }
    });
    for (auto& thread : threads) thread.join();
    // Observe actual iterator progress before cancellation, not a timer guess.
    QueryLimiter::setMaxScanRows(10000);
    QueryLimiter::StatePtr state = std::make_shared<QueryLimiter::RequestState>();
    auto token = QueryCancel::makeToken();
    auto running = std::async(std::launch::async, [&] {
        QueryLimiter::Scope attach(state);
        QueryCancel::Scope cancel(token);
        fails([&] { DatabaseEngine::queryVector("alice", "app", "long", q); }, "vector_query_cancelled");
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (state->scannedRows.load() < 5 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(state->scannedRows.load() >= 5);
    token->cancelled.store(true);
    assert(DatabaseEngine::queryVector("bob", "app", "short", q).size() == 3);
    running.get();
    {
        QueryLimiter::Scope cancelledRead;
        auto cancelled = QueryCancel::makeToken();
        cancelled->cancelled.store(true);
        QueryCancel::Scope cancel(cancelled);
        assert(DatabaseEngine::find("alice", "app", "long", {{"value", {{"$gt", 99999}}}}).empty());
        std::string reason;
        assert(QueryLimiter::consumeTruncatedFlag(reason) && reason == "cancelled");
    }
    DBTaskQueueSharded queue(1, 1, 32);
    {
        QueryLimiter::Scope owner;
        QueryCancel::Scope cancel(QueryCancel::makeToken());
        QueryLimiter::markTruncated("owning-request");
        std::promise<void> done;
        auto result = done.get_future();
        assert(queue.enqueue([&] {
            std::string reason;
            assert(QueryLimiter::consumeTruncatedFlag(reason) && reason == "owning-request");
            assert(!QueryCancel::isCancelled());
            QueryLimiter::markTruncated("worker-result");
            QueryCancel::getToken()->cancelled.store(true);
            DatabaseEngine::setReadContext("strong", 0, 100);
            done.set_value();
            throw std::runtime_error("expected worker exception for reuse test");
        }));
        result.get();
        std::string reason;
        assert(QueryLimiter::consumeTruncatedFlag(reason) && reason == "worker-result");
    }
    {
        QueryLimiter::Scope next;
        std::promise<void> done;
        auto result = done.get_future();
        assert(queue.enqueue([&] {
            std::string reason;
            assert(!QueryLimiter::consumeTruncatedFlag(reason));
            assert(!QueryCancel::isCancelled());
            assert(DatabaseEngine::queryVector("bob", "app", "short", q).size() == 3);
            done.set_value();
        }));
        result.get();
    }
    std::cout << "VECTOR_SAFETY_CONCURRENCY_PASS\n";
}
