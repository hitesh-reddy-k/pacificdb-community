#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

static void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << std::endl;
        std::exit(1);
    }
    std::cout << "[PASS] " << message << std::endl;
}

int main() {
    const OwnedTestRoot ownedRoot("vector-correctness");
    const std::string root = ownedRoot.dataRoot().string();
    const std::string user = "vector_tester";

    DatabaseEngine::init(root);
    LSM::init(root);
    DatabaseEngine::ensureUserRoot(user);
    DatabaseEngine::createDatabase(user, "vector_db", "vector");
    DatabaseEngine::createCollection(user, "vector_db", "embeddings");

    DatabaseEngine::insertVector(user, "vector_db", "embeddings",
        {{"id", "unit"}, {"tag", "keep"}, {"modality", "text"},
         {"vector", json::array({1.0, 0.0})}});
    DatabaseEngine::insertVector(user, "vector_db", "embeddings",
        {{"id", "big"}, {"tag", "drop"}, {"modality", "image"},
         {"vector", json::array({10.0, 0.0})}});
    DatabaseEngine::insertVector(user, "vector_db", "embeddings",
        {{"id", "orthogonal"}, {"vector", json::array({0.0, 1.0})}});
    DatabaseEngine::insertVector(user, "vector_db", "embeddings",
        {{"id", "bad_dim"}, {"vector", json::array({1.0, 0.0, 0.0})}});
    LSM::flush(user, "vector_db", "embeddings");

    auto cosine = DatabaseEngine::queryVector(user, "vector_db", "embeddings", {
        {"vector", json::array({1.0, 0.0})},
        {"k", 4},
        {"metric", "cosine"}
    });

    bool sawUnit = false;
    bool sawBig = false;
    bool sawBadDim = false;
    for (const auto& row : cosine) {
        const std::string id = row.value("id", std::string(""));
        if (id == "unit") {
            sawUnit = true;
            expect(std::abs(row.value("score", 0.0) - 1.0) < 1e-9,
                   "cosine unit score is normalized");
        } else if (id == "big") {
            sawBig = true;
            expect(std::abs(row.value("score", 0.0) - 1.0) < 1e-9,
                   "cosine same direction ignores magnitude");
        } else if (id == "bad_dim") {
            sawBadDim = true;
        }
    }
    expect(sawUnit && sawBig, "cosine returns same-direction vectors");
    expect(!sawBadDim, "dimension-mismatched vector is skipped");

    auto dot = DatabaseEngine::queryVector(user, "vector_db", "embeddings", {
        {"vector", json::array({1.0, 0.0})},
        {"k", 1},
        {"metric", "dot"}
    });
    expect(!dot.empty() && dot[0].value("id", "") == "big",
           "dot product ranking remains magnitude-sensitive");

    auto filtered = DatabaseEngine::queryVector(user, "vector_db", "embeddings", {
        {"vector", json::array({1.0, 0.0})},
        {"k", 4},
        {"metric", "cosine"},
        {"filter", {{"tag", "keep"}}},
        {"modality", "text"}
    });
    expect(filtered.size() == 1 && filtered[0].value("id", "") == "unit",
           "streaming vector scan applies field and modality filters");

    std::cout << "VECTOR_CORRECTNESS_PASS" << std::endl;
    return 0;
}
