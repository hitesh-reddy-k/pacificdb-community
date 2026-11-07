#include "vector_index_catalog.hpp"
#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <iostream>

using Catalog = pacificdb::VectorIndexCatalog;
template<class F> void rejects(F function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}
int main() {
    OwnedTestRoot root("vector-index-catalog");
    DatabaseEngine::init(root.dataRoot().string());
    for (const std::string user : {"alice", "bob"}) {
        DatabaseEngine::ensureUserRoot(user);
        assert(DatabaseEngine::createDatabase(user, "app", "vector"));
        assert(!DatabaseEngine::createCollection(user, "app", "vectors").empty());
    }
    const json input{{"dimensions", 25}, {"metric", "euclidean"}};
    const std::vector<double> durableVector(25, 1.0);
    DatabaseEngine::insertVector("alice", "app", "vectors",
        {{"id", "authoritative"}, {"vector", durableVector}});
    const auto expected = Catalog::validate(input);
    assert(expected.at("metric") == "l2");
    assert(Catalog::validate(expected) == expected);
    assert(Catalog::graphConfig(expected).metric == pacificdb::DistanceMetric::EUCLIDEAN);
    for (const json bad : {json::array(), json::object(), json{{"dimensions", -1}},
            json{{"dimensions", 0}}, json{{"dimensions", 1.5}}, json{{"dimensions", 65537}},
            json{{"dimensions", 2}, {"M", 1}}, json{{"dimensions", 2}, {"ef_search", 0}},
            json{{"dimensions", 2}, {"M", 64}, {"ef_construction", 32}},
            json{{"dimensions", 2}, {"metric", "unknown"}},
            json{{"dimensions", 2}, {"max_source_bytes", 1}},
            json{{"dimensions", 2}, {"max_index_bytes", 1}},
            json{{"dimensions", 2}, {"quantization", true}}}) {
        rejects([&] { Catalog::validate(bad); });
    }
    rejects([&] { Catalog::configure("alice", "app", "missing", input); });
    assert(Catalog::list("alice", "app", "vectors").empty());
    assert(Catalog::configure("alice", "app", "vectors", input) == expected);
    assert(Catalog::list("alice", "app", "vectors") == std::vector<json>{expected});
    assert(Catalog::list("bob", "app", "vectors").empty());
    auto updated = input; updated["ef_search"] = 300;
    const auto replacement = Catalog::configure("alice", "app", "vectors", updated);
    assert(Catalog::list("alice", "app", "vectors") == std::vector<json>{replacement});
    LSM::flush("alice", "pacificdb_meta", "vector_index_specs_v1");
    LSM::restoreFromWal();
    assert(Catalog::list("alice", "app", "vectors") == std::vector<json>{replacement});
    const auto snapshot = LSM::exportSnapshotJson(1);
    assert(Catalog::erase("alice", "app", "vectors", "vector"));
    assert(Catalog::list("alice", "app", "vectors").empty());
    const auto records = DatabaseEngine::find("alice", "app", "vectors", {{"id", "authoritative"}});
    assert(records.size() == 1 && records.front().at("vector") == json(durableVector));
    assert(LSM::applySnapshot(snapshot));
    assert(Catalog::list("alice", "app", "vectors") == std::vector<json>{replacement});
    // Old specs cannot attach to a newly created collection with the same name.
    assert(DatabaseEngine::dropCollection("alice", "app", "vectors"));
    assert(!DatabaseEngine::createCollection("alice", "app", "vectors").empty());
    assert(Catalog::list("alice", "app", "vectors").empty());
    std::cout << "VECTOR_INDEX_CATALOG_PASS (standalone scope)\n";
}
