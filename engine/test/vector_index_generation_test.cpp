#include "vector_index_generation.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

using Manager = pacificdb::VectorIndexGeneration;
using State = Manager::State;

void wait(Manager& manager, State expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (manager.status().state != expected) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main() {
    std::atomic<uint64_t> certificate{1};
    Manager manager(100000);
    pacificdb::HNSWConfig config; config.dimensions = 2;
    auto version = [&]() -> std::optional<pacificdb::VectorFreshness::Stamp> {
        return pacificdb::VectorFreshness::Stamp{certificate.load()};
    };
    auto source = [](const Manager::Emit& emit, const std::atomic<bool>&) {
        emit({"a", {1, 0}}); emit({"b", {0, 1}});
    };
    assert(manager.status().state == State::ABSENT);
    assert(manager.enqueue(config, version, source)); wait(manager, State::READY);
    auto old = manager.acquire(1); assert(old && old->number == 1);
    assert(old->graph->search({1, 0}, 1).front().first == "a");
    assert(!manager.acquire(2));
    certificate.store(2); manager.invalidate(); assert(!manager.acquire(1));
    // A query's retained generation remains alive across invalidation/publication.
    assert(old->graph->size() == 2);
    assert(manager.enqueue(config, version, source)); wait(manager, State::READY);
    assert(manager.acquire(2)->number == 2);
    std::atomic<bool> entered{false}, release{false};
    assert(manager.enqueue(config, version, [&](const Manager::Emit& emit,
                                               const std::atomic<bool>& stop) {
        entered.store(true);
        while (!release.load() && !stop.load()) std::this_thread::yield();
        emit({"c", {1, 1}});
    }));
    while (!entered.load()) std::this_thread::yield();
    manager.invalidate();
    assert(!manager.enqueue(config, version, source));
    certificate.store(3); release.store(true); wait(manager, State::STALE);
    // Wait for the invalidated worker to release its build slot.
    while (!manager.enqueue(config, version, source)) std::this_thread::yield();
    wait(manager, State::READY); assert(manager.acquire(3)->number == 3);
    Manager tiny(1);
    assert(tiny.enqueue(config, version, source)); wait(tiny, State::FAILED);
    assert(!tiny.status().error.empty() && !tiny.acquire(3));
    assert(manager.enqueue(config, version, [](const Manager::Emit& emit,
                                               const std::atomic<bool>&) {
        emit({"duplicate", {1, 0}}); emit({"duplicate", {0, 1}});
    }));
    wait(manager, State::FAILED); assert(!manager.acquire(3));
    assert(old->graph->size() == 2);
    // Detect source mutation even if an external invalidation notification is lost.
    assert(manager.enqueue(config, version, [&](const Manager::Emit& emit,
                                                const std::atomic<bool>&) {
        emit({"changed", {1, 1}}); certificate.fetch_add(1);
    }));
    wait(manager, State::STALE); assert(!manager.acquire(certificate.load()));
    // Certificate/source failures are visible and do not kill the worker.
    assert(manager.enqueue(config, []() -> std::optional<pacificdb::VectorFreshness::Stamp> {
        throw std::runtime_error("certificate unavailable");
    }, source));
    wait(manager, State::FAILED);
    assert(manager.status().error == "certificate unavailable");
    std::atomic<bool> finishReaders{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) readers.emplace_back([&] {
        while (!finishReaders.load()) {
            auto generation = manager.acquire(certificate.load());
            if (generation) assert(generation->graph->search({1, 0}, 1).front().first == "a");
        }
    });
    for (int i = 0; i < 20; ++i) {
        manager.invalidate(); certificate.fetch_add(1);
        assert(manager.enqueue(config, version, source)); wait(manager, State::READY);
        assert(manager.acquire(certificate.load()));
    }
    finishReaders.store(true);
    for (auto& reader : readers) reader.join();
    // Destruction cancels an active cooperative producer and joins its worker.
    std::atomic<bool> running{false};
    {
        Manager shutdown(10000);
        assert(shutdown.enqueue(config, version, [&](const Manager::Emit&,
                                                      const std::atomic<bool>& stop) {
            running.store(true);
            while (!stop.load()) std::this_thread::yield();
        }));
        while (!running.load()) std::this_thread::yield();
    }
    std::cout << "VECTOR_GENERATION_LIFECYCLE_PASS (experimental primitive only)\n";
}
