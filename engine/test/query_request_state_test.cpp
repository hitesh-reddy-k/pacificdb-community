#include "query_limiter.hpp"
#include "query_cancellation.hpp"
#include <cassert>
#include <atomic>
#include <future>
#include <thread>

int main() {
    std::atomic<int> arrived{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 16; ++t) workers.emplace_back([&, t] {
        for (int round = 0; round < 300; ++round) {
            QueryLimiter::Scope scope;
            auto current = QueryCancel::makeToken();
            if (t % 4 == 1) current->deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            QueryCancel::Scope cancellation(current);
            const auto reason = std::to_string(t) + ":" + std::to_string(round);
            QueryLimiter::startQuery("same-id");
            QueryLimiter::markTruncated(reason);
            if (round == 0) {
                arrived.fetch_add(1);
                while (arrived.load() != 16) std::this_thread::yield();
            }
            std::this_thread::yield();
            std::string actual;
            assert(QueryLimiter::consumeTruncatedFlag(actual) && actual == reason);
            assert(!QueryLimiter::consumeTruncatedFlag(actual));
            if (t % 2) QueryCancel::getToken()->cancelled.store(true);
            assert(QueryCancel::isCancelled() == bool(t % 2));
            QueryLimiter::checkTimeout("same-id");
            QueryLimiter::endQuery("same-id");
        }
    });
    for (auto& worker : workers) worker.join();
    QueryLimiter::StatePtr delayed;
    auto token = QueryCancel::makeToken();
    std::function<void()> task;
    {
        QueryLimiter::Scope owner;
        QueryCancel::Scope cancellation(token);
        delayed = QueryLimiter::capture();
        task = QueryLimiter::bind([] {
            assert(QueryCancel::isCancelled());
            QueryLimiter::markTruncated("delayed-owner");
        });
    }
    token->cancelled.store(true);
    std::thread detachedOwner(std::move(task));
    detachedOwner.join();
    {
        QueryLimiter::Scope unrelated;
        std::string actual;
        assert(!QueryLimiter::consumeTruncatedFlag(actual));
    }
    {
        QueryLimiter::Scope attach(delayed);
        std::string actual;
        assert(QueryLimiter::consumeTruncatedFlag(actual) && actual == "delayed-owner");
        try {
            QueryLimiter::Scope child;
            QueryLimiter::markTruncated("discarded-error");
            throw std::runtime_error("expected");
        } catch (const std::runtime_error&) {}
        assert(!QueryLimiter::consumeTruncatedFlag(actual));
    }
    std::cout << "REQUEST_STATE_ISOLATION_PASS\n";
}
