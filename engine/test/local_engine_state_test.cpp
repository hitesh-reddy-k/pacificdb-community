#include "local_engine.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path makeTestRoot() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = fs::temp_directory_path() /
        fs::u8path(std::string(u8"pacificdb engine state తెలుగు ") + suffix);
    fs::create_directories(root);
    return root;
}

void applyObservations(
    const json& value,
    pacificdb::cli::EngineObservations& observations) {
    observations.protocolHealthy = value.value("protocolHealthy", false);
    observations.identityAvailable = value.value("identityAvailable", false);
    observations.identityMatches = value.value("identityMatches", false);
    observations.pidPresent = value.value("pidPresent", false);
    observations.pidLive = value.value("pidLive", false);
    observations.executableIdentityKnown =
        value.value("executableIdentityKnown", false);
    observations.executableMatches = value.value("executableMatches", false);
    observations.portOpen = value.value("portOpen", false);
    observations.lockOwned = value.value("lockOwned", false);
    observations.lockIdentityMatches =
        value.value("lockIdentityMatches", false);
    observations.spawnedProcessExited =
        value.value("spawnedProcessExited", false);
    observations.startupDeadlineExpired =
        value.value("startupDeadlineExpired", false);
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream fixture(argv[1]);
    assert(fixture);
    const auto cases = json::parse(fixture);
    assert(cases.is_array());

    for (const auto& testCase : cases) {
        pacificdb::cli::EngineObservations observations{};
        applyObservations(testCase.at("observations"), observations);
        const auto state = pacificdb::cli::classifyEngineState(observations);
        assert(pacificdb::cli::engineStateName(state) ==
               testCase.at("expectedState").get<std::string>());
        assert(pacificdb::cli::engineStateCode(state, observations) ==
               testCase.at("expectedCode").get<std::string>());
        assert(pacificdb::cli::actionForEngineState(state, false) ==
               pacificdb::cli::EngineAction::none);
    }

    const auto root = makeTestRoot();
    const auto metadataPath = root / "engine.metadata.json";
    const auto pidPath = root / "engine.pid";
    try {
        const auto currentPid = pacificdb::cli::currentProcessId();
        assert(currentPid > 0);
        assert(pacificdb::cli::processIsLive(currentPid));
        const auto currentExecutable =
            pacificdb::cli::processExecutable(currentPid);
        assert(currentExecutable.has_value());
        assert(currentExecutable->is_absolute());
        assert(pacificdb::cli::sameExecutable(
            *currentExecutable, *currentExecutable));
        assert(!pacificdb::cli::processIsLive(
            std::numeric_limits<std::uint64_t>::max()));
        assert(!pacificdb::cli::requestEngineShutdown(
            std::numeric_limits<std::uint64_t>::max()));
#ifdef _WIN32
        const std::wstring shutdownName =
            L"Local\\PacificDBEngineShutdown-" + std::to_wstring(currentPid);
        const HANDLE shutdownEvent = CreateEventW(
            nullptr, TRUE, FALSE, shutdownName.c_str());
        assert(shutdownEvent != nullptr);
        assert(pacificdb::cli::requestEngineShutdown(currentPid));
        assert(WaitForSingleObject(shutdownEvent, 1000) == WAIT_OBJECT_0);
        CloseHandle(shutdownEvent);
#endif

        const auto firstToken = pacificdb::cli::generateDiscoveryToken();
        const auto secondToken = pacificdb::cli::generateDiscoveryToken();
        assert(firstToken.size() >= 32);
        assert(secondToken.size() >= 32);
        assert(firstToken != secondToken);
        assert(pacificdb::cli::constantTimeEquals(firstToken, firstToken));
        assert(!pacificdb::cli::constantTimeEquals(firstToken, secondToken));
        const auto firstFingerprint = pacificdb::cli::dataRootFingerprint(root);
        assert(firstFingerprint.rfind("sha256:", 0) == 0);
        assert(firstFingerprint == pacificdb::cli::dataRootFingerprint(
            fs::canonical(root)));

        pacificdb::cli::EngineProcessMetadata expected;
        expected.version = 1;
        expected.pid = 4242;
        expected.executable = fs::absolute(root / "bin" / "db engine");
        expected.instanceId = "engine_instance_a";
        expected.dataRootFingerprint = "sha256:0123456789abcdef";
        expected.discoveryNonce = "nonce_0123456789abcdef";
        expected.port = 19000;
        expected.startedAtMs = 123456789;

        pacificdb::cli::writeProcessMetadata(metadataPath, expected);
        const auto actual = pacificdb::cli::readProcessMetadata(metadataPath);
        assert(actual.has_value());
        assert(actual->version == expected.version);
        assert(actual->pid == expected.pid);
        assert(actual->executable == expected.executable);
        assert(actual->instanceId == expected.instanceId);
        assert(actual->dataRootFingerprint == expected.dataRootFingerprint);
        assert(actual->discoveryNonce == expected.discoveryNonce);
        assert(actual->port == expected.port);
        assert(actual->startedAtMs == expected.startedAtMs);
        assert(!fs::exists(metadataPath.string() + ".tmp"));
#ifndef _WIN32
        struct stat details {};
        assert(::stat(metadataPath.c_str(), &details) == 0);
        assert((details.st_mode & 0777) == 0600);
#endif

        pacificdb::cli::writeNumericPid(pidPath, 4242);
        assert(pacificdb::cli::readNumericPid(pidPath).value() == 4242);
#ifndef _WIN32
        struct stat pidDetails {};
        assert(::stat(pidPath.c_str(), &pidDetails) == 0);
        assert((pidDetails.st_mode & 0777) == 0600);
#endif

        const std::string malformed = "{\"version\":1,\"pid\":-1}";
        {
            std::ofstream output(metadataPath, std::ios::trunc);
            output << malformed;
        }
        assert(!pacificdb::cli::readProcessMetadata(metadataPath).has_value());
        assert(pacificdb::cli::readNumericPid(pidPath).value() == 4242);
        std::ifstream unchanged(metadataPath);
        assert(std::string(
                   std::istreambuf_iterator<char>(unchanged),
                   std::istreambuf_iterator<char>()) == malformed);
    } catch (...) {
        fs::remove_all(root);
        throw;
    }
    fs::remove_all(root);
    return 0;
}
