#include "storage_root_guard.hpp"

#include "storage_path.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char* kIdentityFile = ".pacificdb-root-identity.json";
constexpr const char* kLockFile = ".pacificdb-root.lock";

std::uint64_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

json ownerJson(
    const fs::path& canonicalRoot,
    const std::string& clusterId,
    const std::string& nodeId,
    const std::string& instanceId) {
    return {
        {"schema", "pacificdb.storage-root-owner.v1"},
        {"version", 1},
        {"pid", currentProcessId()},
        {"cluster_id", clusterId},
        {"node_id", nodeId},
        {"canonical_root", canonicalRoot.u8string()},
        {"instance_id", instanceId},
    };
}

#ifdef _WIN32
void writeOwner(HANDLE file, const std::string& payload) {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(file)) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "truncate DATA_ROOT owner metadata");
    }
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto remaining = std::min<std::size_t>(
            payload.size() - offset,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                file, payload.data() + offset,
                static_cast<DWORD>(remaining), &written, nullptr) ||
            written == 0) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "write DATA_ROOT owner metadata");
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "flush DATA_ROOT owner metadata");
    }
}

std::string readOwnerPayload(const fs::path& path) {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string payload;
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            CloseHandle(file);
            return {};
        }
        if (read == 0) break;
        payload.append(buffer, read);
        if (payload.size() > 64 * 1024) {
            CloseHandle(file);
            return {};
        }
    }
    CloseHandle(file);
    return payload;
}
#else
void writeOwner(int descriptor, const std::string& payload) {
    if (::ftruncate(descriptor, 0) != 0 ||
        ::lseek(descriptor, 0, SEEK_SET) < 0) {
        throw std::system_error(
            errno, std::system_category(),
            "truncate DATA_ROOT owner metadata");
    }
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto written = ::write(
            descriptor, payload.data() + offset, payload.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            throw std::system_error(
                errno, std::system_category(),
                "write DATA_ROOT owner metadata");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
        throw std::system_error(
            errno, std::system_category(),
            "flush DATA_ROOT owner metadata");
    }
}

std::string readOwnerPayload(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return {};
    std::string payload;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || payload.size() + static_cast<std::size_t>(count) >
                64 * 1024) {
            ::close(descriptor);
            return {};
        }
        if (count == 0) break;
        payload.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    return payload;
}
#endif

std::string utcNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

void fsyncDirectory(const fs::path& directory) {
#ifndef _WIN32
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(
            "cannot open DATA_ROOT for directory fsync: " +
            std::string(std::strerror(errno)));
    }
    if (::fsync(fd) != 0) {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("cannot fsync DATA_ROOT: " + reason);
    }
    ::close(fd);
#else
    (void)directory;
#endif
}

void atomicWriteIdentity(const fs::path& identityPath, const json& identity) {
    fs::path temporary = identityPath;
    temporary += ".tmp";
#ifdef _WIN32
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create root identity temporary file");
        out << identity.dump(2) << "\n";
        out.flush();
        if (!out) throw std::runtime_error("cannot flush root identity temporary file");
    }
    std::error_code ec;
    fs::rename(temporary, identityPath, ec);
    if (ec) {
        fs::remove(temporary);
        throw std::runtime_error("cannot publish root identity: " + ec.message());
    }
#else
    const int fd = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::runtime_error(
            "cannot create root identity temporary file: " +
            std::string(std::strerror(errno)));
    }
    const std::string payload = identity.dump(2) + "\n";
    size_t offset = 0;
    while (offset < payload.size()) {
        const ssize_t written =
            ::write(fd, payload.data() + offset, payload.size() - offset);
        if (written < 0) {
            const std::string reason = std::strerror(errno);
            ::close(fd);
            ::unlink(temporary.c_str());
            throw std::runtime_error("cannot write root identity: " + reason);
        }
        offset += static_cast<size_t>(written);
    }
    if (::fsync(fd) != 0) {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        ::unlink(temporary.c_str());
        throw std::runtime_error("cannot fsync root identity: " + reason);
    }
    ::close(fd);
    if (::rename(temporary.c_str(), identityPath.c_str()) != 0) {
        const std::string reason = std::strerror(errno);
        ::unlink(temporary.c_str());
        throw std::runtime_error("cannot publish root identity: " + reason);
    }
    fsyncDirectory(identityPath.parent_path());
#endif
}

bool hasUnidentifiedContent(const fs::path& root) {
    for (const auto& entry : fs::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (name == kLockFile) continue;
        return true;
    }
    return false;
}

json loadIdentity(const fs::path& identityPath) {
    std::ifstream input(identityPath);
    if (!input) throw std::runtime_error("cannot open DATA_ROOT identity metadata");
    json identity = json::parse(input, nullptr, false);
    if (identity.is_discarded() || !identity.is_object()) {
        throw std::runtime_error("DATA_ROOT identity metadata is malformed");
    }
    return identity;
}

void requireIdentityField(
    const json& identity,
    const char* field,
    const std::string& expected) {
    if (!identity.contains(field) || !identity[field].is_string()) {
        throw std::runtime_error(
            std::string("DATA_ROOT identity is missing ") + field);
    }
    const std::string actual = identity[field].get<std::string>();
    if (actual != expected) {
        throw std::runtime_error(
            std::string("DATA_ROOT identity mismatch for ") + field +
            ": expected=" + expected + " actual=" + actual);
    }
}

}  // namespace

struct StorageRootGuard::State {
    fs::path canonicalRoot;
    fs::path identityPath;
    fs::path lockPath;
#ifdef _WIN32
    HANDLE lockHandle = INVALID_HANDLE_VALUE;
#else
    int lockFd = -1;
#endif

    ~State() {
#ifdef _WIN32
        if (lockHandle != INVALID_HANDLE_VALUE) CloseHandle(lockHandle);
#else
        if (lockFd >= 0) {
            ::flock(lockFd, LOCK_UN);
            ::close(lockFd);
        }
#endif
    }
};

StorageRootGuard::StorageRootGuard() = default;
StorageRootGuard::~StorageRootGuard() = default;
StorageRootGuard::StorageRootGuard(StorageRootGuard&&) noexcept = default;
StorageRootGuard& StorageRootGuard::operator=(StorageRootGuard&&) noexcept = default;

StorageRootGuard::StorageRootGuard(std::unique_ptr<State> state)
    : state_(std::move(state)) {}

StorageRootGuard StorageRootGuard::acquire(
    const fs::path& configuredRoot,
    const std::string& clusterId,
    const std::string& nodeId,
    const std::string& storageFormatVersion,
    const std::string& instanceId) {
    if (configuredRoot.empty() || !configuredRoot.is_absolute()) {
        throw std::runtime_error("DATA_ROOT must be an explicit absolute path");
    }
    validateStorageIdentifier(clusterId, "clusterId");
    validateStorageIdentifier(nodeId, "nodeId");
    validateStorageIdentifier(storageFormatVersion, "storageFormatVersion");
    if (!instanceId.empty()) {
        validateStorageIdentifier(instanceId, "instanceId");
    }

    std::error_code ec;
    fs::create_directories(configuredRoot, ec);
    if (ec) {
        throw std::runtime_error("cannot create DATA_ROOT: " + ec.message());
    }
    const fs::path canonicalRoot = fs::canonical(configuredRoot, ec);
    if (ec || canonicalRoot.empty() || !canonicalRoot.is_absolute()) {
        throw std::runtime_error("cannot canonicalize DATA_ROOT");
    }

    auto state = std::make_unique<State>();
    state->canonicalRoot = canonicalRoot;
    state->identityPath =
        validateContainedStoragePath(canonicalRoot, canonicalRoot / kIdentityFile);
    state->lockPath =
        validateContainedStoragePath(canonicalRoot, canonicalRoot / kLockFile);

#ifdef _WIN32
    state->lockHandle = CreateFileW(
        state->lockPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (state->lockHandle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            "DATA_ROOT is locked by another PacificDB process");
    }
    writeOwner(
        state->lockHandle,
        ownerJson(canonicalRoot, clusterId, nodeId, instanceId).dump() + "\n");
#else
    state->lockFd = ::open(
        state->lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (state->lockFd < 0) {
        throw std::runtime_error(
            "cannot open DATA_ROOT lock: " + std::string(std::strerror(errno)));
    }
    if (::flock(state->lockFd, LOCK_EX | LOCK_NB) != 0) {
        throw std::runtime_error(
            "DATA_ROOT is locked by another PacificDB process");
    }
    writeOwner(
        state->lockFd,
        ownerJson(canonicalRoot, clusterId, nodeId, instanceId).dump() + "\n");
#endif

    if (fs::exists(state->identityPath)) {
        json identity = loadIdentity(state->identityPath);
        requireIdentityField(identity, "clusterId", clusterId);
        requireIdentityField(identity, "nodeId", nodeId);
        requireIdentityField(
            identity, "canonicalRoot", canonicalRoot.u8string());
        if (!identity.contains("creationTimestamp") ||
            !identity["creationTimestamp"].is_string() ||
            identity["creationTimestamp"].get<std::string>().empty()) {
            throw std::runtime_error(
                "DATA_ROOT identity is missing creationTimestamp");
        }
        const std::string actualFormat =
            identity.value("storageFormatVersion", std::string());
        if (actualFormat != storageFormatVersion) {
            // v2 reads v1 JSON SST/index artifacts and rewrites them lazily on
            // flush/compaction, so the only atomic migration is root metadata.
            if (actualFormat == "1" && storageFormatVersion == "2") {
                identity["storageFormatVersion"] = "2";
                identity["migratedFromStorageFormatVersion"] = "1";
                identity["migrationTimestamp"] = utcNow();
                atomicWriteIdentity(state->identityPath, identity);
            } else {
                requireIdentityField(
                    identity, "storageFormatVersion", storageFormatVersion);
            }
        }
    } else {
        if (hasUnidentifiedContent(canonicalRoot)) {
            throw std::runtime_error(
                "DATA_ROOT contains data but has no PacificDB root identity; "
                "refusing implicit adoption");
        }
        const json identity = {
            {"schema", "pacificdb.storage-root-identity.v1"},
            {"clusterId", clusterId},
            {"nodeId", nodeId},
            {"storageFormatVersion", storageFormatVersion},
            {"canonicalRoot", canonicalRoot.u8string()},
            {"creationTimestamp", utcNow()},
        };
        atomicWriteIdentity(state->identityPath, identity);
    }

    return StorageRootGuard(std::move(state));
}

std::optional<StorageRootOwner> StorageRootGuard::readOwner(
    const fs::path& configuredRoot) noexcept {
    try {
        if (configuredRoot.empty() || !configuredRoot.is_absolute()) {
            return std::nullopt;
        }
        std::error_code error;
        const auto canonicalRoot = fs::canonical(configuredRoot, error);
        if (error) return std::nullopt;
        const auto lockPath = validateContainedStoragePath(
            canonicalRoot, canonicalRoot / kLockFile);
        const auto payload = readOwnerPayload(lockPath);
        if (payload.empty()) return std::nullopt;
        const auto value = json::parse(payload, nullptr, false);
        if (value.is_discarded() || !value.is_object() ||
            value.value("schema", std::string()) !=
                "pacificdb.storage-root-owner.v1") {
            return std::nullopt;
        }
        StorageRootOwner owner;
        owner.version = value.value("version", 0);
        owner.pid = value.value("pid", std::uint64_t{0});
        owner.clusterId = value.value("cluster_id", std::string());
        owner.nodeId = value.value("node_id", std::string());
        owner.canonicalRoot = fs::u8path(
            value.value("canonical_root", std::string()));
        owner.instanceId = value.value("instance_id", std::string());
        if (owner.version != 1 || owner.pid == 0 || owner.clusterId.empty() ||
            owner.nodeId.empty() || !owner.canonicalRoot.is_absolute()) {
            return std::nullopt;
        }
        return owner;
    } catch (...) {
        return std::nullopt;
    }
}

const fs::path& StorageRootGuard::canonicalRoot() const {
    if (!state_) throw std::logic_error("StorageRootGuard is not acquired");
    return state_->canonicalRoot;
}

const fs::path& StorageRootGuard::identityPath() const {
    if (!state_) throw std::logic_error("StorageRootGuard is not acquired");
    return state_->identityPath;
}

const fs::path& StorageRootGuard::lockPath() const {
    if (!state_) throw std::logic_error("StorageRootGuard is not acquired");
    return state_->lockPath;
}
