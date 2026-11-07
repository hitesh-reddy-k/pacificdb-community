#include "database_engine.hpp"
#include "owned_test_root.hpp"
#include "wal.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <fstream>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void ready(const std::string& name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        auto status = DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", name);
        if (status.at("state") == "READY") return;
        require(status.at("state") != "FAILED", "native index build failed");
        require(std::chrono::steady_clock::now() < deadline, "native index readiness deadline");
        std::this_thread::yield();
    }
}
int main() {
    OwnedTestRoot root("native-point-cache");
    DatabaseEngine::init(root.dataRoot().string());
    struct Stop { ~Stop() { DatabaseEngine::stopNativeVectorIndexes(); WAL::shutdown(); } } stop;
    try {
        DatabaseEngine::ensureUserRoot("alice");
        require(DatabaseEngine::createDatabase("alice", "app", "vector"), "create database");
        require(!DatabaseEngine::createCollection("alice", "app", "vectors").empty(), "create collection");
        std::vector<json> records;
        for (int i = 0; i < 200; ++i) records.push_back({{"id", std::to_string(i)},
            {"vector", {double(i), 1.}}, {"group", i%2}, {"title", i == 199 ? "rare ocean" : "shared ocean"}});
        DatabaseEngine::insertMany("alice", "app", "vectors", records);
        const auto options = [](const std::string& name, const std::string& algorithm) {
            return json{{"name", name}, {"algorithm", algorithm}, {"dimensions", 2}, {"metric", "dot"},
                {"max_elements", 500}, {"ef_search", 100}, {"max_source_bytes", 1024*1024},
                {"max_index_bytes", 4*1024*1024}, {"max_lexical_bytes", 1024*1024}};
        };
        auto flat = options("flat", "flat"); flat["text_field"] = "title";
        DatabaseEngine::buildNativeVectorIndex("alice", "app", "vectors", flat); ready("flat");
        const json query{{"vector", {1., 0.}}, {"metric", "dot"}, {"k", 10}};
        const auto correct = [](const std::vector<json>& hits) {
            require(hits.size() == 10, "native result cardinality");
            for (int i = 0; i < 10; ++i) {
                require(hits[i].at("id") == std::to_string(199-i), "native ranking changed with cache reuse");
                require(hits[i].at("score") == 199-i, "native score changed with cache reuse");
            }
        };
        json execution;
        correct(DatabaseEngine::queryNativeExactIndex("alice", "app", "vectors", "flat", query, &execution));
        correct(DatabaseEngine::queryNativeExactIndex("alice", "app", "vectors", "flat", query, &execution));
        require(execution.value("payload_cache_hits", size_t{0}) == 10,
            "native exact warm query did not reuse verified authoritative payloads");
        const json hybrid{{"vector", {1., 0.}}, {"metric", "dot"}, {"text", "rare"},
            {"k", 5}, {"candidates", 30}, {"filter", {{"group", 1}}}};
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto hits = DatabaseEngine::queryNativeHybridIndex("alice", "app", "vectors", "flat", hybrid);
            require(hits.size() == 5 && hits.front().at("id") == "199", "hybrid ranking changed with cache reuse");
            for (const auto& hit : hits) require(hit.at("group") == 1, "hybrid cache bypassed its metadata filter");
        }
        // Catalog changes conservatively invalidate earlier generations. Exercise
        // each index while its own source/configuration certificate is eligible.
        DatabaseEngine::buildNativeVectorIndex("alice", "app", "vectors", options("ann", "hnsw")); ready("ann");
        correct(DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "ann", query, &execution));
        correct(DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "ann", query, &execution));
        std::cout << "ANN_CACHE_EXECUTION " << execution.dump() << '\n';
        require(execution.contains("timings_ms"), "native ANN stage timings are missing");
        for (const char* stage : {"definition", "graph", "record_resolution", "scoring", "final_validation", "total"})
            require(execution.at("timings_ms").at(stage).get<double>() >= 0., "invalid native ANN stage timing");
        require(execution.at("payload_fetches").get<size_t>() >= 10 &&
                execution.at("payload_cache_hits") == execution.at("payload_fetches"),
            "native ANN warm query did not reuse verified candidate payloads");
        require(execution.at("authoritative_fetches")==0,"ANN generation cache unexpectedly read storage candidates");
        require(execution.at("document_materializations")==10,"ANN materialized documents beyond top K");
        DatabaseEngine::stopNativeVectorIndexes();
        DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","ann");ready("ann");
        require(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann").value("checkpoint_restored",false),"valid graph checkpoint was not reused");
        correct(DatabaseEngine::queryNativeVectorIndex("alice","app","vectors","ann",query));
        DatabaseEngine::stopNativeVectorIndexes();
        const auto cache=std::filesystem::path(root.dataRoot().string()+".vector-derived-v1");
        bool corrupted=false;
        for(const auto& entry:std::filesystem::directory_iterator(cache))if(entry.path().extension()==".graph"){
            std::fstream file(entry.path(),std::ios::binary|std::ios::in|std::ios::out);file.seekp(105);file.put('X');corrupted=true;
        }
        require(corrupted,"graph checkpoint was not written");
        DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","ann");ready("ann");
        const auto rebuilt=DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann");
        require(!rebuilt.value("checkpoint_restored",true)&&rebuilt.contains("checkpoint_error"),"corrupt checkpoint did not rebuild with diagnostics");
        correct(DatabaseEngine::queryNativeVectorIndex("alice","app","vectors","ann",query));
        require(DatabaseEngine::updateOne("alice", "app", "vectors", {{"id", "199"}},
            {{"$set", {{"vector", {-1., 0.}}}}}), "update failed");
        for (const auto& name : {"flat", "ann"}) {
            bool rejected = false;
            try {
                if (std::string(name) == "flat") DatabaseEngine::queryNativeExactIndex("alice", "app", "vectors", name, query);
                else DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", name, query);
            } catch (const std::exception& error) { rejected = std::string(error.what()).find("unavailable") != std::string::npos; }
            require(rejected, "warm payload cache made a stale native index eligible");
        }
        std::cout << "NATIVE_POINT_CACHE_PASS\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "NATIVE_POINT_CACHE_FAIL: " << error.what() << '\n'; return 1;
    }
}
