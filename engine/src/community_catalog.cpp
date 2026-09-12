#include "community_catalog.hpp"

#include "database_engine.hpp"
#include "id_generator.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace pacificdb::community {
namespace {

constexpr const char* kDatabase = "pacificdb_meta";
constexpr const char* kProjects = "projects";
constexpr const char* kDatabaseProjects = "database_projects";
constexpr const char* kMediaManifests = "media_manifests";
constexpr const char* kMediaChunks = "media_chunks";
constexpr const char* kRestoreJournal = "restore_journal";

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

json publicDocument(json document) {
    static constexpr const char* internal[] = {
        "_mvcc_commit_ms", "_mvcc_version", "_raft_commit_index",
        "_raft_term",      "_visibility_floor", "_visibility_state",
        "committed",       "created_at_ms",     "created_txn",
        "deleted_at_ms",   "deleted_txn",       "tenant_id", "version"};
    for (const char* key : internal) document.erase(key);
    return document;
}

void requireText(const std::string& value, const char* name) {
    if (value.empty() || value.size() > 128) {
        throw std::invalid_argument(std::string(name) + " must be 1-128 characters");
    }
}

void requireSha256(const std::string& value) {
    if (value.size() != 64 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
        throw std::invalid_argument("sha256 must contain 64 hexadecimal characters");
    }
}

std::vector<unsigned char> decodeBase64(const std::string& encoded) {
    if (encoded.empty() || encoded.size() % 4 != 0) {
        throw std::invalid_argument("invalid base64 media chunk");
    }
    std::vector<unsigned char> decoded(encoded.size() / 4 * 3);
    const int count = EVP_DecodeBlock(decoded.data(),
                                      reinterpret_cast<const unsigned char*>(encoded.data()),
                                      static_cast<int>(encoded.size()));
    if (count < 0) throw std::invalid_argument("invalid base64 media chunk");
    std::size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=') ++padding;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ++padding;
    decoded.resize(static_cast<std::size_t>(count) - padding);
    return decoded;
}

std::string sha256Hex(const std::vector<unsigned char>& bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("could not hash media chunk");
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) out << std::setw(2) << int(digest[i]);
    return out.str();
}

std::string chunkId(const std::string& mediaId, long long index) {
    return mediaId + ":" + std::to_string(index);
}

}  // namespace

bool isReservedDatabase(std::string_view name) {
    return name == kDatabase || name == "system";
}

CommunityCatalog& CommunityCatalog::instance() {
    static CommunityCatalog catalog;
    return catalog;
}

void CommunityCatalog::initialize(const std::string& userId) {
    std::lock_guard<std::mutex> lock(initializeMutex_);
    const auto key = std::make_pair(DatabaseEngine::getDataRoot(), userId);
    if (initializedRoots_.find(key) != initializedRoots_.end()) return;
    DatabaseEngine::ensureUserRoot(userId);
    if (!DatabaseEngine::createDatabase(userId, kDatabase)) {
        throw std::runtime_error("could not initialize community metadata database");
    }
    for (const char* collection : {kProjects, kDatabaseProjects,
                                   kMediaManifests, kMediaChunks,
                                   kRestoreJournal}) {
        if (DatabaseEngine::createCollection(userId, kDatabase, collection).empty()) {
            throw std::runtime_error("could not initialize community metadata collection");
        }
    }
    initializedRoots_.insert(key);
}

json CommunityCatalog::createProject(const std::string& userId,
                                     const std::string& name) {
    requireText(name, "project name");
    initialize(userId);
    json project{{"id", IDGenerator::generatePrefixedId("project")},
                 {"name", name},
                 {"created_at_ms", nowMs()}};
    DatabaseEngine::insert(userId, kDatabase, kProjects, project);
    return project;
}

json CommunityCatalog::listProjects(const std::string& userId) {
    initialize(userId);
    auto rows = DatabaseEngine::find(userId, kDatabase, kProjects, json::object());
    json result = json::array();
    for (auto& row : rows) result.push_back(publicDocument(std::move(row)));
    return result;
}

json CommunityCatalog::getProject(const std::string& userId,
                                  const std::string& id) {
    initialize(userId);
    auto rows = DatabaseEngine::find(userId, kDatabase, kProjects, {{"id", id}}, 1);
    return rows.empty() ? json() : publicDocument(std::move(rows.front()));
}

bool CommunityCatalog::deleteProject(const std::string& userId,
                                     const std::string& id) {
    initialize(userId);
    auto mappings = DatabaseEngine::find(userId, kDatabase, kDatabaseProjects,
                                         {{"project_id", id}});
    for (const auto& mapping : mappings) {
        DatabaseEngine::deleteOne(userId, kDatabase, kDatabaseProjects,
                                  {{"id", mapping.at("id")}});
    }
    return DatabaseEngine::deleteOne(userId, kDatabase, kProjects, {{"id", id}});
}

bool CommunityCatalog::mapDatabase(const std::string& userId,
                                   const std::string& projectId,
                                   const std::string& databaseName) {
    requireText(databaseName, "database name");
    if (getProject(userId, projectId).is_null()) {
        throw std::invalid_argument("project not found");
    }
    const std::string id = "database:" + databaseName;
    if (DatabaseEngine::deleteOne(userId, kDatabase, kDatabaseProjects,
                                  {{"id", id}})) {
        // The replacement below is the single current mapping.
    }
    DatabaseEngine::insert(userId, kDatabase, kDatabaseProjects,
                           {{"id", id},
                            {"project_id", projectId},
                            {"database", databaseName},
                            {"created_at_ms", nowMs()}});
    return true;
}

json CommunityCatalog::databaseProject(const std::string& userId,
                                       const std::string& databaseName) {
    initialize(userId);
    auto rows = DatabaseEngine::find(userId, kDatabase, kDatabaseProjects,
                                     {{"id", "database:" + databaseName}}, 1);
    return rows.empty() ? json() : publicDocument(std::move(rows.front()));
}

json CommunityCatalog::listProjectDatabases(const std::string& userId,
                                            const std::string& projectId) {
    if (getProject(userId, projectId).is_null()) {
        throw std::invalid_argument("project not found");
    }
    const auto existing = DatabaseEngine::listDatabases(userId);
    const auto mappings = DatabaseEngine::find(
        userId, kDatabase, kDatabaseProjects, {{"project_id", projectId}});
    json result = json::array();
    for (const auto& mapping : mappings) {
        const auto database = mapping.value("database", "");
        if (!database.empty() && !isReservedDatabase(database) &&
            std::find(existing.begin(), existing.end(), database) != existing.end()) {
            result.push_back(database);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

json CommunityCatalog::beginMedia(const std::string& userId,
                                  const std::string& databaseName,
                                  const std::string& collection,
                                  const std::string& filename,
                                  const std::string& contentType,
                                  long long sizeBytes,
                                  long long chunkCount,
                                  const std::string& sha256,
                                  const std::string& resumeId) {
    requireText(databaseName, "database name");
    requireText(collection, "collection");
    requireText(filename, "filename");
    requireText(contentType, "content type");
    requireSha256(sha256);
    if (sizeBytes < 0 || chunkCount <= 0) {
        throw std::invalid_argument("invalid media size or chunk count");
    }
    initialize(userId);
    if (!resumeId.empty()) {
        auto existing = getMedia(userId, resumeId);
        if (existing.is_null()) throw std::invalid_argument("media upload not found");
        if (existing.value("status", "") == "ready") return existing;
        if (existing.value("database", "") != databaseName ||
            existing.value("collection", "") != collection ||
            existing.value("filename", "") != filename ||
            existing.value("size_bytes", -1LL) != sizeBytes ||
            existing.value("chunk_count", -1LL) != chunkCount ||
            existing.value("sha256", "") != sha256) {
            throw std::invalid_argument("resume metadata does not match existing upload");
        }
        return existing;
    }
    json manifest{{"id", IDGenerator::generatePrefixedId("media")},
                  {"database", databaseName},
                  {"collection", collection},
                  {"filename", filename},
                  {"content_type", contentType},
                  {"size_bytes", sizeBytes},
                  {"chunk_count", chunkCount},
                  {"sha256", sha256},
                  {"status", "uploading"},
                  {"created", nowMs()}};
    DatabaseEngine::insert(userId, kDatabase, kMediaManifests, manifest);
    return manifest;
}

json CommunityCatalog::putMediaChunk(const std::string& userId,
                                     const std::string& mediaId,
                                     long long index,
                                     const std::string& dataBase64,
                                     long long sizeBytes,
                                     const std::string& sha256) {
    requireSha256(sha256);
    const auto manifest = getMedia(userId, mediaId);
    if (manifest.is_null()) throw std::invalid_argument("media upload not found");
    if (manifest.value("status", "") == "ready") {
        throw std::invalid_argument("media upload is already complete");
    }
    if (index < 0 || index >= manifest.value("chunk_count", 0LL)) {
        throw std::invalid_argument("media chunk index is out of range");
    }
    const auto decoded = decodeBase64(dataBase64);
    if (sizeBytes < 0 || static_cast<long long>(decoded.size()) != sizeBytes) {
        throw std::invalid_argument("media chunk size does not match payload");
    }
    if (sha256Hex(decoded) != sha256) {
        throw std::invalid_argument("media chunk checksum does not match payload");
    }
    const std::string id = chunkId(mediaId, index);
    auto existing = DatabaseEngine::find(userId, kDatabase, kMediaChunks,
                                         {{"id", id}}, 1);
    if (!existing.empty()) {
        if (existing.front().value("sha256", "") == sha256 &&
            existing.front().value("size_bytes", -1LL) == sizeBytes) {
            return {{"stored", true}, {"duplicate", true}, {"index", index}};
        }
        throw std::invalid_argument("media chunk conflicts with committed chunk");
    }
    DatabaseEngine::insert(userId, kDatabase, kMediaChunks,
                           {{"id", id},
                            {"media_id", mediaId},
                            {"index", index},
                            {"size_bytes", sizeBytes},
                            {"sha256", sha256},
                            {"data", dataBase64}});
    return {{"stored", true}, {"duplicate", false}, {"index", index}};
}

json CommunityCatalog::finalizeMedia(const std::string& userId,
                                     const std::string& mediaId) {
    auto manifest = getMedia(userId, mediaId);
    if (manifest.is_null()) throw std::invalid_argument("media upload not found");
    if (manifest.value("status", "") == "ready") return manifest;

    const long long expectedCount = manifest.value("chunk_count", 0LL);
    long long total = 0;
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (!raw) throw std::runtime_error("could not create media checksum context");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(raw, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("could not initialize media checksum");
    }
    for (long long index = 0; index < expectedCount; ++index) {
        auto chunk = getMediaChunk(userId, mediaId, index);
        if (chunk.is_null()) throw std::runtime_error("media upload has missing chunks");
        const auto bytes = decodeBase64(chunk.at("data"));
        if (sha256Hex(bytes) != chunk.value("sha256", "")) {
            throw std::runtime_error("media chunk checksum mismatch");
        }
        total += static_cast<long long>(bytes.size());
        if (EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error("could not update media checksum");
        }
    }
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int resultLength = 0;
    if (EVP_DigestFinal_ex(digest.get(), result, &resultLength) != 1) {
        throw std::runtime_error("could not finish media checksum");
    }
    std::ostringstream hash;
    hash << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < resultLength; ++i) hash << std::setw(2) << int(result[i]);
    if (total != manifest.value("size_bytes", -1LL) ||
        hash.str() != manifest.value("sha256", "")) {
        throw std::runtime_error("media checksum or size does not match manifest");
    }
    DatabaseEngine::updateOne(userId, kDatabase, kMediaManifests,
                              {{"id", mediaId}},
                              {{"$set", {{"status", "ready"},
                                          {"completed", nowMs()}}}});
    return getMedia(userId, mediaId);
}

json CommunityCatalog::listMedia(const std::string& userId,
                                 bool includeIncomplete,
                                 const std::string& databaseName,
                                 const std::string& collection) {
    initialize(userId);
    json filter = json::object();
    if (!includeIncomplete) filter["status"] = "ready";
    if (!databaseName.empty()) filter["database"] = databaseName;
    if (!collection.empty()) filter["collection"] = collection;
    auto rows = DatabaseEngine::find(userId, kDatabase, kMediaManifests, filter);
    json result = json::array();
    for (auto& row : rows) result.push_back(publicDocument(std::move(row)));
    return result;
}

json CommunityCatalog::getMedia(const std::string& userId,
                                const std::string& mediaId) {
    initialize(userId);
    auto rows = DatabaseEngine::find(userId, kDatabase, kMediaManifests,
                                     {{"id", mediaId}}, 1);
    return rows.empty() ? json() : publicDocument(std::move(rows.front()));
}

json CommunityCatalog::getMediaChunk(const std::string& userId,
                                     const std::string& mediaId,
                                     long long index) {
    initialize(userId);
    auto rows = DatabaseEngine::find(userId, kDatabase, kMediaChunks,
                                     {{"id", chunkId(mediaId, index)}}, 1);
    return rows.empty() ? json() : publicDocument(std::move(rows.front()));
}

bool CommunityCatalog::deleteMedia(const std::string& userId,
                                   const std::string& mediaId,
                                   bool allowReady) {
    const auto manifest = getMedia(userId, mediaId);
    if (manifest.is_null()) return false;
    if (!allowReady && manifest.value("status", "") == "ready") return false;
    const long long chunks = manifest.value("chunk_count", 0LL);
    for (long long index = 0; index < chunks; ++index) {
        DatabaseEngine::deleteOne(userId, kDatabase, kMediaChunks,
                                  {{"id", chunkId(mediaId, index)}});
    }
    return DatabaseEngine::deleteOne(userId, kDatabase, kMediaManifests,
                                     {{"id", mediaId}});
}

long long CommunityCatalog::cleanupMedia(const std::string& userId,
                                         const std::string& mediaId) {
    if (!mediaId.empty()) return deleteMedia(userId, mediaId, false) ? 1 : 0;
    const auto uploads = listMedia(userId, true);
    long long deleted = 0;
    for (const auto& upload : uploads) {
        if (upload.value("status", "") != "ready" &&
            deleteMedia(userId, upload.at("id"), false)) {
            ++deleted;
        }
    }
    return deleted;
}

json CommunityCatalog::recordRestore(const std::string& userId,
                                     const std::string& backupId,
                                     const std::string& targetDirectory,
                                     bool success,
                                     const std::string& error) {
    requireText(backupId, "backup id");
    initialize(userId);
    json record{{"id", IDGenerator::generatePrefixedId("restore")},
                {"backup_id", backupId},
                {"target_directory", targetDirectory},
                {"status", success ? "completed" : "failed"},
                {"error", error},
                {"completed", nowMs()}};
    DatabaseEngine::insert(userId, kDatabase, kRestoreJournal, record);
    return record;
}

json CommunityCatalog::listRestores(const std::string& userId) {
    initialize(userId);
    auto rows = DatabaseEngine::find(userId, kDatabase, kRestoreJournal,
                                     json::object());
    json result = json::array();
    for (auto& row : rows) result.push_back(publicDocument(std::move(row)));
    return result;
}

}  // namespace pacificdb::community
