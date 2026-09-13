#include "storage_root_guard.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::uint64_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace

int main() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = fs::temp_directory_path() /
        fs::path("pacificdb lock owner తెలుగు " + suffix);
    fs::create_directories(root);

    try {
        const auto first = StorageRootGuard::acquire(
            root, "cluster", "node", "2", "instance-a");
        const auto owner = StorageRootGuard::readOwner(root);
        assert(owner.has_value());
        assert(owner->pid == currentProcessId());
        assert(owner->clusterId == "cluster");
        assert(owner->nodeId == "node");
        assert(owner->canonicalRoot == fs::canonical(root));
        assert(owner->instanceId == "instance-a");

        bool rejected = false;
        try {
            auto second = StorageRootGuard::acquire(
                root, "cluster", "node", "2", "instance-b");
            (void)second;
        } catch (const std::exception&) {
            rejected = true;
        }
        assert(rejected);

        const auto ownerAfterRejection = StorageRootGuard::readOwner(root);
        assert(ownerAfterRejection.has_value());
        assert(ownerAfterRejection->instanceId == "instance-a");
    } catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
    return 0;
}
