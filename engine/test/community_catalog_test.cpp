#include "community_catalog.hpp"
#include "database_engine.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"

#include <openssl/evp.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

std::string sha256(const std::string& value) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(value.data(), value.size(), digest, &length,
                   EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("test sha256 failed");
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < length; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

std::string base64(const std::string& value) {
    std::string encoded(((value.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<int>(value.size()));
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

int main() {
    const OwnedTestRoot root("community-catalog");
    DatabaseEngine::init(root.dataRoot().string(), false);
#ifdef _WIN32
    _putenv_s("MAX_RESULT_SIZE_MB", "1");
#else
    setenv("MAX_RESULT_SIZE_MB", "1", 1);
#endif
    QueryLimiter::init();

    auto& catalog = pacificdb::community::CommunityCatalog::instance();
    catalog.initialize("system");
    const auto project = catalog.createProject("system", "demo");

    assert(project.at("name") == "demo");
    DatabaseEngine::createDatabase("system", "app");
    assert(catalog.mapDatabase("system", project.at("id"), "app"));
    assert(catalog.mapDatabase("system", project.at("id"), "app"));
    assert(catalog.databaseProject("system", "app").at("project_id") ==
           project.at("id"));
    const auto otherProject = catalog.createProject("system", "other");
    bool crossProjectRejected = false;
    try {
        catalog.mapDatabase("system", otherProject.at("id"), "app");
    } catch (const std::invalid_argument&) {
        crossProjectRejected = true;
    }
    assert(crossProjectRejected);
    assert(catalog.databaseProject("system", "app").at("project_id") ==
           project.at("id"));
    bool projectDeleteRejected = false;
    try {
        catalog.deleteProject("system", project.at("id"));
    } catch (const std::invalid_argument&) {
        projectDeleteRejected = true;
    }
    assert(projectDeleteRejected);
    bool missingDatabaseRejected = false;
    try {
        catalog.mapDatabase("system", project.at("id"), "missing-db");
    } catch (const std::invalid_argument&) {
        missingDatabaseRejected = true;
    }
    assert(missingDatabaseRejected);
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
    assert(upload.at("status") == "uploading");
    assert(upload.at("state_version") == 2);
    assert(upload.at("received_chunks") == 0);
    assert(upload.at("received_bytes") == 0);
    assert(upload.at("next_chunk") == 0);
    assert(upload.at("resumable") == true);
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

    const std::string nineBytes = "AAABBBCCC";
    const auto resumable = catalog.beginMedia(
        "system", "app", "photos", "resume.mp4", "video/mp4",
        static_cast<long long>(nineBytes.size()), 3, sha256(nineBytes));
    const auto firstProgress = catalog.putMediaChunk(
        "system", resumable.at("id"), 0, "QUFB", 3, sha256("AAA"));
    assert(firstProgress.at("received_chunks") == 1);
    assert(firstProgress.at("received_bytes") == 3);
    assert(firstProgress.at("next_chunk") == 1);
    assert(firstProgress.at("received_indices") == nlohmann::json::array({0}));
    const auto resumed = catalog.beginMedia(
        "system", "app", "photos", "resume.mp4", "video/mp4",
        static_cast<long long>(nineBytes.size()), 3, sha256(nineBytes),
        resumable.at("id"));
    assert(resumed.at("id") == resumable.at("id"));
    assert(resumed.at("next_chunk") == 1);
    const auto incomplete = catalog.finalizeMedia("system", resumable.at("id"));
    assert(incomplete.at("resumable") == true);
    assert(incomplete.at("error") == "media_chunks_missing");
    assert(incomplete.at("next_chunk") == 1);
    assert(catalog.cleanupMedia("system", resumable.at("id")) == 1);
    assert(catalog.cleanupMedia("system", resumable.at("id")) == 0);
    assert(catalog.getMedia("system", resumable.at("id")).at("status") ==
           "aborted");
    assert(catalog.cleanupMedia("system", upload.at("id")) == 0);

    const auto conflicting = catalog.beginMedia(
        "system", "app", "photos", "conflict.bin", "application/octet-stream",
        3, 1, sha256("AAA"));
    catalog.putMediaChunk("system", conflicting.at("id"), 0,
                          "QUFB", 3, sha256("AAA"));
    bool conflictRejected = false;
    try {
        catalog.putMediaChunk("system", conflicting.at("id"), 0,
                              "QkJC", 3, sha256("BBB"));
    } catch (const std::invalid_argument&) {
        conflictRejected = true;
    }
    assert(conflictRejected);
    assert(catalog.getMedia("system", conflicting.at("id")).at("status") ==
           "failed");
    assert(catalog.getMediaChunk("system", conflicting.at("id"), 0).is_null());

    const auto reconcile = catalog.beginMedia(
        "system", "app", "photos", "reconcile.bin", "application/octet-stream",
        6, 2, sha256("AAABBB"));
    DatabaseEngine::insert("system", "pacificdb_meta", "media_chunks", {
        {"id", reconcile.at("id").get<std::string>() + ":1"},
        {"media_id", reconcile.at("id")}, {"index", 1},
        {"size_bytes", 3}, {"sha256", sha256("BBB")}, {"data", "QkJC"},
    });
    DatabaseEngine::updateOne("system", "pacificdb_meta", "media_manifests",
                              {{"id", reconcile.at("id")}},
                              {{"$set", {{"status", "verifying"}}}});
    const auto reconciled = catalog.reconcileMedia("system", reconcile.at("id"));
    assert(reconciled.at("repaired_progress") == 1);
    assert(reconciled.at("reset_verifying") == 1);
    const auto repaired = catalog.getMedia("system", reconcile.at("id"));
    assert(repaired.at("status") == "uploading");
    assert(repaired.at("received_chunks") == 1);
    assert(repaired.at("received_bytes") == 3);
    assert(repaired.at("next_chunk") == 0);

    DatabaseEngine::insert("system", "pacificdb_meta", "media_chunks", {
        {"id", "missing-media:0"}, {"media_id", "missing-media"},
        {"index", 0}, {"size_bytes", 3}, {"sha256", sha256("AAA")},
        {"data", "QUFB"},
    });
    assert(catalog.reconcileMedia("system").at("removed_orphans") == 1);
    assert(catalog.getMediaChunk("system", "missing-media", 0).is_null());

    const auto fresh = catalog.beginMedia(
        "system", "app", "photos", "fresh.bin", "application/octet-stream",
        3, 1, sha256("AAA"));
    const auto expired = catalog.beginMedia(
        "system", "app", "photos", "expired.bin", "application/octet-stream",
        3, 1, sha256("AAA"));
    DatabaseEngine::updateOne("system", "pacificdb_meta", "media_manifests",
                              {{"id", expired.at("id")}},
                              {{"$set", {{"lease_expires_at_ms", nowMs() - 1}}}});
    assert(catalog.cleanupMedia("system") == 1);
    assert(catalog.getMedia("system", fresh.at("id")).at("status") == "uploading");
    assert(catalog.getMedia("system", expired.at("id")).at("status") == "aborted");

    // The combined base64 chunks exceed the configured 1 MiB query result
    // ceiling. Upload, resume, listing and finalization must use bounded
    // per-chunk reads rather than one unbounded media_id query.
    const std::string largeChunk(600 * 1024, 'A');
    const auto largeUpload = catalog.beginMedia(
        "system", "app", "photos", "large.bin", "application/octet-stream",
        static_cast<long long>(largeChunk.size() * 2), 2,
        sha256(largeChunk + largeChunk));
    const auto encodedChunk = base64(largeChunk);
    assert(catalog.putMediaChunk("system", largeUpload.at("id"), 1,
                                encodedChunk,
                                static_cast<long long>(largeChunk.size()),
                                sha256(largeChunk)).at("next_chunk") == 0);
    assert(catalog.putMediaChunk("system", largeUpload.at("id"), 0,
                                encodedChunk,
                                static_cast<long long>(largeChunk.size()),
                                sha256(largeChunk)).at("next_chunk") == 2);
    assert(catalog.getMedia("system", largeUpload.at("id"))
               .at("received_chunks") == 2);
    assert(catalog.finalizeMedia("system", largeUpload.at("id"))
               .at("status") == "ready");
    assert(catalog.getMediaChunk("system", largeUpload.at("id"), 1)
               .at("data") == encodedChunk);

    // Catalog maintenance must continue past the normal per-query document
    // limit and must not materialize all media payloads in one result.
    DatabaseEngine::createDatabase("system", "page-a");
    DatabaseEngine::createDatabase("system", "page-b");
    assert(catalog.mapDatabase("system", project.at("id"), "page-a"));
    assert(catalog.mapDatabase("system", project.at("id"), "page-b"));
    for (int index = 0; index < 5; ++index) {
        DatabaseEngine::insert("system", "pacificdb_meta", "media_chunks", {
            {"id", "orphan-page-" + std::to_string(index) + ":0"},
            {"media_id", "orphan-page-" + std::to_string(index)},
            {"index", 0}, {"size_bytes", 3},
            {"sha256", sha256("AAA")}, {"data", "QUFB"},
        });
    }
    const auto priorMaxDocs = QueryLimiter::getMaxResultDocs();
    QueryLimiter::setMaxResultDocs(2);
    assert(catalog.listProjectDatabases("system", project.at("id")) ==
           nlohmann::json::array({"app", "page-a", "page-b"}));
    assert(catalog.reconcileMedia("system").at("removed_orphans") == 5);
    QueryLimiter::setMaxResultDocs(priorMaxDocs);

    std::cout << "COMMUNITY_CATALOG_PASS\n";
    return 0;
}
