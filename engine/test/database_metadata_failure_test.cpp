#include "database_engine.hpp"
#include "owned_test_root.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static std::string bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main() {
    const OwnedTestRoot root("metadata-publication-failure");
    DatabaseEngine::init(root.dataRoot().string(), false);
    require(DatabaseEngine::createDatabase("alice", "app"), "baseline database");
    require(!DatabaseEngine::createCollection("alice", "app", "baseline").empty(),
            "baseline collection");
    const auto database = root.dataRoot() / "alice" / "app";
    const auto metadata = database / "db.meta";
    const auto previous = bytes(metadata);
    const auto blocked = database / "db.meta.security.tmp";
    fs::create_directory(blocked);
    require(DatabaseEngine::createCollection("alice", "app", "new").empty(),
            "collection creation acknowledged failed metadata publication");
    require(bytes(metadata) == previous, "failed creation replaced existing metadata");
    // A failed attempt can leave a collection marker. Retrying must repair its
    // catalog entry through the same checked publication path.
    require(DatabaseEngine::createCollection("alice", "app", "new").empty(),
            "repair acknowledged failed metadata publication");
    require(bytes(metadata) == previous, "failed repair replaced existing metadata");
    require(!DatabaseEngine::applyReplicatedEntry({
                {"action", "createCollection"}, {"op", "CREATE_COLLECTION"},
                {"userId", "alice"}, {"db", "app"}, {"collection", "new"}}),
            "replicated apply accepted an unpublished collection marker");
    require(bytes(metadata) == previous, "failed apply replaced existing metadata");
    fs::remove(blocked);
    require(!DatabaseEngine::createCollection("alice", "app", "new").empty(),
            "retry did not repair collection metadata");
    require(!DatabaseEngine::createCollection("alice", "app", "baseline").empty(),
            "baseline collection was lost");

    const auto fresh = root.dataRoot() / "alice" / "fresh";
    fs::create_directories(fresh / "db.meta.security.tmp");
    require(!DatabaseEngine::createDatabase("alice", "fresh"),
            "database creation acknowledged failed metadata publication");
    require(!fs::exists(fresh / "db.meta"), "failed creation left a partial catalog");
    fs::remove(fresh / "db.meta.security.tmp");
    require(DatabaseEngine::createDatabase("alice", "fresh"), "database retry failed");
    std::cout << "DATABASE_METADATA_FAILURE_PASS\n";
}
