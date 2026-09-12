#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include "community_shell.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <termios.h>
using Socket = int;
constexpr Socket invalid_socket = -1;
#endif

namespace {

void closeSocket(Socket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

class Network {
public:
    Network() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("could not initialize Windows networking");
        }
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
    }
    ~Network() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

bool canConnect(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) return false;
    bool connected = false;
    for (auto* address = addresses; address; address = address->ai_next) {
        Socket socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == invalid_socket) continue;
        connected = connect(socket, address->ai_addr,
                            static_cast<int>(address->ai_addrlen)) == 0;
        closeSocket(socket);
        if (connected) break;
    }
    freeaddrinfo(addresses);
    return connected;
}

std::string request(const std::string& host, const std::string& port,
                    const nlohmann::json& command, int timeoutSeconds = 30);

bool engineResponds(const std::string& host, const std::string& port) {
    try {
        const auto response = nlohmann::json::parse(
            request(host, port, {{"action", "ping"}, {"userId", "system"}}, 1));
        return response.value("status", "") == "pong";
    } catch (...) {
        return false;
    }
}

bool isLocalHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

std::filesystem::path localDataHome() {
    if (const char* configured = std::getenv("PACIFICDB_HOME"))
        return std::filesystem::absolute(configured);
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"))
        return std::filesystem::path(local) / "PacificDB";
    if (const char* profile = std::getenv("USERPROFILE"))
        return std::filesystem::path(profile) / "AppData" / "Local" / "PacificDB";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / "Library" / "Application Support" / "PacificDB";
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"))
        return std::filesystem::path(data) / "pacificdb";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "pacificdb";
#endif
    throw std::runtime_error("could not determine PacificDB data directory");
}

void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    if (_putenv_s(name, value.c_str()) != 0)
        throw std::runtime_error(std::string("could not set ") + name);
#else
    if (setenv(name, value.c_str(), 1) != 0)
        throw std::runtime_error(std::string("could not set ") + name);
#endif
}

void setDefaultEnvironment(const char* name, const std::string& value) {
    if (!std::getenv(name)) setEnvironment(name, value);
}

std::filesystem::path executablePath(const char* argv0) {
#ifdef _WIN32
    std::vector<char> buffer(MAX_PATH + 1);
    const DWORD count = GetModuleFileNameA(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (count > 0 && count < buffer.size())
        return std::filesystem::path(std::string(buffer.data(), count));
#elif defined(__linux__)
    std::vector<char> buffer(PATH_MAX + 1);
    const auto count = readlink("/proc/self/exe", buffer.data(), PATH_MAX);
    if (count > 0) return std::filesystem::path(std::string(buffer.data(), count));
#endif
    const std::filesystem::path supplied(argv0);
    if (supplied.has_parent_path()) return std::filesystem::absolute(supplied);
    if (const char* search = std::getenv("PATH")) {
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        std::stringstream paths(search);
        for (std::string directory; std::getline(paths, directory, separator);) {
            const auto candidate = std::filesystem::path(directory) / supplied;
            if (std::filesystem::exists(candidate)) return std::filesystem::absolute(candidate);
        }
    }
    return std::filesystem::absolute(supplied);
}

void ensureLocalEngine(const std::string& host, const std::string& port,
                       const char* argv0, bool autoStart) {
    if (canConnect(host, port) || !autoStart || !isLocalHost(host)) return;

    const auto home = localDataHome();
    const auto data = home / "data";
    const auto backup = home / "backup";
    const auto restore = home / "restore";
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(backup);
    std::filesystem::create_directories(restore);
    const auto startLock = home / ".engine-starting";
    std::error_code lockError;
    if (!std::filesystem::create_directory(startLock, lockError)) {
        if (lockError) throw std::runtime_error("could not lock local engine startup: " +
                                                lockError.message());
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (engineResponds(host, port)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        throw std::runtime_error("another PacificDB process did not finish starting the local engine");
    }
    struct StartLock {
        std::filesystem::path path;
        ~StartLock() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } startLockGuard{startLock};
    const int numericPort = std::stoi(port);
    const int raftPort = numericPort == 9000 ? 9100 : std::min(numericPort + 1, 65535);
    setDefaultEnvironment("PACIFICDB_HOME", home.string());
    setDefaultEnvironment("PACIFICDB_ENVIRONMENT", "development");
    setDefaultEnvironment("DATA_ROOT", data.string());
    setDefaultEnvironment("BACKUP_ROOT", backup.string());
    setDefaultEnvironment("RESTORE_DIR", restore.string());
    setEnvironment("ENGINE_BIND_HOST", "127.0.0.1");
    setDefaultEnvironment("ENGINE_AUTH_REQUIRED", "0");
    setEnvironment("ENGINE_PORT", port);
    setDefaultEnvironment("RAFT_LISTEN_PORT", std::to_string(raftPort));
    setDefaultEnvironment("RAFT_CLUSTER_ID", "pacificdb-local");
    setDefaultEnvironment("RAFT_NODE_ID", "node-1");
    setDefaultEnvironment("RAFT_IS_LEADER", "1");
    setDefaultEnvironment("MIN_QUORUM_SIZE", "1");

    auto engine = executablePath(argv0).parent_path() /
#ifdef _WIN32
        "db_engine.exe";
#else
        "db_engine";
#endif
    if (!std::filesystem::is_regular_file(engine))
        throw std::runtime_error("local engine is not installed next to pacificdb");

    unsigned long long processId = 0;
#ifdef _WIN32
    std::string command = "\"" + engine.string() + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                        &startup, &process)) {
        throw std::runtime_error("could not start local engine");
    }
    processId = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("could not start local engine");
    if (child == 0) {
        if (setsid() < 0) _exit(126);
        const auto log = (home / "engine.log").string();
        const int descriptor = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (descriptor < 0) _exit(126);
        dup2(descriptor, STDOUT_FILENO);
        dup2(descriptor, STDERR_FILENO);
        close(descriptor);
        execl(engine.c_str(), engine.filename().c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    processId = static_cast<unsigned long long>(child);
#endif
    const auto pidFile = home / "engine.pid";
    std::ofstream pid(pidFile, std::ios::trunc);
    if (!pid) throw std::runtime_error("could not write local engine PID");
    pid << processId << '\n';
    pid.close();
#ifndef _WIN32
    chmod(pidFile.c_str(), S_IRUSR | S_IWUSR);
#endif

    for (int attempt = 0; attempt < 300; ++attempt) {
        if (engineResponds(host, port)) {
            std::cout << "✓ Local engine started at " << host << ':' << port << '\n';
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("local engine did not start; see " +
                             (home / "engine.log").string());
}

std::string request(const std::string& host, const std::string& port,
                    const nlohmann::json& command, int timeoutSeconds) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
        throw std::runtime_error("could not resolve host " + host);
    }

    Socket socket = invalid_socket;
    for (auto* address = addresses; address; address = address->ai_next) {
        socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket != invalid_socket &&
            connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            break;
        }
        if (socket != invalid_socket) closeSocket(socket);
        socket = invalid_socket;
    }
    freeaddrinfo(addresses);
    if (socket == invalid_socket) {
        throw std::runtime_error("could not connect to " + host + ":" + port);
    }

#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeoutSeconds * 1000);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{timeoutSeconds, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    const std::string payload = command.dump() + "\n";
    std::size_t sent = 0;
    while (sent < payload.size()) {
        const auto count = send(socket, payload.data() + sent,
                                static_cast<int>(payload.size() - sent), 0);
        if (count <= 0) {
            closeSocket(socket);
            throw std::runtime_error("connection closed while sending request");
        }
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    while (response.find('\n') == std::string::npos) {
        const auto count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            closeSocket(socket);
            throw std::runtime_error("connection closed before a complete response");
        }
        response.append(buffer, static_cast<std::size_t>(count));
        if (response.size() > 16 * 1024 * 1024) {
            closeSocket(socket);
            throw std::runtime_error("response exceeded 16 MiB");
        }
    }
    closeSocket(socket);
    response.resize(response.find('\n'));
    return response;
}

int sendAndPrint(const std::string& host, const std::string& port,
                 const nlohmann::json& command) {
    auto payload = command;
    if (!payload.contains("userId")) payload["userId"] = "system";
    auto response = nlohmann::json::parse(request(host, port, payload));
    const std::string action = payload.value("action", "");
    if (response.is_object() && action.rfind("admin_", 0) != 0) {
        for (auto it = response.begin(); it != response.end();) {
            if (!it.key().empty() && it.key().front() == '_') it = response.erase(it);
            else ++it;
        }
        for (const char* key : {"requestId", "trace_id", "traceparent", "term", "isLeader",
                                "leader_term", "commit_index", "last_applied",
                                "consistency_mode", "consistency_semantics", "client_session_id",
                                "last_seen_version", "minimum_visible_version",
                                "returned_doc_version", "sst_visibility_source"}) {
            response.erase(key);
        }
        auto stripDocumentMetadata = [](nlohmann::json& document) {
            if (!document.is_object()) return;
            for (const char* key : {"_mvcc_commit_ms", "_mvcc_version", "_raft_commit_index",
                                    "_raft_term", "_visibility_floor", "_visibility_state",
                                    "_logicalWritePayloadHash", "created_at_ms", "created_txn",
                                    "deleted_at_ms", "deleted_txn", "version", "committed",
                                    "tenant_id"}) {
                document.erase(key);
            }
        };
        if (response.contains("data")) {
            auto& data = response["data"];
            if (data.is_array()) for (auto& document : data) stripDocumentMetadata(document);
            else stripDocumentMetadata(data);
        }
    }
    std::cout << response.dump(2) << '\n';
    return response.contains("error") ? 1 : 0;
}

std::string encodeBase64(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > INT_MAX) throw std::runtime_error("file is too large");
    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                     bytes.data(), static_cast<int>(bytes.size()));
    if (size < 0) throw std::runtime_error("could not encode file");
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
}

std::vector<unsigned char> decodeBase64(const std::string& encoded) {
    if (encoded.empty()) return {};
    if (encoded.size() % 4 != 0 || encoded.size() > INT_MAX) {
        throw std::runtime_error("database returned invalid base64 media");
    }
    std::vector<unsigned char> bytes(3 * (encoded.size() / 4));
    int size = EVP_DecodeBlock(bytes.data(),
                               reinterpret_cast<const unsigned char*>(encoded.data()),
                               static_cast<int>(encoded.size()));
    if (size < 0) throw std::runtime_error("database returned invalid base64 media");
    if (encoded.back() == '=') --size;
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') --size;
    bytes.resize(static_cast<std::size_t>(size));
    return bytes;
}

nlohmann::json sendJson(const std::string& host, const std::string& port,
                        nlohmann::json command,
                        const pacificdb::cli::ShellContext* context = nullptr) {
    if (!command.contains("userId")) command["userId"] = "system";
    if (context) {
        if (!command.contains("dbName") && !context->database.empty())
            command["dbName"] = context->database;
        if (!command.contains("token") && !context->token.empty())
            command["token"] = context->token;
    }
    auto response = nlohmann::json::parse(request(host, port, command));
    if (response.contains("error")) {
        std::string text = response.at("error").is_string()
            ? response.at("error").get<std::string>() : "request_failed";
        if (response.contains("message") && response.at("message").is_string() &&
            response.at("message").get<std::string>() != text) {
            text += ": " + response.at("message").get<std::string>();
        }
        throw std::runtime_error(text);
    }
    return response;
}

void usage(std::ostream& output = std::cerr) {
    output << "usage: pacificdb [options] "
                 "[shell|ping|request JSON|put-media|get-media|put-vector|query-vector]\n"
                 "       options: --host HOST --port PORT --database NAME --no-start\n";
}

void printShellHelp() {
    std::cout << pacificdb::cli::shellHelp();
}

std::filesystem::path cliHome() {
    if (const char* configured = std::getenv("PACIFICDB_CLI_HOME")) return configured;
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"))
        return std::filesystem::path(local) / "PacificDB";
    if (const char* profile = std::getenv("USERPROFILE"))
        return std::filesystem::path(profile) / "AppData" / "Local" / "PacificDB";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / "Library" / "Application Support" / "PacificDB";
#else
    if (const char* state = std::getenv("XDG_STATE_HOME"))
        return std::filesystem::path(state) / "pacificdb";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "state" / "pacificdb";
#endif
    throw std::runtime_error("could not determine PacificDB CLI data directory");
}

void ownerOnly(const std::filesystem::path& path) {
#ifndef _WIN32
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#else
    (void)path;
#endif
}

pacificdb::cli::ShellContext loadContext(const std::filesystem::path& home) {
    pacificdb::cli::ShellContext context;
    std::ifstream input(home / "context.json");
    if (!input) return context;
    auto value = nlohmann::json::parse(input, nullptr, false);
    if (!value.is_object()) return context;
    context.database = value.value("database", "");
    context.projectId = value.value("projectId", "");
    context.token = value.value("token", "");
    return context;
}

void saveContext(const std::filesystem::path& home,
                 const pacificdb::cli::ShellContext& context) {
    std::filesystem::create_directories(home);
    const auto temporary = home / "context.json.tmp";
    const auto destination = home / "context.json";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("could not save CLI context");
    output << nlohmann::json{{"database", context.database},
                             {"projectId", context.projectId},
                             {"token", context.token}}.dump(2);
    output.flush();
    if (!output) throw std::runtime_error("could not save CLI context");
    output.close();
    if (!output) throw std::runtime_error("could not save CLI context");
    ownerOnly(temporary);
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
    if (error) throw std::runtime_error("could not save CLI context: " + error.message());
    ownerOnly(destination);
}

bool safeHistory(const std::string& line) {
    return line.find("password") == std::string::npos &&
           line.find("\"token\"") == std::string::npos &&
           line.find("pdb_") == std::string::npos;
}

void appendHistory(const std::filesystem::path& home, const std::string& line) {
    if (!safeHistory(line)) return;
    std::filesystem::create_directories(home);
    const auto path = home / "history";
    std::ofstream output(path, std::ios::app);
    if (!output) throw std::runtime_error("could not save CLI history");
    output << line << '\n';
    output.flush();
    if (!output) throw std::runtime_error("could not save CLI history");
    output.close();
    if (!output) throw std::runtime_error("could not save CLI history");
    ownerOnly(path);
}

std::string readPassword() {
    std::string password;
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode);
    if (console) SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
    std::getline(std::cin, password);
    if (console) SetConsoleMode(input, mode);
#else
    termios original{};
    const bool terminal = tcgetattr(STDIN_FILENO, &original) == 0;
    if (terminal) {
        termios hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    }
    std::getline(std::cin, password);
    if (terminal) tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
#endif
    std::cout << '\n';
    return password;
}

std::string sha256Hex(const unsigned char* bytes, std::size_t count) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(bytes, count, digest, &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("could not hash media bytes");
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) result << std::setw(2) << int(digest[i]);
    return result.str();
}

std::string fileSha256(const std::filesystem::path& filename) {
    std::ifstream input(filename, std::ios::binary);
    if (!input) throw std::runtime_error("could not open " + filename.string());
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("could not initialize media checksum");
    std::vector<unsigned char> buffer(1024 * 1024);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        if (input.gcount() > 0 &&
            EVP_DigestUpdate(digest.get(), buffer.data(),
                             static_cast<std::size_t>(input.gcount())) != 1)
            throw std::runtime_error("could not update media checksum");
    }
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(digest.get(), result, &length) != 1)
        throw std::runtime_error("could not finish media checksum");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) out << std::setw(2) << int(result[i]);
    return out.str();
}

std::string contentTypeFor(const std::filesystem::path& filename) {
    std::string extension = filename.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".gif") return "image/gif";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".png") return "image/png";
    if (extension == ".webp") return "image/webp";
    if (extension == ".mp4") return "video/mp4";
    if (extension == ".mov") return "video/quicktime";
    if (extension == ".webm") return "video/webm";
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".wav") return "audio/wav";
    return "application/octet-stream";
}

nlohmann::json withContext(nlohmann::json command,
                           const pacificdb::cli::ShellContext& context) {
    if (!command.contains("userId")) command["userId"] = "system";
    if (!command.contains("dbName") && !context.database.empty())
        command["dbName"] = context.database;
    if (!command.contains("token") && !context.token.empty())
        command["token"] = context.token;
    return command;
}

int sendContextAndPrint(const std::string& host, const std::string& port,
                        nlohmann::json command,
                        const pacificdb::cli::ShellContext& context) {
    return sendAndPrint(host, port, withContext(std::move(command), context));
}

nlohmann::json uploadMedia(const std::string& host, const std::string& port,
                           const std::filesystem::path& filename,
                           const std::string& collection,
                           const std::string& resume,
                           const pacificdb::cli::ShellContext& context) {
    if (context.database.empty()) throw std::runtime_error("select a database first");
    if (!std::filesystem::is_regular_file(filename))
        throw std::runtime_error("media path is not a regular file");
    const auto size = std::filesystem::file_size(filename);
    if (size == 0) throw std::runtime_error("media file must be non-empty");
    const auto capabilities = sendJson(host, port,
        withContext({{"action", "community_capabilities"}}, context));
    const long long maximum = capabilities.value("max_request_bytes", 0LL);
    if (maximum <= 64 * 1024) throw std::runtime_error("engine request limit is too small");
    const std::size_t chunkBytes = static_cast<std::size_t>(std::min<long long>(
        4LL * 1024 * 1024, (maximum - 64LL * 1024) * 3 / 4));
    if (chunkBytes < 64 * 1024) throw std::runtime_error("derived media chunk is too small");
    const auto chunkCount = static_cast<long long>((size + chunkBytes - 1) / chunkBytes);
    const std::string checksum = fileSha256(filename);
    auto begin = nlohmann::json{{"action", "community_media_begin"},
        {"collection", collection}, {"filename", filename.filename().string()},
        {"content_type", contentTypeFor(filename)}, {"size_bytes", size},
        {"chunk_count", chunkCount}, {"sha256", checksum}};
    if (!resume.empty()) begin["resume_id"] = resume;
    auto manifest = sendJson(host, port, withContext(std::move(begin), context)).at("media");
    if (manifest.value("status", "") == "ready") return manifest;

    std::ifstream input(filename, std::ios::binary);
    std::vector<unsigned char> bytes(chunkBytes);
    long long index = 0;
    while (input) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) break;
        std::vector<unsigned char> chunk(bytes.begin(), bytes.begin() +
                                        static_cast<std::ptrdiff_t>(count));
        auto command = withContext({{"action", "community_media_put_chunk"},
            {"media_id", manifest.at("id")}, {"index", index},
            {"data", encodeBase64(chunk)}, {"size_bytes", count},
            {"sha256", sha256Hex(chunk.data(), chunk.size())}}, context);
        if (static_cast<long long>(command.dump().size() + 1) > maximum)
            throw std::runtime_error("serialized media chunk exceeds engine request limit");
        sendJson(host, port, std::move(command));
        ++index;
    }
    return sendJson(host, port, withContext({
        {"action", "community_media_finalize"}, {"media_id", manifest.at("id")}},
        context)).at("media");
}

nlohmann::json downloadMedia(const std::string& host, const std::string& port,
                             const std::string& id,
                             const std::filesystem::path& destination,
                             const pacificdb::cli::ShellContext& context) {
    const auto manifest = sendJson(host, port, withContext({
        {"action", "community_media_get"}, {"media_id", id}}, context)).at("media");
    if (manifest.value("status", "") != "ready")
        throw std::runtime_error("media is not ready");
    const auto partial = destination.string() + ".part";
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not write " + partial);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("could not initialize download checksum");
    long long total = 0;
    try {
        for (long long index = 0; index < manifest.value("chunk_count", 0LL); ++index) {
            const auto chunk = sendJson(host, port, withContext({
                {"action", "community_media_get_chunk"}, {"media_id", id},
                {"index", index}}, context)).at("chunk");
            const auto bytes = decodeBase64(chunk.at("data"));
            if (sha256Hex(bytes.data(), bytes.size()) != chunk.value("sha256", ""))
                throw std::runtime_error("media chunk checksum mismatch");
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            if (!output || EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) != 1)
                throw std::runtime_error("could not write downloaded media");
            total += static_cast<long long>(bytes.size());
        }
        output.close();
        unsigned char raw[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(digest.get(), raw, &length) != 1)
            throw std::runtime_error("could not finish download checksum");
        std::ostringstream checksum;
        checksum << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; ++i) checksum << std::setw(2) << int(raw[i]);
        if (total != manifest.value("size_bytes", -1LL) ||
            checksum.str() != manifest.value("sha256", ""))
            throw std::runtime_error("downloaded media does not match its manifest");
        std::filesystem::rename(partial, destination);
        ownerOnly(destination);
        return {{"status", "ok"}, {"id", id}, {"filename", destination.string()},
                {"size_bytes", total}, {"sha256", checksum.str()}};
    } catch (...) {
        output.close();
        std::filesystem::remove(partial);
        throw;
    }
}

nlohmann::json exportBackup(const std::string& host, const std::string& port,
                            const std::string& id,
                            const std::filesystem::path& destination,
                            const pacificdb::cli::ShellContext& context) {
    const auto manifest = sendJson(host, port, withContext({
        {"action", "export_backup_manifest"}, {"backup_id", id}}, context));
    if (manifest.value("format", "") != "pacificdb-full-backup-v1" ||
        !manifest.contains("backup") || !manifest.value("files", nlohmann::json()).is_array())
        throw std::runtime_error("database returned an invalid backup manifest");

    const std::filesystem::path partial = destination.string() + ".part";
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not write " + partial.string());
    long long total = 0;
    try {
        output << "{\"format\":" << nlohmann::json(manifest.at("format")).dump()
               << ",\"backup\":" << manifest.at("backup").dump() << ",\"files\":[";
        const auto& files = manifest.at("files");
        for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
            const auto& file = files.at(fileIndex);
            if (!file.value("path", nlohmann::json()).is_string() ||
                !file.value("size_bytes", nlohmann::json()).is_number_unsigned() ||
                !file.value("sha256", nlohmann::json()).is_string())
                throw std::runtime_error("database returned an invalid backup file manifest");
            const std::string path = file.at("path");
            const auto size = file.at("size_bytes").get<uint64_t>();
            if (fileIndex) output << ',';
            output << "{\"path\":" << nlohmann::json(path).dump()
                   << ",\"size_bytes\":" << size
                   << ",\"sha256\":" << nlohmann::json(file.at("sha256")).dump()
                   << ",\"chunks\":[";

            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
                EVP_MD_CTX_new(), EVP_MD_CTX_free);
            if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
                throw std::runtime_error("could not initialize backup checksum");
            uint64_t offset = 0;
            std::size_t chunkIndex = 0;
            while (offset < size) {
                const auto chunk = sendJson(host, port, withContext({
                    {"action", "export_backup_file_chunk"}, {"backup_id", id},
                    {"path", path}, {"offset", offset},
                    {"max_bytes", std::min<uint64_t>(1024 * 1024, size - offset)}},
                    context));
                const auto bytes = decodeBase64(chunk.value("data", ""));
                if (bytes.empty() || chunk.value("offset", uint64_t(-1)) != offset ||
                    chunk.value("size_bytes", uint64_t(-1)) != bytes.size() ||
                    chunk.value("sha256", "") != sha256Hex(bytes.data(), bytes.size()))
                    throw std::runtime_error("invalid backup chunk: " + path);
                if (chunkIndex++) output << ',';
                output << nlohmann::json(chunk.at("data")).dump();
                if (EVP_DigestUpdate(digest.get(), bytes.data(), bytes.size()) != 1)
                    throw std::runtime_error("could not update backup checksum");
                offset += bytes.size();
                total += static_cast<long long>(bytes.size());
            }
            unsigned char raw[EVP_MAX_MD_SIZE];
            unsigned int length = 0;
            if (EVP_DigestFinal_ex(digest.get(), raw, &length) != 1)
                throw std::runtime_error("could not finish backup checksum");
            std::ostringstream checksum;
            checksum << std::hex << std::setfill('0');
            for (unsigned int i = 0; i < length; ++i)
                checksum << std::setw(2) << int(raw[i]);
            if (checksum.str() != file.at("sha256").get<std::string>())
                throw std::runtime_error("backup file checksum mismatch: " + path);
            output << "]}";
        }
        output << "]}\n";
        output.close();
        if (!output) throw std::runtime_error("could not finish backup export");
        std::error_code error;
        std::filesystem::rename(partial, destination, error);
        if (error) {
            std::filesystem::remove(destination, error);
            error.clear();
            std::filesystem::rename(partial, destination, error);
        }
        if (error) throw std::runtime_error("could not save backup export: " + error.message());
        ownerOnly(destination);
        return {{"status", "ok"}, {"backup_id", id},
                {"filename", destination.string()}, {"files", files.size()},
                {"size_bytes", total}};
    } catch (...) {
        output.close();
        std::filesystem::remove(partial);
        throw;
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Network network;
        std::string host = "127.0.0.1";
        std::string port = "9000";
        std::string database;
        std::string contentType;
        std::string metric = "cosine";
        bool autoStart = true;
        bool showHelp = false;
        int topK = 10;
        nlohmann::json metadata = nlohmann::json::object();
        std::vector<std::string> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--no-start") autoStart = false;
            else if (arg == "--help" || arg == "-h") showHelp = true;
            else if (arg == "--host" || arg == "--port" || arg == "--database" ||
                arg == "--content-type" || arg == "--metadata" ||
                arg == "--metric" || arg == "--k") {
                if (++i >= argc) throw std::runtime_error(arg + " requires a value");
                if (arg == "--host") host = argv[i];
                else if (arg == "--port") port = argv[i];
                else if (arg == "--database") database = argv[i];
                else if (arg == "--content-type") contentType = argv[i];
                else if (arg == "--metadata") metadata = nlohmann::json::parse(argv[i]);
                else if (arg == "--metric") metric = argv[i];
                else topK = std::stoi(argv[i]);
            } else {
                positional.push_back(arg);
            }
        }
        const int numericPort = std::stoi(port);
        if (numericPort < 1 || numericPort > 65535 || port != std::to_string(numericPort)) {
            throw std::runtime_error("port must be an integer from 1 to 65535");
        }
        if (showHelp) {
            usage(std::cout);
            return 0;
        }
        if (positional.empty()) positional.push_back("shell");
        const std::vector<std::string> commands{"ping", "request", "shell", "put-media",
                                                "get-media", "put-vector", "query-vector"};
        if (std::find(commands.begin(), commands.end(), positional[0]) != commands.end())
            ensureLocalEngine(host, port, argv[0], autoStart);
        if (positional[0] == "ping" && positional.size() == 1) {
            return sendAndPrint(host, port, {{"action", "ping"}});
        }
        if (positional[0] == "request" && positional.size() >= 2) {
            std::string json = positional[1];
            for (std::size_t i = 2; i < positional.size(); ++i) json += " " + positional[i];
            return sendAndPrint(host, port, nlohmann::json::parse(json));
        }
        if (positional[0] == "put-media" && positional.size() == 4) {
            if (database.empty()) throw std::runtime_error("--database is required");
            if (!metadata.is_object()) throw std::runtime_error("--metadata must be a JSON object");
            std::ifstream input(positional[3], std::ios::binary);
            if (!input) throw std::runtime_error("could not open " + positional[3]);
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
            metadata["filename"] = std::filesystem::path(positional[3]).filename().string();
            if (!contentType.empty()) metadata["contentType"] = contentType;
            metadata["id"] = positional[2];
            metadata["kind"] = "media";
            metadata["dataBase64"] = encodeBase64(bytes);
            metadata["sizeBytes"] = bytes.size();
            metadata["encoding"] = "base64";
            return sendAndPrint(host, port, {{"action", "insert"},
                {"dbName", database}, {"collection", positional[1]}, {"data", metadata}});
        }
        if (positional[0] == "get-media" && positional.size() == 4) {
            if (database.empty()) throw std::runtime_error("--database is required");
            auto response = sendJson(host, port, {{"action", "find"},
                {"dbName", database}, {"collection", positional[1]},
                {"filter", {{"id", positional[2]}}}, {"limit", 1}});
            if (!response.contains("data") || !response["data"].is_array() ||
                response["data"].empty() ||
                !response["data"][0].contains("dataBase64")) {
                throw std::runtime_error("media not found: " + positional[2]);
            }
            const auto bytes = decodeBase64(response["data"][0]["dataBase64"].get<std::string>());
            std::ofstream output(positional[3], std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("could not write " + positional[3]);
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            if (!output) throw std::runtime_error("could not write " + positional[3]);
            std::cout << nlohmann::json{{"status", "ok"}, {"id", positional[2]},
                {"filename", positional[3]}, {"sizeBytes", bytes.size()}}.dump(2) << '\n';
            return 0;
        }
        if (positional[0] == "put-vector" && positional.size() == 4) {
            if (database.empty()) throw std::runtime_error("--database is required");
            if (!metadata.is_object()) throw std::runtime_error("--metadata must be a JSON object");
            metadata["id"] = positional[2];
            metadata["kind"] = "vector";
            metadata["vector"] = nlohmann::json::parse(positional[3]);
            return sendAndPrint(host, port, {{"action", "insertVector"},
                {"dbName", database}, {"collection", positional[1]}, {"data", metadata}});
        }
        if (positional[0] == "query-vector" && positional.size() == 3) {
            if (database.empty()) throw std::runtime_error("--database is required");
            return sendAndPrint(host, port, {{"action", "queryVector"},
                {"dbName", database}, {"collection", positional[1]},
                {"vector", nlohmann::json::parse(positional[2])},
                {"k", topK}, {"metric", metric}});
        }
        if (positional[0] == "shell" && positional.size() == 1) {
            const auto home = cliHome();
            auto context = loadContext(home);
            if (!database.empty()) context.database = database;
            std::cout << "\n"
                         "  ╭────────────────────────────────────╮\n"
                         "  │  ≋  PACIFICDB  ·  COMMUNITY BETA  │\n"
                         "  │     Documents · Vectors · Media    │\n"
                         "  ╰────────────────────────────────────╯\n"
                         "  Type help to see commands.\n";
            for (std::string line;
                 std::cout << (context.database.empty() ? "pacificdb> " :
                     "pacificdb:" + context.database + "> ") && std::getline(std::cin, line);) {
                if (line.empty()) continue;
                try {
                    auto parsed = pacificdb::cli::parseShellCommand(line, context);
                    const std::string kind = parsed.value("kind", "");
                    if (kind == "exit") break;
                    appendHistory(home, line);
                    if (kind == "help") printShellHelp();
                    else if (kind == "clear") std::cout << "\033[2J\033[H";
                    else if (kind == "history") {
                        std::ifstream history(home / "history");
                        std::cout << history.rdbuf();
                    } else if (kind == "context_show") {
                        std::cout << nlohmann::json{{"database", context.database.empty()
                            ? nlohmann::json(nullptr) : nlohmann::json(context.database)},
                            {"projectId", context.projectId.empty()
                            ? nlohmann::json(nullptr) : nlohmann::json(context.projectId)},
                            {"authenticated", !context.token.empty()}}.dump(2) << '\n';
                    } else if (kind == "context_clear" || kind == "logout") {
                        context = {};
                        saveContext(home, context);
                        std::cout << nlohmann::json{{"status", "ok"}}.dump(2) << '\n';
                    } else if (kind == "login") {
                        std::cout << "Password: " << std::flush;
                        const auto result = sendJson(host, port, {{"action", "security_authenticate"},
                            {"username", parsed.value("username", "")},
                            {"password", readPassword()}});
                        context.token = result.at("token");
                        saveContext(home, context);
                        std::cout << nlohmann::json{{"status", "ok"},
                            {"username", result.value("username", "")},
                            {"role", result.value("role", "")}}.dump(2) << '\n';
                    } else if (kind == "use_project") {
                        const auto response = sendJson(host, port, withContext({
                            {"action", "community_project_get"},
                            {"id", parsed.at("id")}}, context));
                        if (!response.contains("project") ||
                            !response.at("project").is_object() ||
                            response.at("project").value("id", "").empty()) {
                            throw std::runtime_error("project_not_found");
                        }
                        context.projectId = response.at("project").at("id");
                        saveContext(home, context);
                        std::cout << nlohmann::json{{"status", "ok"},
                            {"projectId", context.projectId}}.dump(2) << '\n';
                    } else if (kind == "use_database") {
                        const auto response = sendJson(host, port, withContext(
                            {{"action", "listDatabases"}}, context));
                        if (!response.is_array() ||
                            std::find(response.begin(), response.end(), parsed.at("name")) ==
                                response.end()) {
                            throw std::runtime_error("database_not_found");
                        }
                        context.database = parsed.at("name");
                        saveContext(home, context);
                        std::cout << nlohmann::json{{"status", "ok"},
                            {"database", context.database}}.dump(2) << '\n';
                    } else if (kind == "create_database") {
                        const std::string name = parsed.at("name");
                        sendContextAndPrint(host, port,
                            {{"action", "createDatabase"}, {"dbName", name}}, context);
                        if (!context.projectId.empty()) {
                            sendJson(host, port, withContext({
                                {"action", "community_database_map"},
                                {"database", name}, {"project_id", context.projectId}}, context));
                        }
                    } else if (kind == "show_database") {
                        const auto collections = sendJson(host, port, withContext(
                            {{"action", "listCollections"}}, context));
                        std::cout << nlohmann::json{{"status", "ok"},
                            {"database", context.database},
                            {"collections", collections.contains("collections")
                                ? collections["collections"] : collections}}.dump(2) << '\n';
                    } else if (kind == "backup_export") {
                        const std::filesystem::path filename = parsed.value("filename", "").empty()
                            ? parsed.at("id").get<std::string>() + ".json"
                            : parsed.at("filename").get<std::string>();
                        std::cout << exportBackup(host, port, parsed.at("id"),
                            filename, context).dump(2) << '\n';
                    } else if (kind == "media_upload") {
                        std::cout << uploadMedia(host, port, parsed.at("filename"),
                            parsed.at("collection"), parsed.value("resume", ""), context)
                            .dump(2) << '\n';
                    } else if (kind == "media_download") {
                        std::cout << downloadMedia(host, port, parsed.at("id"),
                            parsed.at("filename"), context).dump(2) << '\n';
                    } else if (kind == "media_find") {
                        auto response = sendJson(host, port, withContext({
                            {"action", "community_media_list"}, {"all", true}}, context));
                        std::string query = parsed.at("query");
                        std::transform(query.begin(), query.end(), query.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        nlohmann::json matches = nlohmann::json::array();
                        for (const auto& media : response.value("media", nlohmann::json::array())) {
                            std::string haystack = media.value("id", "") + " " +
                                media.value("filename", "") + " " +
                                media.value("content_type", "") + " " +
                                media.value("collection", "");
                            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if (haystack.find(query) != std::string::npos) matches.push_back(media);
                        }
                        std::cout << nlohmann::json{{"status", "ok"},
                            {"count", matches.size()}, {"media", matches}}.dump(2) << '\n';
                    } else if (kind == "vector_put") {
                        sendContextAndPrint(host, port, {{"action", "insertVector"},
                            {"collection", parsed.at("collection")}, {"data", {
                                {"id", parsed.at("id")}, {"kind", "vector"},
                                {"vector", parsed.at("vector")}}}}, context);
                    } else if (kind == "request") {
                        const auto command = parsed.at("command");
                        const int status = sendContextAndPrint(host, port, command, context);
                        if (status == 0 && parsed.contains("clear_project") &&
                            context.projectId == parsed.at("clear_project")) {
                            context.projectId.clear();
                            saveContext(home, context);
                        }
                        if (status == 0 && parsed.contains("clear_database") &&
                            context.database == parsed.at("clear_database")) {
                            context.database.clear();
                            saveContext(home, context);
                        }
                    }
                } catch (const std::exception& error) {
                    std::cerr << "error: " << error.what() << '\n';
                }
            }
            return 0;
        }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
