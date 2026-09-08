#include "lsm_manifest.hpp"

#include "storage_format_v2.hpp"
#include "test_failpoint.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#undef ERROR
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kManifestName = "_manifest.json";
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uintmax_t kMaxManifestBytes = 16 * 1024 * 1024;

std::mutex g_publishLocksMutex;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> g_publishLocks;

std::shared_ptr<std::mutex> publishLock(const fs::path& directory) {
    std::lock_guard<std::mutex> lock(g_publishLocksMutex);
    auto& result = g_publishLocks[fs::absolute(directory).lexically_normal().string()];
    if (!result) result = std::make_shared<std::mutex>();
    return result;
}

void setError(std::string* error, const std::string& value) {
    if (error) *error = value;
}

bool safeSstName(const std::string& name) {
    const fs::path path(name);
    return !name.empty() && !path.is_absolute() && path == path.filename() &&
           path.extension() == ".sst" && name != ".sst";
}

bool syncPath(const fs::path& path, bool directory = false) {
#ifdef _WIN32
    const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    const int flags = directory ? (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                                : (O_RDONLY | O_CLOEXEC);
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) return false;
    const bool ok = directory ? (::fsync(fd) == 0) : (::fdatasync(fd) == 0);
    ::close(fd);
    return ok;
#endif
}

bool replaceFile(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
    return MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

bool validateManifest(const fs::path& directory, LsmManifest& manifest,
                      bool verifyContents, std::string* error) {
    if (manifest.formatVersion != kFormatVersion || manifest.generation == 0) {
        setError(error, "unsupported LSM manifest version or generation");
        return false;
    }
    std::sort(manifest.sstFiles.begin(), manifest.sstFiles.end());
    if (std::adjacent_find(manifest.sstFiles.begin(), manifest.sstFiles.end()) !=
        manifest.sstFiles.end()) {
        setError(error, "duplicate SST in LSM manifest");
        return false;
    }
    for (const auto& name : manifest.sstFiles) {
        if (!safeSstName(name)) {
            setError(error, "unsafe SST name in LSM manifest");
            return false;
        }
        std::error_code ec;
        if (!fs::is_regular_file(directory / name, ec) || ec) {
            setError(error, "missing manifest SST " + name);
            return false;
        }
        if (verifyContents) {
            std::string verifyError;
            if (!pacificdb::storage_v2::verifySst(directory / name, &verifyError)) {
                setError(error, "invalid manifest SST " + name + ": " + verifyError);
                return false;
            }
        }
    }
    return true;
}

}  // namespace

LsmManifestLoadResult LsmManifestStore::load(const fs::path& lsmDirectory) {
    LsmManifestLoadResult result;
    const fs::path path = lsmDirectory / kManifestName;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        if (ec) {
            result.status = LsmManifestLoadStatus::ERROR;
            result.error = "cannot inspect LSM manifest: " + ec.message();
        }
        return result;
    }
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxManifestBytes) {
        result.status = LsmManifestLoadStatus::ERROR;
        result.error = "invalid LSM manifest size";
        return result;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open LSM manifest");
        const json persisted = json::parse(input);
        if (!persisted.is_object() || !persisted.contains("formatVersion") ||
            !persisted.contains("generation") || !persisted.contains("coveredWalLsn") ||
            !persisted.contains("sstFiles") || !persisted["sstFiles"].is_array()) {
            throw std::runtime_error("missing LSM manifest fields");
        }
        result.manifest.formatVersion = persisted["formatVersion"].get<std::uint32_t>();
        result.manifest.generation = persisted["generation"].get<std::uint64_t>();
        result.manifest.coveredWalLsn = persisted["coveredWalLsn"].get<std::uint64_t>();
        result.manifest.sstFiles = persisted["sstFiles"].get<std::vector<std::string>>();
        if (!validateManifest(lsmDirectory, result.manifest, false, &result.error)) {
            result.status = LsmManifestLoadStatus::ERROR;
            return result;
        }
        result.status = LsmManifestLoadStatus::OK;
        return result;
    } catch (const std::exception& exception) {
        result.status = LsmManifestLoadStatus::ERROR;
        result.error = exception.what();
        return result;
    }
}

bool LsmManifestStore::publishedSsts(const fs::path& lsmDirectory,
                                     std::vector<fs::path>& output,
                                     std::string* error) {
    output.clear();
    const auto loaded = load(lsmDirectory);
    if (loaded.status == LsmManifestLoadStatus::ERROR) {
        setError(error, loaded.error);
        return false;
    }
    if (loaded.status == LsmManifestLoadStatus::OK) {
        for (const auto& name : loaded.manifest.sstFiles) output.push_back(lsmDirectory / name);
        return true;
    }

    std::error_code ec;
    fs::directory_iterator iterator(lsmDirectory, ec), end;
    while (!ec && iterator != end) {
        std::error_code typeError;
        const bool regular = iterator->is_regular_file(typeError);
        if (typeError) {
            ec = typeError;
            break;
        }
        if (regular && iterator->path().extension() == ".sst") {
            std::string verifyError;
            if (!pacificdb::storage_v2::verifySst(iterator->path(), &verifyError)) {
                setError(error, "invalid legacy SST " + iterator->path().filename().string() +
                                ": " + verifyError);
                output.clear();
                return false;
            }
            output.push_back(iterator->path());
        }
        iterator.increment(ec);
    }
    if (ec) {
        setError(error, "cannot enumerate legacy SSTs: " + ec.message());
        output.clear();
        return false;
    }
    std::sort(output.begin(), output.end());
    return true;
}

bool LsmManifestStore::publish(const fs::path& lsmDirectory,
                               const LsmManifest& requested,
                               const std::vector<fs::path>& newArtifacts,
                               std::string* error) {
    auto collectionPublishLock = publishLock(lsmDirectory);
    std::lock_guard<std::mutex> publication(*collectionPublishLock);
    LsmManifest manifest = requested;
    const auto current = load(lsmDirectory);
    if (current.status == LsmManifestLoadStatus::ERROR) {
        setError(error, current.error);
        return false;
    }
    if (!validateManifest(lsmDirectory, manifest,
                          current.status == LsmManifestLoadStatus::ABSENT, error)) {
        return false;
    }
    if (current.status == LsmManifestLoadStatus::OK &&
        (manifest.generation <= current.manifest.generation ||
         manifest.coveredWalLsn < current.manifest.coveredWalLsn)) {
        setError(error, "LSM manifest generation or WAL boundary regressed");
        return false;
    }

    const fs::path normalizedDirectory = fs::absolute(lsmDirectory).lexically_normal();
    std::unordered_set<std::string> newSsts;
    for (const auto& artifact : newArtifacts) {
        const fs::path normalizedArtifact = fs::absolute(artifact).lexically_normal();
        if (normalizedArtifact.parent_path() != normalizedDirectory) {
            setError(error, "new LSM artifact is outside the collection directory");
            return false;
        }
        if (normalizedArtifact.extension() == ".sst") {
            newSsts.insert(normalizedArtifact.filename().string());
        }
    }
    if (current.status == LsmManifestLoadStatus::OK) {
        const std::unordered_set<std::string> published(
            current.manifest.sstFiles.begin(), current.manifest.sstFiles.end());
        for (const auto& name : manifest.sstFiles) {
            if (!published.count(name) && !newSsts.count(name)) {
                setError(error, "manifest adds an SST that is not a new artifact");
                return false;
            }
        }
    }
    for (const auto& artifact : newArtifacts) {
        const fs::path normalizedArtifact = fs::absolute(artifact).lexically_normal();
        std::error_code ec;
        if (!fs::is_regular_file(normalizedArtifact, ec) || ec || !syncPath(normalizedArtifact)) {
            setError(error, "cannot synchronize new LSM artifact");
            return false;
        }
        if (normalizedArtifact.extension() == ".sst") {
            if (current.status == LsmManifestLoadStatus::OK) {
                std::string verifyError;
                if (!pacificdb::storage_v2::verifySst(normalizedArtifact, &verifyError)) {
                    setError(error, "invalid new SST " +
                                    normalizedArtifact.filename().string() + ": " + verifyError);
                    return false;
                }
            }
            for (const char* suffix : {".bloom", ".sidx"}) {
                const fs::path sidecar = normalizedArtifact.string() + suffix;
                const bool exists = fs::exists(sidecar, ec);
                if (ec || (exists && (!fs::is_regular_file(sidecar, ec) ||
                                      ec || !syncPath(sidecar)))) {
                    setError(error, "cannot synchronize new LSM sidecar");
                    return false;
                }
            }
        }
    }
    if (!syncPath(lsmDirectory, true)) {
        setError(error, "cannot synchronize LSM artifacts directory");
        return false;
    }

    const json persisted = {
        {"formatVersion", manifest.formatVersion},
        {"generation", manifest.generation},
        {"coveredWalLsn", manifest.coveredWalLsn},
        {"sstFiles", manifest.sstFiles}
    };
    const fs::path temporary = lsmDirectory / (std::string(kManifestName) + ".tmp");
    const fs::path destination = lsmDirectory / kManifestName;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << persisted.dump();
        output.flush();
        if (!output) {
            setError(error, "cannot write temporary LSM manifest");
            return false;
        }
    }
    if (!syncPath(temporary)) {
        setError(error, "cannot synchronize temporary LSM manifest");
        return false;
    }
    pacificdb::test::hitFailpoint(
        "FP_LSM_CHECKPOINT_AFTER_MANIFEST_SYNC", manifest.coveredWalLsn);
    if (!replaceFile(temporary, destination)) {
        setError(error, "cannot rename LSM manifest");
        return false;
    }
    pacificdb::test::hitFailpoint(
        "FP_LSM_CHECKPOINT_AFTER_MANIFEST_RENAME", manifest.coveredWalLsn);
    if (!syncPath(lsmDirectory, true)) {
        setError(error, "cannot synchronize published LSM manifest");
        return false;
    }
    return true;
}
