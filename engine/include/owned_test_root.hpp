#pragma once

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// Test storage must never depend on the process working directory. This helper
// creates one uniquely owned absolute parent in the operating-system temporary
// directory and removes it only while its exact ownership marker is intact.
class OwnedTestRoot {
public:
    explicit OwnedTestRoot(const std::string& prefix) {
        if (prefix.empty()) {
            throw std::invalid_argument("test-root prefix must not be empty");
        }
        for (unsigned char ch : prefix) {
            if (!std::isalnum(ch) && ch != '-' && ch != '_') {
                throw std::invalid_argument("test-root prefix contains an unsafe character");
            }
        }

        const auto base = std::filesystem::weakly_canonical(
            std::filesystem::temp_directory_path());
        const auto pid =
#ifdef _WIN32
            static_cast<unsigned long long>(_getpid());
#else
            static_cast<unsigned long long>(getpid());
#endif
        const auto tick = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        bool created = false;
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            const auto nonce = sequence_.fetch_add(1, std::memory_order_relaxed);
            const auto candidate =
                base / ("pacificdb-test-" + prefix + "-" + std::to_string(pid) +
                        "-" + std::to_string(tick) + "-" +
                        std::to_string(nonce) + "-" + std::to_string(attempt));
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec)) {
                parent_ = std::filesystem::canonical(candidate);
                created = true;
                break;
            }
            if (ec && ec != std::errc::file_exists) {
                throw std::runtime_error(
                    "unable to create owned test root: " + ec.message());
            }
        }
        if (!created) {
            throw std::runtime_error("unable to allocate a unique owned test root");
        }

        token_ = prefix + ":" + std::to_string(pid) + ":" +
                 std::to_string(tick);
        marker_ = parent_ / ".pacificdb-owned-test-root";
        {
            std::ofstream marker(marker_, std::ios::binary | std::ios::trunc);
            if (!marker) {
                std::filesystem::remove(parent_);
                throw std::runtime_error("unable to write owned test-root marker");
            }
            marker << token_ << '\n';
            marker.flush();
            if (!marker) {
                std::filesystem::remove(marker_);
                std::filesystem::remove(parent_);
                throw std::runtime_error("unable to persist owned test-root marker");
            }
        }

        dataRoot_ = parent_ / "data";
        std::filesystem::create_directory(dataRoot_);
        dataRoot_ = std::filesystem::canonical(dataRoot_);
        if (!dataRoot_.is_absolute()) {
            throw std::runtime_error("owned test data root is not absolute");
        }
    }

    OwnedTestRoot(const OwnedTestRoot&) = delete;
    OwnedTestRoot& operator=(const OwnedTestRoot&) = delete;
    OwnedTestRoot(OwnedTestRoot&&) = delete;
    OwnedTestRoot& operator=(OwnedTestRoot&&) = delete;

    ~OwnedTestRoot() {
        try {
            if (parent_.empty() || marker_.empty() ||
                std::filesystem::is_symlink(marker_) ||
                !std::filesystem::is_regular_file(marker_)) {
                return;
            }
            std::ifstream marker(marker_, std::ios::binary);
            std::string actual;
            std::getline(marker, actual);
            if (actual == token_) {
                std::filesystem::remove_all(parent_);
            }
        } catch (...) {
            // A test-root cleanup failure must not mask the test result. The
            // marker makes any retained directory safe to identify later.
        }
    }

    const std::filesystem::path& parent() const {
        return parent_;
    }

    const std::filesystem::path& dataRoot() const {
        return dataRoot_;
    }

private:
    inline static std::atomic<unsigned long long> sequence_{0};
    std::filesystem::path parent_;
    std::filesystem::path marker_;
    std::filesystem::path dataRoot_;
    std::string token_;
};
