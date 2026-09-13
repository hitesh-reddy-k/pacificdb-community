#include "media_upload_state.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace pacificdb::community {

MediaState parseMediaState(std::string_view value) {
    if (value == "uploading") return MediaState::uploading;
    if (value == "verifying") return MediaState::verifying;
    if (value == "ready") return MediaState::ready;
    if (value == "failed") return MediaState::failed;
    if (value == "aborted") return MediaState::aborted;
    throw std::invalid_argument("unknown media state: " + std::string(value));
}

std::string_view mediaStateName(MediaState state) noexcept {
    switch (state) {
        case MediaState::uploading: return "uploading";
        case MediaState::verifying: return "verifying";
        case MediaState::ready: return "ready";
        case MediaState::failed: return "failed";
        case MediaState::aborted: return "aborted";
    }
    return "failed";
}

bool canTransition(MediaState from, MediaState to) noexcept {
    if (from == to) return true;
    switch (from) {
        case MediaState::uploading:
            return to == MediaState::verifying || to == MediaState::failed ||
                   to == MediaState::aborted;
        case MediaState::verifying:
            return to == MediaState::uploading || to == MediaState::ready ||
                   to == MediaState::failed || to == MediaState::aborted;
        case MediaState::failed:
            return to == MediaState::aborted;
        case MediaState::ready:
        case MediaState::aborted:
            return false;
    }
    return false;
}

MediaProgress reconstructMediaProgress(
    const nlohmann::json& manifest,
    const std::vector<nlohmann::json>& chunks,
    long long nowMs) {
    (void)nowMs;
    if (!manifest.is_object()) {
        throw std::invalid_argument("media manifest must be an object");
    }
    (void)parseMediaState(manifest.value("status", std::string()));
    const long long chunkCount = manifest.value("chunk_count", -1LL);
    const long long sizeBytes = manifest.value("size_bytes", -1LL);
    if (chunkCount <= 0 || sizeBytes < 0) {
        throw std::invalid_argument("media manifest limits are invalid");
    }
    MediaProgress progress;
    progress.stateVersion = manifest.value("state_version", 1);
    if (progress.stateVersion < 1 || progress.stateVersion > 2) {
        throw std::invalid_argument("unsupported media state version");
    }
    if (manifest.contains("lease_expires_at_ms")) {
        progress.leaseExpiresAtMs =
            manifest.value("lease_expires_at_ms", 0LL);
        if (progress.leaseExpiresAtMs <= 0) {
            throw std::invalid_argument("media lease expiry must be positive");
        }
    }

    struct ChunkIdentity {
        long long size = 0;
        std::string sha256;
    };
    std::map<long long, ChunkIdentity> durable;
    for (const auto& chunk : chunks) {
        if (!chunk.is_object()) {
            throw std::invalid_argument("media chunk metadata must be an object");
        }
        const long long index = chunk.value("index", -1LL);
        const long long chunkBytes = chunk.value("size_bytes", -1LL);
        const std::string sha256 = chunk.value("sha256", std::string());
        if (index < 0 || index >= chunkCount || chunkBytes < 0) {
            throw std::invalid_argument("media chunk progress is out of range");
        }
        const auto [entry, inserted] = durable.emplace(
            index, ChunkIdentity{chunkBytes, sha256});
        if (!inserted && (entry->second.size != chunkBytes ||
                          entry->second.sha256 != sha256)) {
            throw std::invalid_argument("conflicting duplicate media chunk metadata");
        }
    }
    for (const auto& [index, identity] : durable) {
        if (identity.size > std::numeric_limits<long long>::max() -
                                progress.receivedBytes) {
            throw std::invalid_argument("media progress size overflow");
        }
        progress.receivedBytes += identity.size;
        if (progress.receivedBytes > sizeBytes) {
            throw std::invalid_argument("media progress exceeds manifest size");
        }
        progress.receivedIndices.push_back(index);
    }
    progress.receivedChunks = static_cast<long long>(durable.size());
    progress.nextMissingIndex = chunkCount;
    for (long long index = 0; index < chunkCount; ++index) {
        if (durable.find(index) == durable.end()) {
            progress.nextMissingIndex = index;
            break;
        }
    }
    return progress;
}

bool leaseExpired(const nlohmann::json& manifest, long long nowMs) {
    if (!manifest.is_object() || !manifest.contains("lease_expires_at_ms")) {
        return false;
    }
    const long long expiry = manifest.value("lease_expires_at_ms", 0LL);
    if (expiry <= 0) {
        throw std::invalid_argument("media lease expiry must be positive");
    }
    return nowMs >= expiry;
}

}  // namespace pacificdb::community
