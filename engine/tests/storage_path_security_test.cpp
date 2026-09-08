#include "storage_path.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

static bool rejected(const std::string& value, std::string_view fieldName) {
    try {
        (void)validateStorageIdentifier(value, fieldName);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

int main() {
    const std::vector<std::string> unsafe = {
        "../escape",
        "../../escape2",
        ".",
        "..",
        "/tmp/escape",
        "tmp/../escape",
        "name/child",
        "name\\child",
        "",
        std::string("nul\0byte", 8),
        std::string(256, 'x'),
    };
    const std::vector<std::string_view> clientControlledFields = {
        "userId",
        "databaseName",
        "collectionName",
        "indexName",
        "snapshotIdentifier",
        "backupIdentifier",
        "restoreIdentifier",
        "temporaryArtifactIdentifier",
    };
    for (const auto field : clientControlledFields) {
        for (const auto& identifier : unsafe) {
            require(rejected(identifier, field),
                    "unsafe identifier was accepted for a client-controlled field");
        }
    }

    const std::vector<std::string> safe = {
        "normal-name",
        "name.with.dots",
        "name-with-dashes",
        "name_with_underscores",
        u8"नाम",
        std::string(255, 'x'),
    };
    for (const auto field : clientControlledFields) {
        for (const auto& identifier : safe) {
            require(validateStorageIdentifier(identifier, field) == identifier,
                    "safe identifier was changed or rejected");
        }
    }

    char rootTemplate[] = "/tmp/pacificdb-path002-unit.XXXXXX";
    char* rootName = ::mkdtemp(rootTemplate);
    require(rootName != nullptr, "mkdtemp failed");
    const fs::path ownerRoot(rootName);
    const fs::path dataRoot = ownerRoot / "data";
    const fs::path outside = ownerRoot / "outside";
    fs::create_directory(dataRoot);
    fs::create_directory(outside);
    {
        std::ofstream marker(ownerRoot / "PACIFICDB_PATH002_TEST_OWNER");
        marker << "storage_path_security_test\n";
    }

    try {
        const fs::path candidate =
            dataRoot / validateStorageIdentifier("normal-name", "userId") /
            validateStorageIdentifier("name.with.dots", "databaseName") /
            validateStorageIdentifier(u8"संग्रह", "collectionName");
        require(isPathWithin(fs::canonical(dataRoot), candidate),
                "safe candidate failed component containment");
        require(validateContainedStoragePath(dataRoot, candidate) ==
                    candidate.lexically_normal(),
                "safe candidate path changed");

        const fs::path prefixCollision =
            dataRoot.parent_path() / (dataRoot.filename().string() + "-evil");
        require(!isPathWithin(fs::canonical(dataRoot), prefixCollision),
                "string-prefix negative control was not rejected");

        const fs::path symlink = dataRoot / "link";
        fs::create_directory_symlink(outside, symlink);
        bool symlinkRejected = false;
        try {
            (void)validateContainedStoragePath(dataRoot, symlink / "database");
        } catch (const std::invalid_argument&) {
            symlinkRejected = true;
        }
        require(symlinkRejected, "symlink escape was accepted");
        require(fs::is_empty(outside), "symlink test created an outside artifact");

        const fs::path fifo = dataRoot / "special";
        require(::mkfifo(fifo.c_str(), 0600) == 0, "mkfifo failed");
        bool specialRejected = false;
        try {
            (void)validateContainedStoragePath(dataRoot, fifo / "child");
        } catch (const std::invalid_argument&) {
            specialRejected = true;
        }
        require(specialRejected, "special-file traversal was accepted");

        const std::vector<std::string> unsafeSnapshotRelativePaths = {
            "../escape",
            "../../escape2",
            ".",
            "..",
            "/tmp/escape",
            "tmp/../escape",
            "name\\child",
            "",
            std::string("nul\0byte", 8),
            std::string(256, 'x'),
        };
        for (const auto& identifier : unsafeSnapshotRelativePaths) {
            bool relativeRejected = false;
            try {
                (void)validateStorageRelativePath(
                    fs::path(identifier), "snapshotArtifact");
            } catch (const std::invalid_argument&) {
                relativeRejected = true;
            }
            require(relativeRejected,
                    "unsafe snapshot relative path was accepted (bytes=" +
                        std::to_string(identifier.size()) + "): " + identifier);
        }
        const fs::path safeSnapshotPath =
            validateStorageRelativePath(
                fs::path("normal-name") / "name.with.dots" /
                    "name_with_underscores",
                "snapshotArtifact");
        require(safeSnapshotPath ==
                    fs::path("normal-name") / "name.with.dots" /
                        "name_with_underscores",
                "safe snapshot relative path was changed");

        const fs::path created =
            createContainedStorageDirectories(dataRoot, dataRoot / "a" / "b");
        require(fs::is_directory(created), "contained directory creation failed");
        require(!fs::exists("/tmp/escape") && !fs::exists("/tmp/escape2"),
                "escape artifact exists");
    } catch (...) {
        if (fs::exists(ownerRoot / "PACIFICDB_PATH002_TEST_OWNER")) {
            fs::remove_all(ownerRoot);
        }
        throw;
    }

    require(fs::exists(ownerRoot / "PACIFICDB_PATH002_TEST_OWNER"),
            "test ownership marker missing");
    fs::remove_all(ownerRoot);
    std::cout << "PATH002_STORAGE_PATH_UNIT_PASS\n";
    return 0;
}
