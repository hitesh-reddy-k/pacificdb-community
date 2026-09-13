#include "community_catalog.hpp"

#include "database_engine.hpp"
#include "id_generator.hpp"
#include "media_upload_state.hpp"
#include "structured_event.hpp"
#include "test_failpoint.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
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

long long mediaLeaseDurationMs() {
    constexpr long long defaultLeaseMs = 24LL * 60 * 60 * 1000;
    const char* configured = std::getenv("PACIFICDB_MEDIA_UPLOAD_LEASE_MS");
    if (!configured || !*configured) return defaultLeaseMs;
    try {
        const long long duration = std::stoll(configured);
        if (duration <= 0) throw std::out_of_range("media lease");
        return duration;
    } catch (...) {
        throw std::invalid_argument(
            "PACIFICDB_MEDIA_UPLOAD_LEASE_MS must be a positive integer");
    }
}

json rawMedia(const std::string& userId, const std::string& mediaId) {
    auto rows = DatabaseEngine::find(
        userId, kDatabase, kMediaManifests, {{"id", mediaId}}, 1);
    return rows.empty() ? json() : publicDocument(std::move(rows.front()));
}

std::vector<json> rawMediaChunks(
    const std::string& userId,
    const std::string& mediaId) {
    auto rows = DatabaseEngine::find(
        userId, kDatabase, kMediaChunks, {{"media_id", mediaId}});
    std::vector<json> chunks;
    chunks.reserve(rows.size());
    for (auto& row : rows) chunks.push_back(publicDocument(std::move(row)));
    return chunks;
}

json progressFields(
    const json& manifest,
    const std::vector<json>& chunks,
    long long timestamp) {
    const auto progress = reconstructMediaProgress(manifest, chunks, timestamp);
    json fields{
        {"state_version", progress.stateVersion},
        {"received_chunks", progress.receivedChunks},
        {"received_bytes", progress.receivedBytes},
        {"next_chunk", progress.nextMissingIndex},
        {"received_indices", progress.receivedIndices},
    };
    if (progress.leaseExpiresAtMs > 0) {
        fields["lease_expires_at_ms"] = progress.leaseExpiresAtMs;
    }
    const auto state = parseMediaState(manifest.value("status", std::string()));
    fields["resumable"] = state == MediaState::uploading ||
                           state == MediaState::verifying;
    return fields;
}

json decorateMedia(
    json manifest,
    const std::vector<json>& chunks,
    long long timestamp) {
    manifest.update(progressFields(manifest, chunks, timestamp));
    return manifest;
}

void updateManifest(
    const std::string& userId,
    const std::string& mediaId,
    const json& fields) {
    if (!DatabaseEngine::updateOne(
            userId, kDatabase, kMediaManifests, {{"id", mediaId}},
            {{"$set", fields}})) {
        throw std::runtime_error("media manifest update was not durable");
    }
}

long long deleteOwnedChunks(
    const std::string& userId,
    const std::string& mediaId) {
    const auto chunks = rawMediaChunks(userId, mediaId);
    long long deleted = 0;
    for (const auto& chunk : chunks) {
        if (DatabaseEngine::deleteOne(
                userId, kDatabase, kMediaChunks, {{"id", chunk.at("id")}})) {
            ++deleted;
        }
    }
    return deleted;
}

void emitMediaEvent(
    const std::string& severity,
    const std::string& code,
    const std::string& mediaId,
    const std::string& message,
    const json& fields = json::object()) {
    pacificdb::observability::emitStructuredEvent(std::cerr, {
        severity, "media", code, mediaId, message, fields,
    });
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
    const auto key = std::make_pair(DatabaseEngine::getDataRoot(), userId);
    {
        std::lock_guard<std::mutex> lock(initializeMutex_);
        if (initializedRoots_.find(key) != initializedRoots_.end()) return;
        DatabaseEngine::ensureUserRoot(userId);
        if (!DatabaseEngine::createDatabase(userId, kDatabase)) {
            throw std::runtime_error(
                "could not initialize community metadata database");
        }
        for (const char* collection : {kProjects, kDatabaseProjects,
                                       kMediaManifests, kMediaChunks,
                                       kRestoreJournal}) {
            if (DatabaseEngine::createCollection(
                    userId, kDatabase, collection).empty()) {
                throw std::runtime_error(
                    "could not initialize community metadata collection");
            }
        }
        initializedRoots_.insert(key);
    }
    (void)reconcileMedia(userId);
}

std::mutex& CommunityCatalog::mediaMutex(
    const std::string& userId,
    const std::string& mediaId) {
    const auto hash = std::hash<std::string>{}(userId + "\n" + mediaId);
    return mediaMutexes_[hash % mediaMutexes_.size()];
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
    const long long timestamp = nowMs();
    if (!resumeId.empty()) {
        std::lock_guard<std::mutex> lock(mediaMutex(userId, resumeId));
        auto existing = rawMedia(userId, resumeId);
        if (existing.is_null()) throw std::invalid_argument("media upload not found");
        if (existing.value("status", "") == "ready") {
            return decorateMedia(
                std::move(existing), rawMediaChunks(userId, resumeId), timestamp);
        }
        const auto state = parseMediaState(existing.value("status", std::string()));
        if (state == MediaState::failed || state == MediaState::aborted) {
            throw std::invalid_argument("media upload is not resumable");
        }
        if (existing.value("database", "") != databaseName ||
            existing.value("collection", "") != collection ||
            existing.value("filename", "") != filename ||
            existing.value("content_type", "") != contentType ||
            existing.value("size_bytes", -1LL) != sizeBytes ||
            existing.value("chunk_count", -1LL) != chunkCount ||
            existing.value("sha256", "") != sha256) {
            throw std::invalid_argument("resume metadata does not match existing upload");
        }
        const long long lease = mediaLeaseDurationMs();
        if (lease > std::numeric_limits<long long>::max() - timestamp) {
            throw std::invalid_argument("media upload lease is too large");
        }
        existing["lease_expires_at_ms"] = timestamp + lease;
        const auto chunks = rawMediaChunks(userId, resumeId);
        auto fields = progressFields(existing, chunks, timestamp);
        fields["lease_expires_at_ms"] = timestamp + lease;
        updateManifest(userId, resumeId, fields);
        existing.update(fields);
        emitMediaEvent("info", "media_upload_resumed", resumeId,
                       "Media upload resumed",
                       {{"received_chunks", fields.at("received_chunks")},
                        {"next_chunk", fields.at("next_chunk")}});
        return existing;
    }
    const long long lease = mediaLeaseDurationMs();
    if (lease > std::numeric_limits<long long>::max() - timestamp) {
        throw std::invalid_argument("media upload lease is too large");
    }
    const std::string mediaId = IDGenerator::generatePrefixedId("media");
    std::lock_guard<std::mutex> lock(mediaMutex(userId, mediaId));
    json manifest{{"id", mediaId},
                  {"database", databaseName},
                  {"collection", collection},
                  {"filename", filename},
                  {"content_type", contentType},
                  {"size_bytes", sizeBytes},
                  {"chunk_count", chunkCount},
                  {"sha256", sha256},
                  {"status", "uploading"},
                  {"state_version", 2},
                  {"received_chunks", 0},
                  {"received_bytes", 0},
                  {"next_chunk", 0},
                  {"received_indices", json::array()},
                  {"lease_expires_at_ms", timestamp + lease},
                  {"resumable", true},
                  {"created", timestamp}};
    DatabaseEngine::insert(userId, kDatabase, kMediaManifests, manifest);
    pacificdb::test::hitFailpoint("FP_MEDIA_AFTER_MANIFEST", 1);
    emitMediaEvent("info", "media_upload_started", mediaId,
                   "Media upload started",
                   {{"chunk_count", chunkCount}, {"size_bytes", sizeBytes}});
    return manifest;
}

json CommunityCatalog::putMediaChunk(const std::string& userId,
                                     const std::string& mediaId,
                                     long long index,
                                     const std::string& dataBase64,
                                     long long sizeBytes,
                                     const std::string& sha256) {
    requireSha256(sha256);
    const auto decoded = decodeBase64(dataBase64);
    if (sizeBytes < 0 || static_cast<long long>(decoded.size()) != sizeBytes) {
        throw std::invalid_argument("media chunk size does not match payload");
    }
    if (sha256Hex(decoded) != sha256) {
        throw std::invalid_argument("media chunk checksum does not match payload");
    }
    initialize(userId);
    std::lock_guard<std::mutex> lock(mediaMutex(userId, mediaId));
    auto manifest = rawMedia(userId, mediaId);
    if (manifest.is_null()) throw std::invalid_argument("media upload not found");
    const auto state = parseMediaState(manifest.value("status", std::string()));
    if (state != MediaState::uploading) {
        throw std::invalid_argument("media upload is not accepting chunks");
    }
    if (index < 0 || index >= manifest.value("chunk_count", 0LL)) {
        throw std::invalid_argument("media chunk index is out of range");
    }
    const std::string id = chunkId(mediaId, index);
    auto existing = DatabaseEngine::find(userId, kDatabase, kMediaChunks,
                                         {{"id", id}}, 1);
    if (!existing.empty()) {
        if (existing.front().value("sha256", "") == sha256 &&
            existing.front().value("size_bytes", -1LL) == sizeBytes) {
            const auto timestamp = nowMs();
            const auto lease = mediaLeaseDurationMs();
            manifest["lease_expires_at_ms"] = timestamp + lease;
            auto fields = progressFields(
                manifest, rawMediaChunks(userId, mediaId), timestamp);
            fields["lease_expires_at_ms"] = timestamp + lease;
            updateManifest(userId, mediaId, fields);
            fields.update({{"stored", true}, {"duplicate", true},
                           {"index", index}, {"media_id", mediaId}});
            return fields;
        }
        updateManifest(userId, mediaId, {
            {"status", "failed"}, {"resumable", false},
            {"error_code", "media_chunk_conflict"},
            {"failed_at_ms", nowMs()},
        });
        (void)deleteOwnedChunks(userId, mediaId);
        emitMediaEvent("error", "media_chunk_conflict", mediaId,
                       "Media chunk conflicts with durable chunk metadata",
                       {{"index", index}});
        throw std::invalid_argument("media chunk conflicts with committed chunk");
    }
    DatabaseEngine::insert(userId, kDatabase, kMediaChunks,
                           {{"id", id},
                            {"media_id", mediaId},
                            {"index", index},
                            {"size_bytes", sizeBytes},
                            {"sha256", sha256},
                            {"data", dataBase64}});
    pacificdb::test::hitFailpoint("FP_MEDIA_AFTER_CHUNK", 1);
    const long long timestamp = nowMs();
    const long long lease = mediaLeaseDurationMs();
    manifest["lease_expires_at_ms"] = timestamp + lease;
    auto fields = progressFields(
        manifest, rawMediaChunks(userId, mediaId), timestamp);
    fields["lease_expires_at_ms"] = timestamp + lease;
    updateManifest(userId, mediaId, fields);
    emitMediaEvent("info", "media_chunk_committed", mediaId,
                   "Media chunk committed", {{"index", index}});
    fields.update({{"stored", true}, {"duplicate", false},
                   {"index", index}, {"media_id", mediaId}});
    return fields;
}

json CommunityCatalog::finalizeMedia(const std::string& userId,
                                     const std::string& mediaId) {
    initialize(userId);
    std::lock_guard<std::mutex> lock(mediaMutex(userId, mediaId));
    auto manifest = rawMedia(userId, mediaId);
    if (manifest.is_null()) throw std::invalid_argument("media upload not found");
    auto state = parseMediaState(manifest.value("status", std::string()));
    auto chunks = rawMediaChunks(userId, mediaId);
    if (state == MediaState::ready) {
        return decorateMedia(std::move(manifest), chunks, nowMs());
    }
    if (state == MediaState::failed || state == MediaState::aborted) {
        throw std::invalid_argument("media upload is not finalizable");
    }

    const long long timestamp = nowMs();
    auto progress = progressFields(manifest, chunks, timestamp);
    updateManifest(userId, mediaId, progress);
    if (progress.at("received_chunks").get<long long>() !=
        manifest.value("chunk_count", 0LL)) {
        manifest.update(progress);
        manifest["error"] = "media_chunks_missing";
        manifest["message"] = "media upload has missing chunks";
        manifest["resumable"] = true;
        return manifest;
    }

    if (!canTransition(state, MediaState::verifying)) {
        throw std::logic_error("illegal media verification transition");
    }
    updateManifest(userId, mediaId, {
        {"status", "verifying"}, {"verifying_at_ms", timestamp},
        {"resumable", true},
    });
    manifest["status"] = "verifying";
    pacificdb::test::hitFailpoint("FP_MEDIA_VERIFYING", 1);
    emitMediaEvent("info", "media_verification_started", mediaId,
                   "Media verification started");

    const long long expectedCount = manifest.value("chunk_count", 0LL);
    long long total = 0;
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (!raw) throw std::runtime_error("could not create media checksum context");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(raw, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("could not initialize media checksum");
    }
    try {
        for (long long index = 0; index < expectedCount; ++index) {
            const auto entry = std::find_if(
                chunks.begin(), chunks.end(), [index](const json& chunk) {
                    return chunk.value("index", -1LL) == index;
                });
            if (entry == chunks.end()) {
                throw std::runtime_error("media upload has missing chunks");
            }
            const auto bytes = decodeBase64(entry->at("data"));
            if (sha256Hex(bytes) != entry->value("sha256", "") ||
                static_cast<long long>(bytes.size()) !=
                    entry->value("size_bytes", -1LL)) {
                throw std::runtime_error("media chunk checksum mismatch");
            }
            if (static_cast<long long>(bytes.size()) >
                std::numeric_limits<long long>::max() - total) {
                throw std::runtime_error("media size overflow");
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
        for (unsigned int i = 0; i < resultLength; ++i) {
            hash << std::setw(2) << static_cast<unsigned int>(result[i]);
        }
        if (total != manifest.value("size_bytes", -1LL) ||
            hash.str() != manifest.value("sha256", "")) {
            throw std::runtime_error(
                "media checksum or size does not match manifest");
        }
    } catch (const std::exception& error) {
        updateManifest(userId, mediaId, {
            {"status", "failed"}, {"resumable", false},
            {"error_code", "media_verification_failed"},
            {"failed_at_ms", nowMs()},
        });
        (void)deleteOwnedChunks(userId, mediaId);
        emitMediaEvent("error", "media_verification_failed", mediaId,
                       "Media verification failed");
        throw;
    }
    if (!canTransition(MediaState::verifying, MediaState::ready)) {
        throw std::logic_error("illegal media ready transition");
    }
    progress["status"] = "ready";
    progress["resumable"] = false;
    progress["completed"] = nowMs();
    pacificdb::test::hitFailpoint("FP_MEDIA_BEFORE_READY", 1);
    updateManifest(userId, mediaId, progress);
    emitMediaEvent("info", "media_ready", mediaId,
                   "Media upload is ready",
                   {{"received_chunks", expectedCount}, {"size_bytes", total}});
    manifest.update(progress);
    return manifest;
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
    for (auto& row : rows) {
        auto manifest = publicDocument(std::move(row));
        const auto id = manifest.value("id", std::string());
        result.push_back(decorateMedia(
            std::move(manifest),
            rawMediaChunks(userId, id),
            nowMs()));
    }
    return result;
}

json CommunityCatalog::getMedia(const std::string& userId,
                                const std::string& mediaId) {
    initialize(userId);
    std::lock_guard<std::mutex> lock(mediaMutex(userId, mediaId));
    auto manifest = rawMedia(userId, mediaId);
    return manifest.is_null() ? json() : decorateMedia(
        std::move(manifest), rawMediaChunks(userId, mediaId), nowMs());
}

json CommunityCatalog::getMediaChunk(const std::string& userId,
                                     const std::string& mediaId,
                                     long long index) {
    initialize(userId);
    std::lock_guard<std::mutex> lock(mediaMutex(userId, mediaId));
    auto rows = DatabaseEngine::find(userId, kDatabase, kMediaChunks,
                                     {{"id", chunkId(mediaId, index)}}, 1);
    return rows.empty() ? json() : publicDocument(std::move(rows.front()));
}

bool CommunityCatalog::deleteMedia(const std::string& userId,
                                   const std::string& mediaId,
                                   bool allowReady) {
    initialize(userId);
    std::lock_guard<std::mutex> lock(mediaMutex(userId, mediaId));
    const auto manifest = rawMedia(userId, mediaId);
    if (manifest.is_null()) return false;
    if (!allowReady && manifest.value("status", "") == "ready") return false;
    (void)deleteOwnedChunks(userId, mediaId);
    return DatabaseEngine::deleteOne(userId, kDatabase, kMediaManifests,
                                     {{"id", mediaId}});
}

long long CommunityCatalog::cleanupMedia(const std::string& userId,
                                         const std::string& mediaId) {
    initialize(userId);
    if (mediaId.empty()) {
        const auto result = reconcileMedia(userId);
        return result.value("expired_uploads", 0LL) +
               result.value("removed_orphans", 0LL);
    }
    std::lock_guard<std::mutex> lock(mediaMutex(userId, mediaId));
    auto manifest = rawMedia(userId, mediaId);
    if (manifest.is_null()) return 0;
    const auto state = parseMediaState(manifest.value("status", std::string()));
    if (state == MediaState::ready || state == MediaState::aborted) return 0;
    if (!canTransition(state, MediaState::aborted)) {
        throw std::logic_error("illegal media cleanup transition");
    }
    (void)deleteOwnedChunks(userId, mediaId);
    updateManifest(userId, mediaId, {
        {"status", "aborted"}, {"resumable", false},
        {"received_chunks", 0}, {"received_bytes", 0},
        {"next_chunk", manifest.value("chunk_count", 0LL)},
        {"received_indices", json::array()}, {"aborted_at_ms", nowMs()},
    });
    emitMediaEvent("info", "media_upload_aborted", mediaId,
                   "Media upload aborted");
    return 1;
}

json CommunityCatalog::reconcileMedia(
    const std::string& userId,
    const std::string& mediaId) {
    initialize(userId);
    json result{
        {"repaired_progress", 0}, {"reset_verifying", 0},
        {"removed_orphans", 0}, {"expired_uploads", 0},
    };
    auto manifests = mediaId.empty()
        ? DatabaseEngine::find(
              userId, kDatabase, kMediaManifests, json::object())
        : DatabaseEngine::find(
              userId, kDatabase, kMediaManifests, {{"id", mediaId}}, 1);
    std::unordered_set<std::string> knownMedia;
    const long long timestamp = nowMs();
    for (auto& stored : manifests) {
        auto manifest = publicDocument(std::move(stored));
        const std::string id = manifest.value("id", std::string());
        if (id.empty()) continue;
        knownMedia.insert(id);
        std::lock_guard<std::mutex> lock(mediaMutex(userId, id));
        auto state = parseMediaState(manifest.value("status", std::string()));
        if (state == MediaState::verifying) {
            updateManifest(userId, id, {
                {"status", "uploading"}, {"resumable", true},
                {"reconciled_at_ms", timestamp},
            });
            manifest["status"] = "uploading";
            state = MediaState::uploading;
            result["reset_verifying"] =
                result.at("reset_verifying").get<long long>() + 1;
        }
        if (state != MediaState::ready && state != MediaState::aborted &&
            leaseExpired(manifest, timestamp)) {
            (void)deleteOwnedChunks(userId, id);
            updateManifest(userId, id, {
                {"status", "aborted"}, {"resumable", false},
                {"received_chunks", 0}, {"received_bytes", 0},
                {"next_chunk", manifest.value("chunk_count", 0LL)},
                {"received_indices", json::array()},
                {"error_code", "media_upload_expired"},
                {"aborted_at_ms", timestamp},
            });
            result["expired_uploads"] =
                result.at("expired_uploads").get<long long>() + 1;
            continue;
        }
        if (state == MediaState::failed || state == MediaState::aborted) {
            (void)deleteOwnedChunks(userId, id);
        }
        const auto chunks = rawMediaChunks(userId, id);
        const auto fields = progressFields(manifest, chunks, timestamp);
        bool differs = false;
        for (const char* key : {
                 "state_version", "received_chunks", "received_bytes",
                 "next_chunk", "received_indices", "resumable"}) {
            if (!manifest.contains(key) || manifest.at(key) != fields.at(key)) {
                differs = true;
                break;
            }
        }
        if (differs) {
            updateManifest(userId, id, fields);
            result["repaired_progress"] =
                result.at("repaired_progress").get<long long>() + 1;
        }
    }

    if (mediaId.empty()) {
        const auto chunks = DatabaseEngine::find(
            userId, kDatabase, kMediaChunks, json::object());
        for (const auto& chunk : chunks) {
            const std::string owner = chunk.value("media_id", std::string());
            if (!owner.empty() && knownMedia.find(owner) != knownMedia.end()) {
                continue;
            }
            if (DatabaseEngine::deleteOne(
                    userId, kDatabase, kMediaChunks, {{"id", chunk.at("id")}})) {
                result["removed_orphans"] =
                    result.at("removed_orphans").get<long long>() + 1;
            }
        }
    } else if (manifests.empty()) {
        result["removed_orphans"] = deleteOwnedChunks(userId, mediaId);
    }
    emitMediaEvent("info", "media_reconciled", mediaId,
                   "Media upload state reconciled", result);
    return result;
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
