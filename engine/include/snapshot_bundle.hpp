#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pacificdb::snapshot_bundle {

struct ArtifactSource {
    std::filesystem::path source;
};

struct Bundle {
    nlohmann::json manifest;
    uint64_t dataOffset = 0;
};

bool isBundle(const std::filesystem::path& path);
Bundle read(const std::filesystem::path& path);
void write(const std::filesystem::path& path,
           nlohmann::json manifest,
           const std::vector<ArtifactSource>& sources);
void extract(const std::filesystem::path& bundlePath,
             const Bundle& bundle,
             size_t artifactIndex,
             const std::filesystem::path& target);

}  // namespace pacificdb::snapshot_bundle
