#include "native_vector_index_service.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

using namespace pacificdb;
using State = VectorIndexGeneration::State;
template<class F> void fails(F function, const std::string& expected) {
    bool failed = false;
    try { function(); } catch (const std::exception& e) {
        failed = std::string(e.what()).find(expected) != std::string::npos;
    }
    assert(failed);
}
void wait(const std::shared_ptr<VectorIndexGeneration>& manager, State expected) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (manager->status().state != expected) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main() {
    const auto certificate = []() -> std::optional<VectorFreshness::Stamp> { return VectorFreshness::Stamp{1}; };
    HNSWConfig config; config.dimensions = 2;
    const auto source = [](const VectorIndexGeneration::Emit& emit, const std::atomic<bool>&) {
        for (int i = 0; i < 50; ++i) emit({std::to_string(i), {float(i), 1}});
    };
    NativeVectorIndexService service(2, 1024*1024, 200000);
    std::atomic<bool> started{false};
    auto blocking = service.queue("alice/app/one", json::object(), config, 100000, certificate,
        [&](const VectorIndexGeneration::Emit&, const std::atomic<bool>& cancelled) {
            assert(service.metrics().at("live_workers") == 1); // no registry lock in callbacks
            started.store(true);
            while (!cancelled.load()) std::this_thread::yield();
        });
    while (!started.load()) std::this_thread::yield();
    auto other = service.queue("bob/app/two", json::object(), config, 100000, certificate, source);
    wait(other, State::READY);
    auto retained = other->acquire(VectorFreshness::Stamp{1}); assert(retained);
    assert(retained->graph->size() == 50);
    fails([&] { service.queue("third", json::object(), config, 100000, certificate, source); }, "worker_limit");
    auto different = config; different.metric = DistanceMetric::DOT_PRODUCT;
    fails([&] { service.queue("bob/app/two", json::object(), different, 100000, certificate, source); }, "reconfigure");
    assert(service.get("bob/app/two") == other);
    assert(service.unload("alice/app/one"));
    assert(!service.get("alice/app/one"));
    assert(!blocking->enqueue(config, certificate, source));
    // A retained manager continues reserving its worker/source slot until joined.
    fails([&] { service.queue("third", json::object(), config, 100000, certificate, source); }, "worker_limit");
    blocking.reset();
    auto replacement = service.queue("third", json::object(), config, 100000, certificate, source);
    wait(replacement, State::READY);
    service.shutdown(); assert(!service.get("bob/app/two"));
    assert(!other->acquire(VectorFreshness::Stamp{1}));
    assert(retained->graph->size() == 50);
    fails([&] { service.queue("fourth", json::object(), config, 100000, certificate, source); }, "stopped");
    other.reset(); replacement.reset();
    assert(service.metrics().at("live_workers") == 0);
    assert(service.metrics().at("reserved_source_bytes") == 0);
    assert(service.metrics().at("graph_bytes").get<size_t>() > 0);
    retained.reset(); assert(service.metrics().at("graph_bytes") == 0);

    NativeVectorIndexService small(2, 1024*1024, 100000);
    auto first = small.queue("first", json::object(), config, 100000, certificate, source); wait(first, State::READY);
    fails([&] { small.queue("second", json::object(), config, 1, certificate, source); }, "memory_budget");
    assert(small.metrics().at("live_workers") == 1 && !small.get("second"));

    NativeVectorIndexService concurrent(2, 1024*1024, 100000);
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) callers.emplace_back([&] {
        concurrent.queue("same", json::object(), config, 100000, certificate, source);
    });
    for (auto& caller : callers) caller.join();
    auto shared = concurrent.get("same"); wait(shared, State::READY);
    assert(concurrent.metrics().at("live_workers") == 1);
    concurrent.shutdown(); shared.reset();
    assert(concurrent.metrics().at("graph_bytes") == 0);

    // Traversal-local cancellation must not poison a successful graph.
    HNSWIndex graph(config);
    for (int i = 0; i < 200; ++i) assert(graph.insert(std::to_string(i), {float(i), 1}));
    size_t polls = 0;
    fails([&] { graph.search({1, 0}, 10, [&] {
        if (++polls == 5) throw std::runtime_error("query cancelled");
    }); }, "query cancelled");
    assert(graph.getStats().at("healthy") == true && graph.search({1, 0}, 10).size() == 10);
    fails([&] { graph.insert("cancelled", {2, 1}, [] { throw std::runtime_error("build cancelled"); }); }, "build cancelled");
    assert(graph.getStats().at("healthy") == false);
    fails([&] { graph.search({1, 0}, 10); }, "unhealthy");

    auto timeoutConfig = config; timeoutConfig.maxBuildMilliseconds = 1;
    NativeVectorIndexService timeout(1, 1024*1024, 100000);
    auto timed = timeout.queue("timeout", json::object(), timeoutConfig, 100000, certificate,
        [](const VectorIndexGeneration::Emit& emit, const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); emit({"a", {1, 0}});
        });
    wait(timed, State::FAILED);
    assert(timed->status().error == "vector_index_build_timeout");
    assert(!timed->acquire(VectorFreshness::Stamp{1}));
    // Rebuild fits a one-generation budget; an externally retained generation
    // remains charged and can explicitly prevent replacement until released.
    NativeVectorIndexService flatService(1,512*1024,1024*1024);
    HNSWConfig flatConfig;flatConfig.metric=DistanceMetric::DOT_PRODUCT;flatConfig.dimensions=25;flatConfig.maxElements=1000;flatConfig.maxMemoryBytes=512*1024;
    const auto flatSource=[](const VectorIndexGeneration::Emit& emit,const std::atomic<bool>&){
        for(int i=0;i<1000;++i)emit({std::to_string(i),{},std::nullopt,std::vector<double>(25,double(i))});
    };
    auto packed=flatService.queue("flat",json::object(),flatConfig,1024*1024,certificate,flatSource,{},true);
    wait(packed,State::READY);
    assert(flatService.metrics().at("graph_bytes").get<size_t>()>256*1024);
    flatService.queue("flat",json::object(),flatConfig,1024*1024,certificate,flatSource,{},true);
    wait(packed,State::READY);assert(packed->status().generation==2);
    auto held=packed->acquire(VectorFreshness::Stamp{1});assert(held&&held->flat);
    flatService.queue("flat",json::object(),flatConfig,1024*1024,certificate,flatSource,{},true);
    wait(packed,State::FAILED);assert(packed->status().error.find("memory_budget")!=std::string::npos);
    assert(held->flat->search(std::vector<double>(25,1.),1).front().first=="999");
    held.reset();assert(flatService.metrics().at("graph_bytes")==0);
    flatService.queue("flat",json::object(),flatConfig,1024*1024,certificate,flatSource,{},true);
    wait(packed,State::READY);
    std::atomic<int> admissions{0};
    flatService.queue("flat",json::object(),flatConfig,1024*1024,certificate,flatSource,{},true,[&]{
        if(++admissions==2)throw std::runtime_error("admission_guard_failed");
    });
    wait(packed,State::FAILED);assert(packed->status().error=="admission_guard_failed");
    // Namespace retirement returns while the source is blocked; its worker
    // observes cancellation and frees index ownership before reaping.
    NativeVectorIndexService retiring(1,1024*1024,100000);
    std::atomic<bool> entered{false},release{false};
    auto cancelled=retiring.queue("tenant/db/old",json::object(),config,100000,certificate,
        [&](const VectorIndexGeneration::Emit& emit,const std::atomic<bool>& stop){
            entered=true;while(!release.load()&&!stop.load())std::this_thread::yield();
            if(!stop.load())emit({"a",{1,0}});
        });
    while(!entered.load())std::this_thread::yield();
    retiring.retire([](const std::string& key){return key=="tenant/db/old";});
    assert(!retiring.get("tenant/db/old"));
    const auto stopDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!cancelled->finished()&&std::chrono::steady_clock::now()<stopDeadline)std::this_thread::yield();
    assert(cancelled->finished());cancelled.reset();retiring.reap();
    assert(retiring.metrics().at("live_workers")==0);
    auto retiredReplacement=retiring.queue("tenant/db/new",json::object(),config,100000,certificate,
        [](const VectorIndexGeneration::Emit& emit,const std::atomic<bool>&){emit({"a",{1,0}});});
    wait(retiredReplacement,State::READY);
    auto heldGeneration=retiredReplacement->acquire(VectorFreshness::Stamp{1});assert(heldGeneration);
    retiring.retire([](const std::string&){return true;});
    while(!retiredReplacement->finished()&&std::chrono::steady_clock::now()<stopDeadline)std::this_thread::yield();
    assert(retiredReplacement->finished());retiredReplacement.reset();retiring.reap();
    assert(retiring.metrics().at("live_workers")==0);
    assert(retiring.metrics().at("graph_bytes").get<size_t>()>0);
    assert(heldGeneration->graph->search({1,0},1).front().first=="a");
    heldGeneration.reset();assert(retiring.metrics().at("graph_bytes")==0);
    std::cout << "NATIVE_VECTOR_INDEX_SERVICE_PASS\n";
}
