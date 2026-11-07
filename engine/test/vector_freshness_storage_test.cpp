#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "vector_index_generation.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

using Manager = pacificdb::VectorIndexGeneration;
static void ready(Manager& manager) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (manager.status().state != Manager::State::READY) {
        assert(manager.status().state != Manager::State::FAILED);
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main() {
    OwnedTestRoot root("vector-freshness-storage");
    LSM::init(root.dataRoot().string());
    const std::string user = "alice", db = "app", collection = "vectors";
    auto certificate = [&] { return LSM::vectorCertificate(user, db, collection); };
    const auto initial = certificate(); assert(initial);
    LSM::put(user, db, collection, {{"id", "a"}, {"vector", {1., 0.}}});
    assert(certificate() && certificate() != initial);
    auto previous = certificate();
    LSM::putMany(user, db, collection, {{{"id", "b"}, {"vector", {0., 1.}}}});
    assert(certificate() != previous);
    LSM::flush(user, db, collection);
    const auto snapshot = LSM::exportSnapshotJson(1);
    Manager manager(1024 * 1024);
    pacificdb::HNSWConfig config; config.dimensions = 2;
    auto source = [&](const Manager::Emit& emit, const std::atomic<bool>& cancelled) {
        const bool complete = LSM::visitLatest(user, db, collection,
            [&](const LSM::LatestRowView& row) {
                if (cancelled.load()) throw std::runtime_error("cancelled vector source");
                const auto fields = row.project({"id", "vector"});
                emit({fields.at("id").get<std::string>(),
                      fields.at("vector").get<std::vector<float>>()});
                return true;
            });
        if (!complete) throw std::runtime_error("incomplete vector source");
    };
    assert(manager.enqueue(config, certificate, source)); ready(manager);
    const auto beforeDelete = certificate(); assert(beforeDelete);
    auto retained = manager.acquire(*beforeDelete); assert(retained);
    assert(retained->graph->size() == 2);
    LSM::del(user, db, collection, "a");
    assert(certificate() && certificate() != beforeDelete);
    assert(!manager.acquire(*certificate()));
    assert(manager.enqueue(config, certificate, source)); ready(manager);
    assert(manager.acquire(*certificate())->graph->size() == 1);
    previous = certificate();
    assert(LSM::applySnapshot(snapshot));
    assert(certificate() && certificate() != previous);
    assert(!manager.acquire(*certificate()));
    assert(manager.enqueue(config, certificate, source)); ready(manager);
    assert(manager.acquire(*certificate())->graph->size() == 2);
    assert(retained->graph->size() == 2);
    previous = certificate(); LSM::restoreFromWal();
    assert(certificate() && certificate() != previous);
    previous = certificate(); assert(LSM::dropCollection(user, db, collection));
    assert(certificate() && certificate() != previous);
    assert(!manager.acquire(*certificate()));
    previous = certificate(); LSM::init(root.dataRoot().string());
    assert(certificate() && certificate() != previous);
    std::cout << "VECTOR_FRESHNESS_STORAGE_PASS\n";
}
