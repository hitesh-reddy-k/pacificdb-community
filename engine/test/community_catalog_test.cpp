#include "community_catalog.hpp"
#include "database_engine.hpp"
#include "owned_test_root.hpp"

#include <cassert>
#include <iostream>

int main() {
    const OwnedTestRoot root("community-catalog");
    DatabaseEngine::init(root.dataRoot().string(), false);

    auto& catalog = pacificdb::community::CommunityCatalog::instance();
    catalog.initialize("system");
    const auto project = catalog.createProject("system", "demo");

    assert(project.at("name") == "demo");
    assert(catalog.mapDatabase("system", project.at("id"), "app"));
    assert(catalog.databaseProject("system", "app").at("project_id") ==
           project.at("id"));
    DatabaseEngine::createDatabase("system", "app");
    const auto otherProject = catalog.createProject("system", "other");
    DatabaseEngine::createDatabase("system", "other-db");
    assert(catalog.mapDatabase("system", otherProject.at("id"), "other-db"));
    assert(catalog.listProjectDatabases("system", project.at("id")) ==
           nlohmann::json::array({"app"}));
    assert(catalog.listProjectDatabases("system", otherProject.at("id")) ==
           nlohmann::json::array({"other-db"}));
    assert(pacificdb::community::isReservedDatabase("pacificdb_meta"));
    assert(pacificdb::community::isReservedDatabase("system"));
    assert(!pacificdb::community::isReservedDatabase("app"));

    const auto upload = catalog.beginMedia(
        "system", "app", "photos", "movie.mp4", "video/mp4", 4, 1,
        "63c1dd951ffedf6f7fd968ad4efa39b8ed584f162f46e715114ee184f8de"
        "9201");
    assert(catalog.putMediaChunk(
                      "system", upload.at("id"), 0, "QUFBQQ==", 4,
                      "63c1dd951ffedf6f7fd968ad4efa39b8ed584f162f46e715114ee184f8de"
                      "9201")
               .at("stored"));
    bool mismatched = false;
    try {
        catalog.putMediaChunk("system", upload.at("id"), 0, "QkJCQg==", 4,
                              "different");
    } catch (...) {
        mismatched = true;
    }
    assert(mismatched);
    assert(catalog.listMedia("system", false).empty());
    assert(catalog.finalizeMedia("system", upload.at("id")).at("status") ==
           "ready");
    assert(catalog.listMedia("system", false).size() == 1);

    std::cout << "COMMUNITY_CATALOG_PASS\n";
    return 0;
}
