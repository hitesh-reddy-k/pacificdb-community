#pragma once

#include <filesystem>
#include <memory>
#include <string>

// Owns the process-wide exclusive lock for one canonical DATA_ROOT and verifies
// the durable identity bound to that root. The lock remains held for the
// lifetime of this object.
class StorageRootGuard {
public:
    StorageRootGuard();
    ~StorageRootGuard();
    StorageRootGuard(StorageRootGuard&&) noexcept;
    StorageRootGuard& operator=(StorageRootGuard&&) noexcept;
    StorageRootGuard(const StorageRootGuard&) = delete;
    StorageRootGuard& operator=(const StorageRootGuard&) = delete;

    static StorageRootGuard acquire(
        const std::filesystem::path& configuredRoot,
        const std::string& clusterId,
        const std::string& nodeId,
        const std::string& storageFormatVersion);

    const std::filesystem::path& canonicalRoot() const;
    const std::filesystem::path& identityPath() const;
    const std::filesystem::path& lockPath() const;

private:
    struct State;
    explicit StorageRootGuard(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
