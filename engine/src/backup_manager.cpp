#include "backup_manager.hpp"
#include "data_durability.hpp"
#include "raft_core.hpp"
#include "snapshot_bundle.hpp"
#include "lsm.hpp"
#include "storage_path.hpp"
#include "storage_root_guard.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

#ifndef PACIFICDB_STORAGE_FORMAT_VERSION
#define PACIFICDB_STORAGE_FORMAT_VERSION "unknown"
#endif

namespace {
std::string absoluteDirectory(const std::string& value, const char* name) {
    fs::path path(value);
    if (value.empty() || !path.is_absolute())
        throw std::invalid_argument(std::string(name) + " must be absolute");
    fs::create_directories(path);
    return fs::canonical(path).string();
}

bool beneath(const fs::path& child, const fs::path& parent) {
    auto c = child.lexically_normal();
    auto p = parent.lexically_normal();
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci)
        if (ci == c.end() || *ci != *pi) return false;
    return true;
}
}

json BackupInfo::toJson() const {
    return {
        {"backup_id", backupId},
        {"status", static_cast<int>(status)},
        {"start_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
            startTime.time_since_epoch()).count()},
        {"end_time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime.time_since_epoch()).count()},
        {"base_path", basePath},
        {"size_bytes", sizeBytes},
        {"documents_count", documentsCount},
        {"documents_count_known", documentsCountKnown},
        {"snapshot_index", snapshotIndex},
        {"checksum", checksum},
        {"error", error},
    };
}

BackupInfo BackupInfo::fromJson(const json& value) {
    BackupInfo info;
    info.backupId = value.value("backup_id", "");
    info.status = static_cast<BackupStatus>(value.value("status", 0));
    info.startTime = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(value.value("start_time_ms", int64_t{0})));
    info.endTime = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(value.value("end_time_ms", int64_t{0})));
    info.basePath = value.value("base_path", "");
    info.sizeBytes = value.value("size_bytes", size_t{0});
    info.documentsCount = value.value("documents_count", size_t{0});
    info.documentsCountKnown = value.value("documents_count_known", false);
    info.snapshotIndex = value.value("snapshot_index", uint64_t{0});
    info.checksum = value.value("checksum", "");
    info.error = value.value("error", "");
    return info;
}

BackupManager& BackupManager::instance() {
    static BackupManager manager;
    return manager;
}

void BackupManager::init(const std::string& dataDir,
                         const std::string& backupDir,
                         const std::string& restoreDir) {
    dataDir_ = absoluteDirectory(dataDir, "DATA_ROOT");
    backupDir_ = absoluteDirectory(backupDir, "BACKUP_ROOT");
    restoreDir_ = absoluteDirectory(restoreDir, "RESTORE_DIR");
    loadBackupCatalog();
}

fs::path BackupManager::backupPathForId(const std::string& backupId) const {
    const auto id = validateStorageIdentifier(backupId, "backupId");
    return validateContainedStoragePath(backupDir_, fs::path(backupDir_) / id);
}

fs::path BackupManager::restoreTargetForRequest(const std::string& targetDir) const {
    fs::path target(targetDir);
    if (!target.is_absolute())
        throw std::invalid_argument("restore target must be absolute");
    target = target.lexically_normal();
    validateStorageIdentifier(target.filename().string(), "restoreIdentifier");
    const fs::path expected = fs::path(restoreDir_) / target.filename();
    if (target != expected)
        throw std::invalid_argument("restore target must be directly beneath RESTORE_DIR");
    return validateContainedStoragePath(restoreDir_, target);
}

std::string BackupManager::generateBackupId() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm stamp{};
#ifdef _WIN32
    localtime_s(&stamp, &time);
#else
    localtime_r(&time, &stamp);
#endif
    std::random_device random;
    std::ostringstream out;
    out << "backup-" << std::put_time(&stamp, "%Y%m%d-%H%M%S")
        << '-' << std::hex << random();
    return out.str();
}

bool BackupManager::copyDirectory(const std::string& src, const std::string& dst) {
    try {
        const fs::path source = fs::canonical(src);
        const fs::path destination = fs::path(dst).lexically_normal();
        fs::create_directories(destination);
        for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
            const fs::path path = it->path();
            if (beneath(path, backupDir_) || beneath(path, restoreDir_)) {
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (it->is_symlink()) throw std::runtime_error("backup source contains symlink");
            const fs::path target = destination / fs::relative(path, source);
            if (it->is_directory()) fs::create_directories(target);
            else if (it->is_regular_file())
                fs::copy_file(path, target, fs::copy_options::overwrite_existing);
        }
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[BackupManager] copy failed: " << error.what() << '\n';
        return false;
    }
}

std::string BackupManager::calculateChecksum(const std::string& path) {
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](unsigned char value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(path))
        if (entry.is_regular_file() && entry.path().filename() != "backup.json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::array<char, 64 * 1024> buffer{};
    for (const auto& file : files) {
        const auto relative = fs::relative(file, path).generic_string();
        for (unsigned char value : relative) mix(value);
        mix(0);
        std::ifstream input(file, std::ios::binary);
        while (input) {
            input.read(buffer.data(), buffer.size());
            for (std::streamsize i = 0; i < input.gcount(); ++i)
                mix(static_cast<unsigned char>(buffer[static_cast<size_t>(i)]));
        }
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

std::string BackupManager::createFullBackup(
    const std::string& description,
    std::optional<size_t> authoritativeDocumentsCount) {
    if (RaftCore::instance().isEnabled() &&
        (!RaftCore::instance().strongReadsAllowed() || !LSM::indexRecoverySettled()))
        return "";
    std::lock_guard<std::mutex> lock(backupMutex_);
    BackupInfo info;
    info.backupId = generateBackupId();
    info.status = BackupStatus::IN_PROGRESS;
    info.startTime = std::chrono::system_clock::now();
    info.basePath = backupPathForId(info.backupId).string();
    if (authoritativeDocumentsCount) {
        info.documentsCount = *authoritativeDocumentsCount;
        info.documentsCountKnown = true;
    }
    try {
        if (RaftCore::instance().isEnabled()) {
            info.snapshotIndex = RaftCore::instance().createBackupSnapshot();
            if (info.snapshotIndex == 0)
                throw std::runtime_error("cannot capture committed snapshot");
        }
        fs::create_directories(info.basePath);
        if (!copyDirectory(dataDir_, info.basePath + "/data"))
            throw std::runtime_error("cannot copy data");
        for (const auto& entry : fs::recursive_directory_iterator(info.basePath))
            if (entry.is_regular_file()) info.sizeBytes += entry.file_size();
        info.checksum = calculateChecksum(info.basePath);
        info.status = BackupStatus::COMPLETED;
        info.endTime = std::chrono::system_clock::now();
        json metadata = info.toJson();
        metadata["description"] = description;
        std::ofstream(info.basePath + "/backup.json") << metadata.dump(2);
        backups_[info.backupId] = info;
        persistBackupCatalog();
        return info.backupId;
    } catch (const std::exception& error) {
        fs::remove_all(info.basePath);
        info.status = BackupStatus::FAILED;
        info.error = error.what();
        info.endTime = std::chrono::system_clock::now();
        backups_[info.backupId] = info;
        persistBackupCatalog();
        return "";
    }
}

BackupInfo BackupManager::getBackupInfo(const std::string& backupId) const {
    validateStorageIdentifier(backupId, "backupId");
    std::lock_guard<std::mutex> lock(backupMutex_);
    auto found = backups_.find(backupId);
    return found == backups_.end() ? BackupInfo{} : found->second;
}

std::vector<BackupInfo> BackupManager::listBackups() const {
    std::lock_guard<std::mutex> lock(backupMutex_);
    std::vector<BackupInfo> result;
    for (const auto& item : backups_) result.push_back(item.second);
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.startTime > b.startTime; });
    return result;
}

bool BackupManager::deleteBackup(const std::string& backupId) {
    validateStorageIdentifier(backupId, "backupId");
    std::lock_guard<std::mutex> lock(backupMutex_);
    auto found = backups_.find(backupId);
    if (found == backups_.end()) return false;
    fs::remove_all(backupPathForId(backupId));
    backups_.erase(found);
    persistBackupCatalog();
    return true;
}

bool BackupManager::verifyBackup(const std::string& backupId) {
    validateStorageIdentifier(backupId, "backupId");
    std::lock_guard<std::mutex> lock(backupMutex_);
    auto found = backups_.find(backupId);
    if (found == backups_.end()) return false;
    if (calculateChecksum(backupPathForId(backupId).string()) != found->second.checksum)
        return false;
    found->second.status = BackupStatus::VERIFIED;
    persistBackupCatalog();
    return true;
}

bool BackupManager::restoreFromBackup(
    const std::string& backupId, const std::string& targetDir,
    const std::string& targetClusterId, const std::string& targetNodeId) {
    validateStorageIdentifier(backupId, "backupId");
    std::lock_guard<std::mutex> lock(backupMutex_);
    auto found = backups_.find(backupId);
    if (found == backups_.end() ||
        calculateChecksum(found->second.basePath) != found->second.checksum)
        return false;
    const fs::path target = restoreTargetForRequest(targetDir);
    if (fs::exists(target) && !fs::is_empty(target)) return false;
    try {
        fs::create_directories(target);
        const std::string restoreId = target.filename().string();
        auto guard = StorageRootGuard::acquire(
            target,
            validateStorageIdentifier(targetClusterId.empty() ? "restored-" + restoreId : targetClusterId,
                                      "targetClusterId"),
            validateStorageIdentifier(targetNodeId.empty() ? "restored-" + restoreId : targetNodeId,
                                      "targetNodeId"),
            PACIFICDB_STORAGE_FORMAT_VERSION);
        const std::set<std::string> local = {
            ".pacificdb-root-identity.json", ".pacificdb-root.lock", "raft"
        };
        for (const auto& entry : fs::directory_iterator(
                 backupPathForId(backupId) / "data")) {
            if (local.count(entry.path().filename().string())) continue;
            fs::copy(entry.path(), target / entry.path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        return true;
    } catch (...) {
        fs::remove_all(target);
        return false;
    }
}

void BackupManager::persistBackupCatalog() {
    json catalog = json::array();
    for (const auto& item : backups_) catalog.push_back(item.second.toJson());
    const fs::path temp = fs::path(backupDir_) / "catalog.json.tmp";
    std::ofstream(temp) << catalog.dump(2);
    fs::rename(temp, fs::path(backupDir_) / "catalog.json");
}

void BackupManager::loadBackupCatalog() {
    std::ifstream input(fs::path(backupDir_) / "catalog.json");
    if (!input) return;
    json catalog = json::parse(input, nullptr, false);
    if (!catalog.is_array()) return;
    for (const auto& value : catalog) {
        auto info = BackupInfo::fromJson(value);
        try {
            info.basePath = backupPathForId(info.backupId).string();
            backups_[info.backupId] = info;
        } catch (...) {}
    }
}
