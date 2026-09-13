#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace pacificdb::cli {

enum class EngineState {
    healthy_existing,
    starting,
    spawned,
    exited,
    unhealthy,
    stale_pid,
    pid_reused,
    port_conflict,
    data_root_in_use,
};

enum class EngineAction {
    none,
    wait,
    spawn,
    remove_stale_metadata,
};

struct EngineObservations {
    bool protocolHealthy = false;
    bool identityAvailable = false;
    bool identityMatches = false;
    bool pidPresent = false;
    bool pidLive = false;
    bool executableIdentityKnown = false;
    bool executableMatches = false;
    bool portOpen = false;
    bool lockOwned = false;
    bool lockIdentityMatches = true;
    bool spawnedProcessExited = false;
    bool startupDeadlineExpired = false;
};

EngineState classifyEngineState(const EngineObservations& observations) noexcept;
std::string_view engineStateName(EngineState state) noexcept;
std::string_view engineStateCode(
    EngineState state,
    const EngineObservations& observations) noexcept;
EngineAction actionForEngineState(EngineState state, bool autoStart) noexcept;

struct EngineProcessMetadata {
    int version = 1;
    std::uint64_t pid = 0;
    std::filesystem::path executable;
    std::string instanceId;
    std::string dataRootFingerprint;
    std::string discoveryNonce;
    int port = 0;
    std::int64_t startedAtMs = 0;
};

std::optional<EngineProcessMetadata> readProcessMetadata(
    const std::filesystem::path& metadataPath) noexcept;
void writeProcessMetadata(
    const std::filesystem::path& metadataPath,
    const EngineProcessMetadata& metadata);
std::optional<std::uint64_t> readNumericPid(
    const std::filesystem::path& pidPath) noexcept;
void writeNumericPid(
    const std::filesystem::path& pidPath,
    std::uint64_t pid);

std::uint64_t currentProcessId() noexcept;
bool processIsLive(std::uint64_t pid) noexcept;
std::optional<std::filesystem::path> processExecutable(
    std::uint64_t pid) noexcept;
bool sameExecutable(
    const std::filesystem::path& first,
    const std::filesystem::path& second) noexcept;
std::string generateDiscoveryToken();
std::string dataRootFingerprint(const std::filesystem::path& dataRoot);
bool constantTimeEquals(std::string_view first, std::string_view second) noexcept;
bool requestEngineShutdown(std::uint64_t pid) noexcept;

struct LocalEngineOptions {
    std::string host;
    int port = 9000;
    std::filesystem::path home;
    std::filesystem::path enginePath;
    bool autoStart = true;
    std::chrono::milliseconds startupTimeout{120000};
};

struct LocalEngineResult {
    EngineState state = EngineState::unhealthy;
    std::string code;
    bool started = false;
    std::uint64_t pid = 0;
};

LocalEngineResult ensureLocalEngine(
    const LocalEngineOptions& options,
    std::ostream& output);

}  // namespace pacificdb::cli
