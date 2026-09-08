#include "storage_root_guard.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("pacificdb-storage-root-migration-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        {
            auto guard = StorageRootGuard::acquire(root, "cluster-a", "node-a", "1");
        }
        {
            auto guard = StorageRootGuard::acquire(root, "cluster-a", "node-a", "2");
        }
        json identity;
        std::ifstream(root / ".pacificdb-root-identity.json") >> identity;
        if (identity.value("storageFormatVersion", "") != "2" ||
            identity.value("migratedFromStorageFormatVersion", "") != "1") {
            throw std::runtime_error("v1 root identity was not migrated to v2");
        }
        bool downgradeRejected = false;
        try {
            auto guard = StorageRootGuard::acquire(root, "cluster-a", "node-a", "1");
        } catch (const std::exception&) {
            downgradeRejected = true;
        }
        if (!downgradeRejected) throw std::runtime_error("v2 to v1 downgrade was accepted");
        fs::remove_all(root);
        std::cout << "STORAGE_ROOT_V1_TO_V2_MIGRATION_PASS\n";
        return 0;
    } catch (const std::exception& error) {
        fs::remove_all(root);
        std::cerr << "STORAGE_ROOT_V1_TO_V2_MIGRATION_FAIL: " << error.what() << '\n';
        return 1;
    }
}
