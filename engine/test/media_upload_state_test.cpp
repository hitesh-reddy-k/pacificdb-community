#include "media_upload_state.hpp"

#include <cassert>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

using nlohmann::json;
using pacificdb::community::MediaState;

int main() {
    using pacificdb::community::canTransition;
    using pacificdb::community::leaseExpired;
    using pacificdb::community::mediaStateName;
    using pacificdb::community::parseMediaState;
    using pacificdb::community::reconstructMediaProgress;

    assert(canTransition(MediaState::uploading, MediaState::verifying));
    assert(canTransition(MediaState::uploading, MediaState::aborted));
    assert(canTransition(MediaState::uploading, MediaState::failed));
    assert(canTransition(MediaState::verifying, MediaState::ready));
    assert(canTransition(MediaState::verifying, MediaState::failed));
    assert(canTransition(MediaState::verifying, MediaState::uploading));
    assert(!canTransition(MediaState::ready, MediaState::aborted));
    assert(!canTransition(MediaState::failed, MediaState::ready));
    assert(mediaStateName(parseMediaState("uploading")) == "uploading");
    bool unknownRejected = false;
    try { (void)parseMediaState("unknown"); }
    catch (const std::invalid_argument&) { unknownRejected = true; }
    assert(unknownRejected);

    json manifest{{"chunk_count", 3}, {"size_bytes", 9},
                  {"status", "uploading"}, {"state_version", 2},
                  {"lease_expires_at_ms", 5000}};
    std::vector<json> chunks = {
        {{"index", 0}, {"size_bytes", 4}, {"sha256", "a"}},
        {{"index", 2}, {"size_bytes", 1}, {"sha256", "b"}},
    };
    const auto progress = reconstructMediaProgress(manifest, chunks, 1000);
    assert(progress.stateVersion == 2);
    assert(progress.receivedChunks == 2);
    assert(progress.receivedBytes == 5);
    assert(progress.nextMissingIndex == 1);
    assert(progress.receivedIndices == std::vector<long long>({0, 2}));
    assert(progress.leaseExpiresAtMs == 5000);
    assert(!leaseExpired(manifest, 4999));
    assert(leaseExpired(manifest, 5000));

    json legacy{{"chunk_count", 1}, {"size_bytes", 4},
                {"status", "uploading"}};
    assert(reconstructMediaProgress(legacy, {}, 1000).stateVersion == 1);
    assert(!leaseExpired(legacy, std::numeric_limits<long long>::max()));

    bool duplicateRejected = false;
    try {
        (void)reconstructMediaProgress(manifest, {
            {{"index", 0}, {"size_bytes", 4}, {"sha256", "a"}},
            {{"index", 0}, {"size_bytes", 3}, {"sha256", "different"}},
        }, 1000);
    } catch (const std::invalid_argument&) { duplicateRejected = true; }
    assert(duplicateRejected);

    bool overflowRejected = false;
    try {
        (void)reconstructMediaProgress(
            json{{"chunk_count", 2},
                 {"size_bytes", std::numeric_limits<long long>::max()},
                 {"status", "uploading"}},
            {{{"index", 0},
              {"size_bytes", std::numeric_limits<long long>::max()}},
             {{"index", 1}, {"size_bytes", 1}}}, 1000);
    } catch (const std::invalid_argument&) { overflowRejected = true; }
    assert(overflowRejected);
    return 0;
}
