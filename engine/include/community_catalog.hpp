#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace pacificdb::community {

using json = nlohmann::json;

bool isReservedDatabase(std::string_view name);

class CommunityCatalog {
public:
    static CommunityCatalog& instance();

    void initialize(const std::string& userId);
    json createProject(const std::string& userId, const std::string& name);
    json listProjects(const std::string& userId, long long limit = 100,
                      long long offset = 0);
    json getProject(const std::string& userId, const std::string& id);
    bool deleteProject(const std::string& userId, const std::string& id);
    bool mapDatabase(const std::string& userId, const std::string& projectId,
                     const std::string& databaseName);
    json listProjectDatabases(const std::string& userId,
                              const std::string& projectId);
    json databaseProject(const std::string& userId,
                         const std::string& databaseName);
    bool unmapDatabase(const std::string& userId,
                       const std::string& databaseName,
                       const std::string& projectId);
    json beginMedia(const std::string& userId, const std::string& databaseName,
                    const std::string& collection, const std::string& filename,
                    const std::string& contentType, long long sizeBytes,
                    long long chunkCount, const std::string& sha256,
                    const std::string& resumeId = {});
    json putMediaChunk(const std::string& userId, const std::string& mediaId,
                       long long index, const std::string& dataBase64,
                       long long sizeBytes, const std::string& sha256);
    json finalizeMedia(const std::string& userId, const std::string& mediaId);
    json listMedia(const std::string& userId, bool includeIncomplete,
                   const std::string& databaseName = {},
                   const std::string& collection = {},
                   long long limit = 100, long long offset = 0);
    json getMedia(const std::string& userId, const std::string& mediaId);
    json getMediaChunk(const std::string& userId, const std::string& mediaId,
                       long long index);
    bool deleteMedia(const std::string& userId, const std::string& mediaId,
                     bool allowReady = true);
    long long cleanupMedia(const std::string& userId,
                           const std::string& mediaId = {});
    json reconcileMedia(const std::string& userId,
                        const std::string& mediaId = {});
    json recordRestore(const std::string& userId, const std::string& backupId,
                       const std::string& targetDirectory, bool success,
                       const std::string& error);
    json listRestores(const std::string& userId);

private:
    std::mutex initializeMutex_;
    std::mutex mappingMutex_;
    std::set<std::pair<std::string, std::string>> initializedRoots_;
    std::array<std::mutex, 64> mediaMutexes_;

    std::mutex& mediaMutex(const std::string& userId,
                           const std::string& mediaId);
};

}  // namespace pacificdb::community
