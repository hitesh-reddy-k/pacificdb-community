#pragma once

#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace pacificdb::community {

enum class MediaState { uploading, verifying, ready, failed, aborted };

MediaState parseMediaState(std::string_view value);
std::string_view mediaStateName(MediaState state) noexcept;
bool canTransition(MediaState from, MediaState to) noexcept;

struct MediaProgress {
    int stateVersion = 1;
    long long receivedChunks = 0;
    long long receivedBytes = 0;
    long long nextMissingIndex = 0;
    long long leaseExpiresAtMs = 0;
    std::vector<long long> receivedIndices;
};

MediaProgress reconstructMediaProgress(
    const nlohmann::json& manifest,
    const std::vector<nlohmann::json>& chunks,
    long long nowMs);
bool leaseExpired(const nlohmann::json& manifest, long long nowMs);

}  // namespace pacificdb::community
