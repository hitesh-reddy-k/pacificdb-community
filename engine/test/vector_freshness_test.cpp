#include "vector_freshness.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    pacificdb::VectorFreshness tracker;
    const auto initial = tracker.capture("alice/db/vectors", 5); assert(initial);
    {
        auto mutation = tracker.collectionMutation("alice/db/vectors");
        assert(!tracker.capture("alice/db/vectors", 5));
        // No global serialization: a distinct slot is still eligible.
        assert(tracker.capture("bob/db/vectors", 5));
    }
    auto updated = tracker.capture("alice/db/vectors", 5);
    assert(updated && *updated != *initial);
    assert(tracker.capture("alice/db/vectors", 6) != updated);
    {
        auto root = tracker.rootMutation();
        assert(!tracker.capture("alice/db/vectors", 5));
        { auto nested = tracker.rootMutation(); assert(!tracker.capture("bob/db/vectors", 5)); }
        assert(!tracker.capture("bob/db/vectors", 5));
    }
    assert(tracker.capture("alice/db/vectors", 5)->root != updated->root);
    // Shared slots must yield conservative invalidation, never false freshness.
    const std::string first = "collection-0";
    std::string collision;
    for (size_t n = 1; collision.empty(); ++n) {
        auto candidate = "collection-" + std::to_string(n);
        if (std::hash<std::string>{}(first) % 4096 ==
            std::hash<std::string>{}(candidate) % 4096) collision = candidate;
    }
    const auto beforeCollision = tracker.capture(first, 0);
    { auto mutation = tracker.collectionMutation(collision); assert(!tracker.capture(first, 0)); }
    assert(tracker.capture(first, 0) != beforeCollision);

    pacificdb::VectorFreshness concurrent;
    std::atomic<uint64_t> value{0}; std::atomic<bool> start{false}, done{false};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int n = 0; n < 20000; ++n) {
            auto mutation = concurrent.collectionMutation("hot");
            value.fetch_add(1);
        }
        done.store(true);
    });
    std::vector<std::thread> readers;
    for (int n = 0; n < 4; ++n) readers.emplace_back([&] {
        start.store(true);
        do {
            const auto before = concurrent.capture("hot", 7);
            const auto record = value.load();
            const auto after = concurrent.capture("hot", 7);
            if (before && before == after) assert(before->collection == 2 * record);
        } while (!done.load());
    });
    writer.join(); for (auto& reader : readers) reader.join();
    assert(concurrent.capture("hot", 7)->collection == 40000);
    std::cout << "VECTOR_FRESHNESS_PASS\n";
}
