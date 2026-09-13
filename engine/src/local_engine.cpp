#include "local_engine.hpp"
#include "socket_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <libproc.h>
#include <netdb.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace pacificdb::cli {

EngineState classifyEngineState(
    const EngineObservations& observations) noexcept {
    if (observations.lockOwned && !observations.lockIdentityMatches) {
        return EngineState::data_root_in_use;
    }
    if (observations.protocolHealthy) {
        if (observations.identityAvailable && !observations.identityMatches) {
            return EngineState::unhealthy;
        }
        return EngineState::healthy_existing;
    }
    if (observations.spawnedProcessExited) return EngineState::exited;
    if (observations.pidPresent && !observations.pidLive) {
        return EngineState::stale_pid;
    }
    if (observations.pidPresent && observations.pidLive) {
        if (observations.executableIdentityKnown &&
            !observations.executableMatches) {
            return EngineState::pid_reused;
        }
        if (observations.startupDeadlineExpired) {
            return EngineState::unhealthy;
        }
        if (observations.executableIdentityKnown &&
            observations.executableMatches) {
            return EngineState::starting;
        }
        return EngineState::unhealthy;
    }
    if (observations.portOpen) return EngineState::port_conflict;
    if (observations.startupDeadlineExpired) return EngineState::unhealthy;
    return EngineState::spawned;
}

std::string_view engineStateName(EngineState state) noexcept {
    switch (state) {
        case EngineState::healthy_existing: return "healthy_existing";
        case EngineState::starting: return "starting";
        case EngineState::spawned: return "spawned";
        case EngineState::exited: return "exited";
        case EngineState::unhealthy: return "unhealthy";
        case EngineState::stale_pid: return "stale_pid";
        case EngineState::pid_reused: return "pid_reused";
        case EngineState::port_conflict: return "port_conflict";
        case EngineState::data_root_in_use: return "data_root_in_use";
    }
    return "unhealthy";
}

std::string_view engineStateCode(
    EngineState state,
    const EngineObservations& observations) noexcept {
    switch (state) {
        case EngineState::healthy_existing: return "engine_healthy_existing";
        case EngineState::starting: return "engine_starting";
        case EngineState::spawned: return "engine_spawn_required";
        case EngineState::exited: return "engine_exited";
        case EngineState::unhealthy:
            return observations.protocolHealthy &&
                    observations.identityAvailable &&
                    !observations.identityMatches
                ? "engine_identity_mismatch"
                : "engine_unhealthy";
        case EngineState::stale_pid: return "engine_stale_pid";
        case EngineState::pid_reused: return "engine_pid_reused";
        case EngineState::port_conflict: return "engine_port_conflict";
        case EngineState::data_root_in_use: return "engine_data_root_in_use";
    }
    return "engine_unhealthy";
}

EngineAction actionForEngineState(
    EngineState state,
    bool autoStart) noexcept {
    if (!autoStart) return EngineAction::none;
    switch (state) {
        case EngineState::starting: return EngineAction::wait;
        case EngineState::spawned: return EngineAction::spawn;
        case EngineState::stale_pid: return EngineAction::remove_stale_metadata;
        default: return EngineAction::none;
    }
}

namespace {

#ifdef _WIN32
std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int characters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (characters <= 0) {
        throw std::runtime_error("invalid UTF-8 environment value");
    }
    std::wstring result(static_cast<std::size_t>(characters), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), characters) !=
        characters) {
        throw std::runtime_error("cannot convert environment value to UTF-16");
    }
    return result;
}

std::wstring environmentName(const char* name) {
    std::wstring result;
    while (*name) result.push_back(static_cast<unsigned char>(*name++));
    return result;
}

std::optional<fs::path> environmentPath(const char* name) {
    const auto wideName = environmentName(name);
    const DWORD required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (required == 0) return std::nullopt;
    std::vector<wchar_t> value(required);
    const DWORD written = GetEnvironmentVariableW(
        wideName.c_str(), value.data(), required);
    if (written == 0 || written >= required) {
        throw std::runtime_error(std::string("could not read ") + name);
    }
    return fs::path(std::wstring(value.data(), written));
}

bool environmentExists(const char* name) {
    const auto wideName = environmentName(name);
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    return required != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

void restrictToCurrentUser(const fs::path& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "open current user token");
    }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> tokenBytes(bytes);
    if (bytes == 0 || !GetTokenInformation(
            token, TokenUser, tokenBytes.data(), bytes, &bytes)) {
        const DWORD error = GetLastError();
        CloseHandle(token);
        throw std::system_error(
            static_cast<int>(error), std::system_category(),
            "read current user token");
    }
    auto* user = reinterpret_cast<TOKEN_USER*>(tokenBytes.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);
    PACL acl = nullptr;
    const DWORD aclResult = SetEntriesInAclW(1, &access, nullptr, &acl);
    CloseHandle(token);
    if (aclResult != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(aclResult), std::system_category(),
            "build current user file ACL");
    }
    const DWORD securityResult = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    if (securityResult != ERROR_SUCCESS) {
        throw std::system_error(
            static_cast<int>(securityResult), std::system_category(),
            "secure current user file ACL");
    }
}
#endif

bool validMetadata(const EngineProcessMetadata& metadata) {
    return metadata.version == 1 && metadata.pid > 0 &&
           metadata.executable.is_absolute() && !metadata.instanceId.empty() &&
           !metadata.dataRootFingerprint.empty() &&
           !metadata.discoveryNonce.empty() && metadata.port >= 1 &&
           metadata.port <= 65535 && metadata.startedAtMs >= 0;
}

json serializeMetadata(const EngineProcessMetadata& metadata) {
    return {
        {"schema", "pacificdb.local-engine.v1"},
        {"version", metadata.version},
        {"pid", metadata.pid},
        {"executable", metadata.executable.u8string()},
        {"instance_id", metadata.instanceId},
        {"data_root_fingerprint", metadata.dataRootFingerprint},
        {"discovery_nonce", metadata.discoveryNonce},
        {"port", metadata.port},
        {"started_at_ms", metadata.startedAtMs},
    };
}

std::uint64_t processIdForTemporaryName() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

fs::path temporaryMetadataPath(const fs::path& metadataPath) {
    static std::atomic<std::uint64_t> sequence{0};
    fs::path temporary = metadataPath;
    temporary += ".tmp." + std::to_string(processIdForTemporaryName()) + "." +
        std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

#ifndef _WIN32
void writeAll(int descriptor, const std::string& payload) {
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto count = ::write(
            descriptor, payload.data() + offset, payload.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            throw std::system_error(
                errno, std::system_category(), "write engine metadata");
        }
        offset += static_cast<std::size_t>(count);
    }
}
#endif

}  // namespace

std::optional<EngineProcessMetadata> readProcessMetadata(
    const fs::path& metadataPath) noexcept {
    try {
        std::ifstream input(metadataPath, std::ios::binary);
        if (!input) return std::nullopt;
        const auto value = json::parse(input, nullptr, false);
        if (value.is_discarded() || !value.is_object() ||
            value.value("schema", std::string()) !=
                "pacificdb.local-engine.v1") {
            return std::nullopt;
        }
        EngineProcessMetadata metadata;
        metadata.version = value.value("version", 0);
        metadata.pid = value.value("pid", std::uint64_t{0});
        metadata.executable = fs::u8path(
            value.value("executable", std::string()));
        metadata.instanceId = value.value("instance_id", std::string());
        metadata.dataRootFingerprint =
            value.value("data_root_fingerprint", std::string());
        metadata.discoveryNonce =
            value.value("discovery_nonce", std::string());
        metadata.port = value.value("port", 0);
        metadata.startedAtMs = value.value("started_at_ms", std::int64_t{-1});
        if (!validMetadata(metadata)) return std::nullopt;
        return metadata;
    } catch (...) {
        return std::nullopt;
    }
}

void writeProcessMetadata(
    const fs::path& metadataPath,
    const EngineProcessMetadata& metadata) {
    if (!metadataPath.is_absolute()) {
        throw std::invalid_argument("engine metadata path must be absolute");
    }
    if (!validMetadata(metadata)) {
        throw std::invalid_argument("engine metadata is invalid");
    }
    fs::create_directories(metadataPath.parent_path());
    const auto temporary = temporaryMetadataPath(metadataPath);
    const std::string payload = serializeMetadata(metadata).dump(2) + "\n";

#ifdef _WIN32
    const HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "create engine metadata temporary file");
    }
    DWORD written = 0;
    const bool writeOk = payload.size() <= std::numeric_limits<DWORD>::max() &&
        WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()),
                  &written, nullptr) && written == payload.size() &&
        FlushFileBuffers(file);
    const DWORD writeError = writeOk ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (writeOk) {
        try {
            restrictToCurrentUser(temporary);
        } catch (...) {
            DeleteFileW(temporary.c_str());
            throw;
        }
    }
    if (!writeOk || !MoveFileExW(
            temporary.c_str(), metadataPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = writeOk ? GetLastError() : writeError;
        DeleteFileW(temporary.c_str());
        throw std::system_error(
            static_cast<int>(error), std::system_category(),
            "publish engine metadata");
    }
#else
    int descriptor = ::open(
        temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        throw std::system_error(
            errno, std::system_category(),
            "create engine metadata temporary file");
    }
    try {
        writeAll(descriptor, payload);
        if (::fsync(descriptor) != 0) {
            throw std::system_error(
                errno, std::system_category(), "flush engine metadata");
        }
        if (::close(descriptor) != 0) {
            descriptor = -1;
            throw std::system_error(
                errno, std::system_category(), "close engine metadata");
        }
        descriptor = -1;
        if (::rename(temporary.c_str(), metadataPath.c_str()) != 0) {
            throw std::system_error(
                errno, std::system_category(), "publish engine metadata");
        }
        const int directory = ::open(
            metadataPath.parent_path().c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory >= 0) {
            const int result = ::fsync(directory);
            const int savedError = errno;
            ::close(directory);
            if (result != 0) {
                throw std::system_error(
                    savedError, std::system_category(),
                    "flush engine metadata directory");
            }
        }
    } catch (...) {
        if (descriptor >= 0) ::close(descriptor);
        ::unlink(temporary.c_str());
        throw;
    }
#endif
}

std::optional<std::uint64_t> readNumericPid(
    const fs::path& pidPath) noexcept {
    try {
        std::ifstream input(pidPath);
        std::uint64_t pid = 0;
        std::string extra;
        if (!(input >> pid) || pid == 0 || (input >> extra)) return std::nullopt;
        return pid;
    } catch (...) {
        return std::nullopt;
    }
}

std::uint64_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

bool processIsLive(std::uint64_t pid) noexcept {
    if (pid == 0) return false;
#ifdef _WIN32
    if (pid > std::numeric_limits<DWORD>::max()) return false;
    const HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    DWORD exitCode = 0;
    const bool live = GetExitCodeProcess(process, &exitCode) &&
        exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return live;
#else
    if (pid > static_cast<std::uint64_t>(
            std::numeric_limits<pid_t>::max())) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

bool requestEngineShutdown(std::uint64_t pid) noexcept {
    if (pid == 0) return false;
#ifdef _WIN32
    if (pid > std::numeric_limits<DWORD>::max()) return false;
    const std::wstring name = L"Local\\PacificDBEngineShutdown-" +
        std::to_wstring(pid);
    const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (!event) return false;
    const bool signaled = SetEvent(event) != FALSE;
    CloseHandle(event);
    return signaled;
#else
    if (pid > static_cast<std::uint64_t>(
            std::numeric_limits<pid_t>::max())) return false;
    return ::kill(static_cast<pid_t>(pid), SIGTERM) == 0;
#endif
}

std::optional<fs::path> processExecutable(std::uint64_t pid) noexcept {
    if (!processIsLive(pid)) return std::nullopt;
    try {
#ifdef _WIN32
        const HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, static_cast<DWORD>(pid));
        if (!process) return std::nullopt;
        std::vector<wchar_t> buffer(32768);
        DWORD length = static_cast<DWORD>(buffer.size());
        const bool ok = QueryFullProcessImageNameW(
            process, 0, buffer.data(), &length);
        CloseHandle(process);
        if (!ok || length == 0) return std::nullopt;
        return fs::path(std::wstring(buffer.data(), length));
#elif defined(__APPLE__)
        if (pid > static_cast<std::uint64_t>(
                std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        std::vector<char> buffer(PROC_PIDPATHINFO_MAXSIZE);
        const int length = proc_pidpath(
            static_cast<int>(pid), buffer.data(),
            static_cast<std::uint32_t>(buffer.size()));
        if (length <= 0) return std::nullopt;
        return fs::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#else
        const auto link = fs::path("/proc") / std::to_string(pid) / "exe";
        std::vector<char> buffer(4096);
        const auto length = ::readlink(
            link.c_str(), buffer.data(), buffer.size() - 1);
        if (length <= 0) return std::nullopt;
        return fs::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
    } catch (...) {
        return std::nullopt;
    }
}

bool sameExecutable(
    const fs::path& first,
    const fs::path& second) noexcept {
    try {
        std::error_code error;
        if (fs::exists(first, error) && !error && fs::exists(second, error) &&
            !error && fs::equivalent(first, second, error) && !error) {
            return true;
        }
        error.clear();
        auto normalizedFirst = fs::weakly_canonical(first, error);
        if (error) normalizedFirst = fs::absolute(first).lexically_normal();
        error.clear();
        auto normalizedSecond = fs::weakly_canonical(second, error);
        if (error) normalizedSecond = fs::absolute(second).lexically_normal();
#ifdef _WIN32
        auto firstText = normalizedFirst.native();
        auto secondText = normalizedSecond.native();
        std::transform(firstText.begin(), firstText.end(), firstText.begin(),
            [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
        std::transform(secondText.begin(), secondText.end(), secondText.begin(),
            [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
        return firstText == secondText;
#else
        return normalizedFirst == normalizedSecond;
#endif
    } catch (...) {
        return false;
    }
}

namespace {

std::string hex(const unsigned char* bytes, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

}  // namespace

std::string generateDiscoveryToken() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, static_cast<int>(sizeof(bytes))) != 1) {
        throw std::runtime_error("could not generate local engine identity");
    }
    return hex(bytes, sizeof(bytes));
}

std::string dataRootFingerprint(const fs::path& dataRoot) {
    std::error_code error;
    auto canonical = fs::weakly_canonical(dataRoot, error);
    if (error || canonical.empty() || !canonical.is_absolute()) {
        throw std::runtime_error("could not canonicalize local engine DATA_ROOT");
    }
#ifdef _WIN32
    auto normalized = canonical.native();
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    const auto value = fs::path(normalized).u8string();
#else
    const auto value = canonical.u8string();
#endif
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestSize = 0;
    if (EVP_Digest(
            value.data(), value.size(), digest, &digestSize,
            EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("could not fingerprint local engine DATA_ROOT");
    }
    return "sha256:" + hex(digest, digestSize);
}

bool constantTimeEquals(
    std::string_view first,
    std::string_view second) noexcept {
    const std::size_t maximum = std::max(first.size(), second.size());
    std::size_t difference = first.size() ^ second.size();
    for (std::size_t index = 0; index < maximum; ++index) {
        const unsigned char firstByte = index < first.size()
            ? static_cast<unsigned char>(first[index]) : 0;
        const unsigned char secondByte = index < second.size()
            ? static_cast<unsigned char>(second[index]) : 0;
        difference |= static_cast<std::size_t>(firstByte ^ secondByte);
    }
    return difference == 0;
}

namespace {

using NativeSocket = pacificdb::net::NativeSocket;

void closeNativeSocket(NativeSocket socket) noexcept {
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

void initializeSocketRuntime() {
#ifdef _WIN32
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!initialized) {
        throw std::runtime_error("could not initialize Windows networking");
    }
#endif
}

struct ProbeResult {
    bool healthy = false;
    bool identityAvailable = false;
    std::string edition;
    std::string instanceId;
    std::string dataRootFingerprint;
    std::uint64_t pid = 0;
};

std::optional<NativeSocket> connectLocal(
    const std::string& host,
    int port,
    std::chrono::milliseconds timeout) {
    initializeSocketRuntime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses) != 0) {
        return std::nullopt;
    }
    NativeSocket connected = pacificdb::net::invalidSocket;
    for (auto* address = addresses; address; address = address->ai_next) {
        const auto candidate = ::socket(
            address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == pacificdb::net::invalidSocket) continue;
        if (::connect(candidate, address->ai_addr,
                      static_cast<int>(address->ai_addrlen)) == 0) {
            connected = candidate;
            break;
        }
        closeNativeSocket(candidate);
    }
    freeaddrinfo(addresses);
    if (connected == pacificdb::net::invalidSocket) return std::nullopt;
    try {
        pacificdb::net::setSocketTimeouts(connected, timeout, timeout);
    } catch (...) {
        closeNativeSocket(connected);
        throw;
    }
    return connected;
}

bool portIsOpen(const LocalEngineOptions& options) {
    try {
        const auto socket = connectLocal(options.host, options.port,
                                         std::chrono::milliseconds(300));
        if (!socket) return false;
        closeNativeSocket(*socket);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<ProbeResult> probeEngine(
    const LocalEngineOptions& options,
    std::string_view nonce) {
    try {
        const auto socket = connectLocal(options.host, options.port,
                                         std::chrono::milliseconds(1000));
        if (!socket) return std::nullopt;
        json command{{"action", "ping"}, {"userId", "system"}};
        if (!nonce.empty()) command["local_discovery_nonce"] = nonce;
        const auto payload = command.dump() + "\n";
        std::size_t sent = 0;
        while (sent < payload.size()) {
            const auto count = ::send(
                *socket, payload.data() + sent,
                static_cast<int>(payload.size() - sent), 0);
            if (count <= 0) {
                closeNativeSocket(*socket);
                return std::nullopt;
            }
            sent += static_cast<std::size_t>(count);
        }

        std::string response;
        response.reserve(1024);
        char buffer[1024];
        while (response.size() <= 64 * 1024) {
            const auto count = ::recv(*socket, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            response.append(buffer, static_cast<std::size_t>(count));
            const auto newline = response.find('\n');
            if (newline == std::string::npos) continue;
            response.resize(newline);
            break;
        }
        closeNativeSocket(*socket);
        if (response.empty() || response.size() > 64 * 1024) {
            return std::nullopt;
        }
        const auto value = json::parse(response, nullptr, false);
        if (value.is_discarded() || !value.is_object() ||
            value.value("status", std::string()) != "pong") {
            return std::nullopt;
        }
        ProbeResult result;
        result.healthy = true;
        result.edition = value.value("edition", std::string());
        result.identityAvailable =
            value.contains("instance_id") && value.contains("engine_pid") &&
            value.contains("data_root_fingerprint");
        if (result.identityAvailable) {
            result.instanceId = value.value("instance_id", std::string());
            result.pid = value.value("engine_pid", std::uint64_t{0});
            result.dataRootFingerprint = value.value(
                "data_root_fingerprint", std::string());
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

struct Inspection {
    EngineObservations observations;
    std::optional<EngineProcessMetadata> metadata;
    std::uint64_t pid = 0;
};

bool storageRootLockHeld(const fs::path& lockPath) noexcept {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        return false;
    }
    const DWORD error = GetLastError();
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
#else
    const int descriptor = ::open(lockPath.c_str(), O_RDWR | O_CLOEXEC);
    if (descriptor < 0) return false;
    if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
        ::flock(descriptor, LOCK_UN);
        ::close(descriptor);
        return false;
    }
    const bool held = errno == EWOULDBLOCK || errno == EAGAIN;
    ::close(descriptor);
    return held;
#endif
}

std::pair<std::uint64_t, std::string> storageRootOwner(
    const fs::path& lockPath) noexcept {
    try {
        std::ifstream input(lockPath, std::ios::binary);
        const auto owner = json::parse(input, nullptr, false);
        if (owner.is_discarded() || !owner.is_object() ||
            owner.value("schema", std::string()) !=
                "pacificdb.storage-root-owner.v1") return {};
        return {owner.value("pid", std::uint64_t{0}),
                owner.value("instance_id", std::string())};
    } catch (...) {
        return {};
    }
}

Inspection inspectEngine(const LocalEngineOptions& options) {
    Inspection inspection;
    const auto metadataPathlish = options.home / "engine.metadata.json";
    const bool metadataFilePresent = fs::exists(metadataPathlish);
    inspection.metadata = readProcessMetadata(metadataPathlish);
    const auto numericPid = readNumericPid(options.home / "engine.pid");
    if (inspection.metadata) {
        inspection.pid = inspection.metadata->pid;
    } else if (numericPid) {
        inspection.pid = *numericPid;
    }
    inspection.observations.pidPresent = inspection.pid != 0;
    inspection.observations.pidLive = processIsLive(inspection.pid);
    if (inspection.observations.pidLive) {
        const auto executable = processExecutable(inspection.pid);
        inspection.observations.executableIdentityKnown = executable.has_value();
        if (executable) {
            inspection.observations.executableMatches =
                sameExecutable(*executable, options.enginePath);
        }
    }

#ifdef _WIN32
    const auto configuredDataRoot = environmentPath("DATA_ROOT");
    const auto dataRoot = configuredDataRoot
        ? fs::absolute(*configuredDataRoot) : options.home / "data";
#else
    const auto dataRoot = std::getenv("DATA_ROOT")
        ? fs::absolute(fs::u8path(std::getenv("DATA_ROOT")))
        : options.home / "data";
#endif
    const auto lockPath = dataRoot / ".pacificdb-root.lock";
    inspection.observations.lockOwned = storageRootLockHeld(lockPath);
    if (inspection.observations.lockOwned) {
        const auto [ownerPid, ownerInstance] = storageRootOwner(lockPath);
        inspection.observations.lockIdentityMatches = inspection.metadata &&
            ownerPid == inspection.metadata->pid &&
            !ownerInstance.empty() &&
            ownerInstance == inspection.metadata->instanceId;
    }

    const auto probe = probeEngine(
        options,
        inspection.metadata ? inspection.metadata->discoveryNonce : std::string());
    inspection.observations.protocolHealthy =
        probe.has_value() && probe->healthy;
    if (inspection.observations.protocolHealthy &&
        (inspection.metadata || metadataFilePresent)) {
        inspection.observations.identityAvailable = true;
        inspection.observations.identityMatches = inspection.metadata &&
            probe->identityAvailable && probe->edition == "community" &&
            probe->instanceId == inspection.metadata->instanceId &&
            probe->pid == inspection.metadata->pid &&
            probe->dataRootFingerprint ==
                inspection.metadata->dataRootFingerprint &&
            inspection.metadata->port == options.port &&
            inspection.observations.executableIdentityKnown &&
            inspection.observations.executableMatches;
    }
    inspection.observations.portOpen = portIsOpen(options);
    return inspection;
}

void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    const auto wideName = environmentName(name);
    const auto wideValue = utf8ToWide(value);
    if (_wputenv_s(wideName.c_str(), wideValue.c_str()) != 0) {
#else
    if (::setenv(name, value.c_str(), 1) != 0) {
#endif
        throw std::runtime_error(std::string("could not set ") + name);
    }
}

void setDefaultEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    if (!environmentExists(name)) setEnvironment(name, value);
#else
    if (!std::getenv(name)) setEnvironment(name, value);
#endif
}

void writePidFile(const fs::path& path, std::uint64_t pid) {
    fs::path temporary = path;
    temporary += ".tmp." + std::to_string(currentProcessId());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << pid << '\n')) {
            throw std::runtime_error("could not write local engine PID");
        }
        output.flush();
        if (!output) throw std::runtime_error("could not flush local engine PID");
    }
#ifdef _WIN32
    try {
        restrictToCurrentUser(temporary);
    } catch (...) {
        fs::remove(temporary);
        throw;
    }
#endif
#ifndef _WIN32
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        ::unlink(temporary.c_str());
        throw std::system_error(
            errno, std::system_category(), "secure local engine PID");
    }
#endif
    std::error_code error;
#ifdef _WIN32
    fs::remove(path, error);
    error.clear();
#endif
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary);
        throw std::runtime_error(
            "could not publish local engine PID: " + error.message());
    }
}

std::string logTail(const fs::path& path, std::size_t maximum = 8192) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size > static_cast<std::streamoff>(maximum)) {
        input.seekg(size - static_cast<std::streamoff>(maximum));
    } else {
        input.seekg(0);
    }
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[noreturn]] void throwStateError(
    EngineState state,
    const EngineObservations& observations,
    const LocalEngineOptions& options,
    const std::string& detail = {}) {
    const std::string code(engineStateCode(state, observations));
    std::string message = code + ": ";
    switch (state) {
        case EngineState::port_conflict:
            message += "port " + std::to_string(options.port) +
                " is in use by a service that is not PacificDB or does not "
                "match this local engine; "
                "choose another --port";
            break;
        case EngineState::pid_reused:
            message += "engine.pid belongs to a different executable";
            break;
        case EngineState::data_root_in_use:
            message += "DATA_ROOT is owned by another PacificDB instance";
            break;
        case EngineState::exited:
            message += "local engine exited during startup";
            break;
        default:
            message += "local engine could not be verified";
            break;
    }
    if (!detail.empty()) message += ": " + detail;
    throw std::runtime_error(message);
}

bool localHost(std::string_view host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

}  // namespace

void writeNumericPid(const fs::path& pidPath, std::uint64_t pid) {
    if (!pidPath.is_absolute() || pid == 0) {
        throw std::invalid_argument("local engine PID path or value is invalid");
    }
    fs::create_directories(pidPath.parent_path());
    writePidFile(pidPath, pid);
}

LocalEngineResult ensureLocalEngine(
    const LocalEngineOptions& options,
    std::ostream& output) {
    if (!localHost(options.host)) {
        return {EngineState::unhealthy, "engine_remote", false, 0};
    }
    if (options.port < 1 || options.port > 65535) {
        throw std::invalid_argument("local engine port must be from 1 to 65535");
    }
    if (!options.home.is_absolute() || !options.enginePath.is_absolute()) {
        throw std::invalid_argument("local engine paths must be absolute");
    }

    const auto startLock = options.home / ".engine-starting";
    auto inspection = inspectEngine(options);
    auto state = classifyEngineState(inspection.observations);
    if (state == EngineState::pid_reused && fs::is_directory(startLock)) {
        // A freshly forked child can briefly still resolve to the launcher
        // executable before exec() publishes db_engine. Only the active
        // startup lock makes this observation transitional.
        state = EngineState::starting;
    }
    if (!options.autoStart) {
        return {state, std::string(engineStateCode(
            state, inspection.observations)), false, inspection.pid};
    }
    if (state == EngineState::healthy_existing) {
        return {state, std::string(engineStateCode(
            state, inspection.observations)), false, inspection.pid};
    }
    if (state == EngineState::stale_pid) {
        if (inspection.observations.portOpen) {
            throwStateError(EngineState::port_conflict,
                            inspection.observations, options);
        }
        std::error_code ignored;
        fs::remove(options.home / "engine.pid", ignored);
        ignored.clear();
        fs::remove(options.home / "engine.metadata.json", ignored);
        inspection = inspectEngine(options);
        state = classifyEngineState(inspection.observations);
    }
    if (state != EngineState::spawned && state != EngineState::starting) {
        throwStateError(state, inspection.observations, options);
    }

    fs::create_directories(options.home);
    std::error_code lockError;
    const bool ownsStartLock = fs::create_directory(startLock, lockError);
    if (!ownsStartLock) {
        if (lockError) {
            throw std::runtime_error(
                "could not lock local engine startup: " + lockError.message());
        }
        const auto deadline = std::chrono::steady_clock::now() +
            options.startupTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            inspection = inspectEngine(options);
            state = classifyEngineState(inspection.observations);
            if (state == EngineState::healthy_existing) {
                return {state, std::string(engineStateCode(
                    state, inspection.observations)), false, inspection.pid};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        inspection.observations.startupDeadlineExpired = true;
        throwStateError(classifyEngineState(inspection.observations),
                        inspection.observations, options,
                        "another launcher did not finish before the startup deadline");
    }
    struct StartLockOwner {
        fs::path path;
        ~StartLockOwner() {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    } startLockOwner{startLock};

    inspection = inspectEngine(options);
    state = classifyEngineState(inspection.observations);
    if (state == EngineState::healthy_existing) {
        return {state, std::string(engineStateCode(
            state, inspection.observations)), false, inspection.pid};
    }
    if (state == EngineState::starting) {
        const auto deadline = std::chrono::steady_clock::now() +
            options.startupTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            inspection = inspectEngine(options);
            state = classifyEngineState(inspection.observations);
            if (state == EngineState::healthy_existing) {
                return {state, std::string(engineStateCode(
                    state, inspection.observations)), false, inspection.pid};
            }
            if (state == EngineState::stale_pid) {
                std::error_code ignored;
                fs::remove(options.home / "engine.pid", ignored);
                ignored.clear();
                fs::remove(options.home / "engine.metadata.json", ignored);
                break;
            }
            if (state != EngineState::starting) {
                throwStateError(state, inspection.observations, options);
            }
        }
        if (state == EngineState::starting) {
            inspection.observations.startupDeadlineExpired = true;
            throwStateError(classifyEngineState(inspection.observations),
                            inspection.observations, options,
                            "the existing process did not become ready before "
                            "the startup deadline");
        }
    }
    if (inspection.observations.portOpen) {
        throwStateError(EngineState::port_conflict,
                        inspection.observations, options);
    }
    if (!fs::is_regular_file(options.enginePath)) {
        throw std::runtime_error(
            "engine_not_installed: local engine is not installed next to pacificdb");
    }

#ifdef _WIN32
    const auto configuredData = environmentPath("DATA_ROOT");
    const auto configuredBackup = environmentPath("BACKUP_ROOT");
    const auto configuredRestore = environmentPath("RESTORE_DIR");
#else
    const std::optional<fs::path> configuredData = std::getenv("DATA_ROOT")
        ? std::optional<fs::path>(fs::u8path(std::getenv("DATA_ROOT"))) : std::nullopt;
    const std::optional<fs::path> configuredBackup = std::getenv("BACKUP_ROOT")
        ? std::optional<fs::path>(fs::u8path(std::getenv("BACKUP_ROOT"))) : std::nullopt;
    const std::optional<fs::path> configuredRestore = std::getenv("RESTORE_DIR")
        ? std::optional<fs::path>(fs::u8path(std::getenv("RESTORE_DIR"))) : std::nullopt;
#endif
    const auto data = configuredData ? fs::absolute(*configuredData) : options.home / "data";
    const auto backup = configuredBackup ? fs::absolute(*configuredBackup) : options.home / "backup";
    const auto restore = configuredRestore ? fs::absolute(*configuredRestore) : options.home / "restore";
    fs::create_directories(data);
    fs::create_directories(backup);
    fs::create_directories(restore);
    const int raftPort = options.port == 9000
        ? 9100 : std::min(options.port + 1, 65535);
    const std::string instanceId = "engine_" +
        generateDiscoveryToken().substr(0, 32);
    const std::string nonce = generateDiscoveryToken();
    const std::string fingerprint = dataRootFingerprint(data);
    setDefaultEnvironment("PACIFICDB_HOME", options.home.u8string());
    setDefaultEnvironment("PACIFICDB_ENVIRONMENT", "development");
    setDefaultEnvironment("DATA_ROOT", data.u8string());
    setDefaultEnvironment("BACKUP_ROOT", backup.u8string());
    setDefaultEnvironment("RESTORE_DIR", restore.u8string());
    setEnvironment("ENGINE_BIND_HOST", "127.0.0.1");
    setDefaultEnvironment("ENGINE_AUTH_REQUIRED", "0");
    setEnvironment("ENGINE_PORT", std::to_string(options.port));
    setDefaultEnvironment("RAFT_LISTEN_PORT", std::to_string(raftPort));
    setDefaultEnvironment("RAFT_CLUSTER_ID", "pacificdb-local");
    setDefaultEnvironment("RAFT_NODE_ID", "node-1");
    setDefaultEnvironment("RAFT_IS_LEADER", "1");
    setDefaultEnvironment("MIN_QUORUM_SIZE", "1");
    setDefaultEnvironment("ENGINE_CPU_CORES", "2");
    setDefaultEnvironment("ENGINE_KEEPALIVE_MAX_REQUESTS", "1");
    setEnvironment("PACIFICDB_INSTANCE_ID", instanceId);
    setEnvironment("PACIFICDB_DISCOVERY_NONCE", nonce);
    setEnvironment("PACIFICDB_DATA_ROOT_FINGERPRINT", fingerprint);

    const auto logPath = options.home / "engine.log";
    std::uint64_t childPid = 0;
    int earlyExitCode = 0;
    bool childExited = false;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE logHandle = CreateFileW(
        logPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "open local engine log");
    }
    HANDLE nullInput = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        CloseHandle(logHandle);
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "open local engine input");
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributeStorage(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(
            attributeList, 1, 0, &attributeBytes)) {
        const DWORD error = GetLastError();
        CloseHandle(nullInput);
        CloseHandle(logHandle);
        throw std::system_error(
            static_cast<int>(error), std::system_category(),
            "initialize local engine handle list");
    }
    HANDLE inheritedHandles[] = {nullInput, logHandle};
    if (!UpdateProcThreadAttribute(
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attributeList);
        CloseHandle(nullInput);
        CloseHandle(logHandle);
        throw std::system_error(
            static_cast<int>(error), std::system_category(),
            "configure local engine handle list");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = logHandle;
    startup.StartupInfo.hStdError = logHandle;
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + options.enginePath.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    DWORD creationFlags = CREATE_NO_WINDOW | DETACHED_PROCESS |
        CREATE_BREAKAWAY_FROM_JOB | EXTENDED_STARTUPINFO_PRESENT;
    BOOL created = CreateProcessW(
        options.enginePath.c_str(), mutableCommand.data(), nullptr, nullptr,
        TRUE, creationFlags, nullptr,
        options.enginePath.parent_path().c_str(),
        &startup.StartupInfo, &process);
    DWORD spawnError = created ? ERROR_SUCCESS : GetLastError();
    if (!created && (spawnError == ERROR_ACCESS_DENIED ||
                     spawnError == ERROR_NOT_SUPPORTED ||
                     spawnError == ERROR_INVALID_PARAMETER)) {
        mutableCommand.assign(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        creationFlags &= ~CREATE_BREAKAWAY_FROM_JOB;
        created = CreateProcessW(
            options.enginePath.c_str(), mutableCommand.data(), nullptr, nullptr,
            TRUE, creationFlags, nullptr,
            options.enginePath.parent_path().c_str(),
            &startup.StartupInfo, &process);
        spawnError = created ? ERROR_SUCCESS : GetLastError();
    }
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(nullInput);
    CloseHandle(logHandle);
    if (!created) {
        throw std::system_error(
            static_cast<int>(spawnError), std::system_category(),
            "start local engine");
    }
    CloseHandle(process.hThread);
    childPid = process.dwProcessId;
#else
    const int logDescriptor = ::open(
        logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (logDescriptor < 0) {
        throw std::system_error(
            errno, std::system_category(), "open local engine log");
    }
    const pid_t child = ::fork();
    if (child < 0) {
        const int error = errno;
        ::close(logDescriptor);
        throw std::system_error(
            error, std::system_category(), "start local engine");
    }
    if (child == 0) {
        if (::setsid() < 0 ||
            ::dup2(logDescriptor, STDOUT_FILENO) < 0 ||
            ::dup2(logDescriptor, STDERR_FILENO) < 0) {
            _exit(126);
        }
        ::close(logDescriptor);
        ::execl(options.enginePath.c_str(),
                options.enginePath.filename().c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(logDescriptor);
    childPid = static_cast<std::uint64_t>(child);
#endif

    const auto deadline = std::chrono::steady_clock::now() +
        options.startupTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            earlyExitCode = static_cast<int>(code);
            childExited = true;
        }
#else
        int status = 0;
        const auto waited = ::waitpid(
            static_cast<pid_t>(childPid), &status, WNOHANG);
        if (waited == static_cast<pid_t>(childPid)) {
            childExited = true;
            earlyExitCode = WIFEXITED(status)
                ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
#endif
        if (childExited) {
#ifdef _WIN32
            CloseHandle(process.hProcess);
#endif
            EngineObservations exited;
            exited.spawnedProcessExited = true;
            throwStateError(EngineState::exited, exited, options,
                "exit code " + std::to_string(earlyExitCode) +
                "; see " + logPath.u8string() + "\n" + logTail(logPath));
        }
        inspection = inspectEngine(options);
        state = classifyEngineState(inspection.observations);
        if (state == EngineState::healthy_existing) {
#ifdef _WIN32
            CloseHandle(process.hProcess);
#else
            std::thread([pid = static_cast<pid_t>(childPid)] {
                int ignored = 0;
                while (::waitpid(pid, &ignored, 0) < 0 && errno == EINTR) {}
            }).detach();
#endif
            output << "✓ Local engine started at " << options.host << ':'
                   << options.port << '\n';
            return {state, std::string(engineStateCode(
                state, inspection.observations)), true, childPid};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#ifdef _WIN32
    CloseHandle(process.hProcess);
#endif
    inspection.observations.startupDeadlineExpired = true;
    throwStateError(classifyEngineState(inspection.observations),
                    inspection.observations, options,
                    "startup deadline expired; engine was left running; see " +
                    logPath.u8string() + "\n" + logTail(logPath));
}

}  // namespace pacificdb::cli
