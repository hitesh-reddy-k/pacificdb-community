#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

enum class BackupStatus { PENDING, IN_PROGRESS, COMPLETED, FAILED, VERIFIED };

struct BackupInfo {
    std::string backupId;
    BackupStatus status{BackupStatus::PENDING};
    std::chrono::system_clock::time_point startTime{};
    std::chrono::system_clock::time_point endTime{};
    std::string basePath;
    size_t sizeBytes{0};
    size_t documentsCount{0};
    bool documentsCountKnown{false};
    uint64_t snapshotIndex{0};
    std::string checksum;
    std::string error;

    json toJson() const;
    static BackupInfo fromJson(const json& value);
};

class BackupManager {
public:
    static BackupManager& instance();
    void init(const std::string& dataDir, const std::string& backupDir,
              const std::string& restoreDir);
    std::string createFullBackup(
        const std::string& description = "",
        std::optional<size_t> authoritativeDocumentsCount = std::nullopt);
    BackupInfo getBackupInfo(const std::string& backupId) const;
    std::vector<BackupInfo> listBackups() const;
    bool deleteBackup(const std::string& backupId);
    bool verifyBackup(const std::string& backupId);
    bool restoreFromBackup(const std::string& backupId,
                           const std::string& targetDir,
                           const std::string& targetClusterId = "",
                           const std::string& targetNodeId = "");

private:
    BackupManager() = default;
    std::string dataDir_;
    std::string backupDir_;
    std::string restoreDir_;
    std::map<std::string, BackupInfo> backups_;
    mutable std::mutex backupMutex_;

    std::string generateBackupId();
    std::filesystem::path backupPathForId(const std::string& backupId) const;
    std::filesystem::path restoreTargetForRequest(const std::string& targetDir) const;
    std::string calculateChecksum(const std::string& path);
    bool copyDirectory(const std::string& src, const std::string& dst);
    void persistBackupCatalog();
    void loadBackupCatalog();
};
