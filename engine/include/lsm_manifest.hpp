#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class LsmManifestLoadStatus { ABSENT, OK, ERROR };

struct LsmManifest {
    std::uint32_t formatVersion{1};
    std::uint64_t generation{0};
    std::uint64_t coveredWalLsn{0};
    std::vector<std::string> sstFiles;
};

struct LsmManifestLoadResult {
    LsmManifestLoadStatus status{LsmManifestLoadStatus::ABSENT};
    LsmManifest manifest;
    std::string error;
};

class LsmManifestStore {
public:
    static LsmManifestLoadResult load(const std::filesystem::path& lsmDirectory);
    static bool publishedSsts(const std::filesystem::path& lsmDirectory,
                              std::vector<std::filesystem::path>& output,
                              std::string* error = nullptr);
    static bool publish(const std::filesystem::path& lsmDirectory,
                        const LsmManifest& manifest,
                        const std::vector<std::filesystem::path>& newArtifacts,
                        std::string* error = nullptr);
};
