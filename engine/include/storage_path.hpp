#pragma once

#include <filesystem>
#include <system_error>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

inline std::string validateStorageIdentifier(std::string_view value,
                                             std::string_view fieldName) {
    const std::string field(fieldName);
    if (value.empty()) {
        throw std::invalid_argument(field + " cannot be empty");
    }
    if (value == "." || value == "..") {
        throw std::invalid_argument(field + " contains an unsafe path component");
    }
    if (value.size() > 255) {
        throw std::invalid_argument(field + " exceeds the maximum byte length");
    }
    if (value.find('\0') != std::string_view::npos ||
        value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos) {
        throw std::invalid_argument(field + " contains a forbidden path character");
    }

    const std::filesystem::path component{std::string(value)};
    if (component.is_absolute() || component.has_root_path() ||
        component.has_parent_path() || component.filename() != component ||
        std::distance(component.begin(), component.end()) != 1) {
        throw std::invalid_argument(field + " is not a valid storage identifier");
    }
    return std::string(value);
}

inline bool isPathWithin(const std::filesystem::path& canonicalRoot,
                         const std::filesystem::path& candidate) {
    const auto root = canonicalRoot.lexically_normal();
    const auto child = candidate.lexically_normal();
    auto rootIt = root.begin();
    auto childIt = child.begin();
    for (; rootIt != root.end(); ++rootIt, ++childIt) {
        if (childIt == child.end() || *rootIt != *childIt) return false;
    }
    return childIt != child.end();
}

inline std::filesystem::path validateStorageRelativePath(
    const std::filesystem::path& relativePath,
    std::string_view fieldName) {
    if (relativePath.empty() || relativePath.is_absolute() ||
        relativePath.has_root_path()) {
        throw std::invalid_argument(std::string(fieldName) +
                                    " is not a relative storage path");
    }
    std::filesystem::path result;
    size_t componentCount = 0;
    for (const auto& component : relativePath) {
        result /= validateStorageIdentifier(component.string(), fieldName);
        ++componentCount;
    }
    if (componentCount == 0) {
        throw std::invalid_argument(std::string(fieldName) +
                                    " cannot be empty");
    }
    return result;
}

inline std::filesystem::path validateContainedStoragePath(
    const std::filesystem::path& canonicalRoot,
    const std::filesystem::path& candidate) {
    namespace fs = std::filesystem;
    if (canonicalRoot.empty() || !canonicalRoot.is_absolute()) {
        throw std::invalid_argument("canonical storage root must be absolute");
    }
    const fs::path root = fs::weakly_canonical(canonicalRoot);
    const fs::path child = candidate.lexically_normal();
    if (!isPathWithin(root, child)) {
        throw std::invalid_argument("storage path escapes canonical DATA_ROOT");
    }

    fs::path walked = root;
    auto rootIt = root.begin();
    auto childIt = child.begin();
    for (; rootIt != root.end() && childIt != child.end(); ++rootIt, ++childIt) {}
    for (; childIt != child.end(); ++childIt) {
        walked /= *childIt;
        std::error_code ec;
        const auto status = fs::symlink_status(walked, ec);
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            continue;
        }
        if (ec) {
            throw std::invalid_argument("cannot inspect storage path component");
        }
        if (fs::is_symlink(status)) {
            throw std::invalid_argument("storage path traverses a symbolic link");
        }
        if (fs::exists(status)) {
            if (!fs::is_directory(status) && walked != child) {
                throw std::invalid_argument(
                    "storage path traverses an unexpected special file");
            }
            if (!fs::is_directory(status) && !fs::is_regular_file(status)) {
                throw std::invalid_argument(
                    "storage path resolves to an unexpected special file");
            }
        }
    }
    return child;
}

inline std::filesystem::path createContainedStorageDirectories(
    const std::filesystem::path& canonicalRoot,
    const std::filesystem::path& candidate) {
    namespace fs = std::filesystem;
    const fs::path root = fs::weakly_canonical(canonicalRoot);
    const fs::path child = validateContainedStoragePath(root, candidate);

    fs::path walked = root;
    auto rootIt = root.begin();
    auto childIt = child.begin();
    for (; rootIt != root.end() && childIt != child.end(); ++rootIt, ++childIt) {}
    for (; childIt != child.end(); ++childIt) {
        walked /= *childIt;
        std::error_code ec;
        const auto status = fs::symlink_status(walked, ec);
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            if (!fs::create_directory(walked, ec) || ec) {
                throw std::runtime_error("cannot create contained storage directory");
            }
        } else if (ec) {
            throw std::runtime_error("cannot inspect contained storage directory");
        } else if (fs::is_symlink(status) || !fs::is_directory(status)) {
            throw std::invalid_argument(
                "contained storage directory is not a real directory");
        }
        validateContainedStoragePath(root, walked);
    }
    return child;
}
