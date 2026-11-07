#include "vector_index_generation.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

using namespace pacificdb;
static void wait(VectorIndexGeneration& manager, VectorIndexGeneration::State expected) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (manager.status().state != expected) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main() {
    auto pool = std::make_shared<VectorAllocationBudget>(128 * 1024);
    {
        HNSWConfig config; config.dimensions = 128;
        HNSWIndex first(config, pool);
        for (int n = 0; n < 40; ++n)
            assert(first.insert(std::string(80, 'a') + std::to_string(n), std::vector<float>(128, float(n))));
        const auto baseline = pool->used(); assert(baseline > 0);
        auto second = std::make_unique<HNSWIndex>(config, pool);
        bool exhausted = false;
        try {
            for (int n = 0; n < 2000; ++n)
                second->insert(std::to_string(n), std::vector<float>(128, 1));
        } catch (const VectorMemoryLimit&) { exhausted = true; }
        assert(exhausted && !second->getStats().at("healthy").get<bool>());
        bool unhealthy = false;
        try { second->search(std::vector<float>(128, 1), 1); }
        catch (const std::runtime_error&) { unhealthy = true; }
        assert(unhealthy);
        second.reset(); assert(pool->used() == baseline);
        // Search scratch is admitted too; a failed read must not poison the graph.
        const size_t held = pool->limit() - pool->used(); assert(pool->reserve(held));
        exhausted = false;
        try { first.search(std::vector<float>(128, 1), 5); }
        catch (const VectorMemoryLimit&) { exhausted = true; }
        assert(exhausted && first.getStats().at("healthy").get<bool>());
        pool->release(held);
        assert(first.search(std::vector<float>(128, 1), 5).size() == 5);
        assert(pool->used() == baseline && pool->peak() <= pool->limit());
    }
    assert(pool->used() == 0);
    {
        HNSWConfig tiny; tiny.dimensions = 128; tiny.maxMemoryBytes = 1024;
        HNSWIndex index(tiny, pool);
        bool exhausted = false;
        try {
            for (int n = 0; n < 100; ++n) index.insert(std::to_string(n), std::vector<float>(128, 1));
        } catch (const VectorMemoryLimit&) { exhausted = true; }
        assert(exhausted && index.getStats().at("peakAllocatedBytes").get<size_t>() <= 1024);
    }
    assert(pool->used() == 0);
    HNSWConfig config; config.dimensions = 2;
    const auto certificate = []() -> std::optional<VectorFreshness::Stamp> { return VectorFreshness::Stamp{1}; };
    const auto source = [](const VectorIndexGeneration::Emit& emit, const std::atomic<bool>&) {
        for (int n = 0; n < 10; ++n) emit({std::to_string(n), {float(n), 1}});
    };
    std::shared_ptr<const VectorIndexGeneration::Generation> retained;
    {
        VectorIndexGeneration manager(10000, pool);
        assert(manager.enqueue(config, certificate, source));
        wait(manager, VectorIndexGeneration::State::READY);
        retained = manager.acquire(VectorFreshness::Stamp{1}); assert(retained);
    }
    const size_t retainedBytes = pool->used(); assert(retainedBytes > 0);
    {
        VectorIndexGeneration manager(10000, pool);
        const auto held = pool->limit() - retainedBytes; assert(pool->reserve(held));
        assert(manager.enqueue(config, certificate, source));
        wait(manager, VectorIndexGeneration::State::FAILED);
        assert(manager.status().error == "vector_index_memory_budget_exceeded");
        assert(!manager.acquire(VectorFreshness::Stamp{1}));
        pool->release(held);
        assert(manager.enqueue(config, certificate, source));
        wait(manager, VectorIndexGeneration::State::READY);
        const size_t withOld = pool->used(); assert(withOld > retainedBytes);
        retained.reset(); assert(pool->used() == withOld - retainedBytes);
    }
    assert(pool->used() == 0 && pool->peak() <= pool->limit());
    std::cout << "VECTOR_MEMORY_BUDGET_PASS\n";
}
