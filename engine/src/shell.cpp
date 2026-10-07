#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include "build_identity.hpp"
#include "community_shell.hpp"
#include "cli_connection.hpp"
#include "local_engine.hpp"

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
#include <optional>
#include <sstream>
#include <set>
#include <regex>
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

class Network {
public:
    Network() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("could not initialize Windows networking");
        }
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
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

pacificdb::cli::ConnectionOptions connection;
std::string authenticationToken;
std::set<std::string> diagnosticSecrets;

bool sensitiveKey(const std::string& key) {
    static const std::regex sensitive("password|token|authorization", std::regex::icase);
    return std::regex_search(key, sensitive);
}

void learnSecrets(const nlohmann::json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (sensitiveKey(it.key()) && it.value().is_string()) {
                const auto secret = it.value().get<std::string>();
                if (!secret.empty()) diagnosticSecrets.insert(secret);
            }
            learnSecrets(it.value());
        }
    } else if (value.is_array()) for (const auto& item : value) learnSecrets(item);
}

std::string redact(std::string text) {
    text = std::regex_replace(text, std::regex(R"(pacificdbs?://[^\s/]*@)"), "pacificdb://[redacted]@");
    auto secrets = diagnosticSecrets;
    secrets.insert(connection.password); secrets.insert(authenticationToken);
    for (const auto& secret : secrets) {
        if (secret.empty()) continue;
        for (std::size_t pos = 0; (pos = text.find(secret, pos)) != std::string::npos;) {
            text.replace(pos, secret.size(), "[redacted]"); pos += 10;
        }
    }
    return text;
}

nlohmann::json redactError(nlohmann::json value) {
    if (value.is_string()) return redact(value.get<std::string>());
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it)
            it.value() = sensitiveKey(it.key()) ? nlohmann::json("[redacted]") : redactError(std::move(it.value()));
    } else if (value.is_array()) for (auto& item : value) item = redactError(std::move(item));
    return value;
}

nlohmann::json parseJson(const std::string& text) {
    auto result = nlohmann::json::parse(text, nullptr, false);
    if (result.is_discarded()) throw std::invalid_argument("invalid JSON");
    return result;
}

bool isLocalHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

#ifdef _WIN32
std::optional<std::filesystem::path> windowsEnvironmentPath(
    const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    std::vector<wchar_t> value(required);
    const DWORD written = GetEnvironmentVariableW(
        name, value.data(), required);
    if (written == 0 || written >= required) return std::nullopt;
    return std::filesystem::path(std::wstring(value.data(), written));
}
#endif

std::filesystem::path localDataHome() {
#ifdef _WIN32
    if (const auto configured = windowsEnvironmentPath(L"PACIFICDB_HOME"))
        return std::filesystem::absolute(*configured);
    if (const auto local = windowsEnvironmentPath(L"LOCALAPPDATA"))
        return *local / "PacificDB";
    if (const auto profile = windowsEnvironmentPath(L"USERPROFILE"))
        return *profile / "AppData" / "Local" / "PacificDB";
#elif defined(__APPLE__)
    if (const char* configured = std::getenv("PACIFICDB_HOME"))
        return std::filesystem::absolute(std::filesystem::u8path(configured));
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / "Library" / "Application Support" / "PacificDB";
#else
    if (const char* configured = std::getenv("PACIFICDB_HOME"))
        return std::filesystem::absolute(std::filesystem::u8path(configured));
    if (const char* data = std::getenv("XDG_DATA_HOME"))
        return std::filesystem::path(data) / "pacificdb";
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "pacificdb";
#endif
    throw std::runtime_error("could not determine PacificDB data directory");
}

std::filesystem::path executablePath(const char* argv0) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (count > 0 && count < buffer.size())
        return std::filesystem::path(std::wstring(buffer.data(), count));
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

std::filesystem::path configuredEnginePath(
    const std::filesystem::path& cliExecutable) {
#ifdef _WIN32
    const auto configured = windowsEnvironmentPath(L"PACIFICDB_ENGINE");
    return configured ? *configured
                      : cliExecutable.parent_path() / "db_engine.exe";
#else
    const char* configured = std::getenv("PACIFICDB_ENGINE");
    return configured && *configured
        ? std::filesystem::u8path(configured)
        : cliExecutable.parent_path() / "db_engine";
#endif
}

std::chrono::milliseconds configuredStartupTimeout() {
    constexpr long long defaultTimeoutMs = 120000;
    const char* configured = std::getenv("PACIFICDB_STARTUP_TIMEOUT_MS");
    if (!configured || !*configured) {
        return std::chrono::milliseconds(defaultTimeoutMs);
    }
    try {
        const long long timeout = std::stoll(configured);
        if (timeout < 100 || timeout > 30LL * 60 * 1000) {
            throw std::out_of_range("startup timeout");
        }
        return std::chrono::milliseconds(timeout);
    } catch (...) {
        throw std::runtime_error(
            "PACIFICDB_STARTUP_TIMEOUT_MS must be from 100 to 1800000");
    }
}

std::string request(const std::string& host, const std::string& port,
                    const nlohmann::json& command) {
    auto options = connection;
    options.host = host; options.port = std::stoi(port);
    auto payload = command;
    learnSecrets(payload);
    if (!payload.contains("token") && !authenticationToken.empty()) payload["token"] = authenticationToken;
    return pacificdb::cli::requestJson(options, payload).dump();
}

nlohmann::json parseServerResponse(const std::string& response,
                                   const std::string& host,
                                   const std::string& port) {
    auto value = nlohmann::json::parse(response, nullptr, false);
    if (value.is_discarded()) {
        throw std::runtime_error("server at " + host + ":" + port +
            " returned a non-JSON response; verify that it is PacificDB");
    }
    return value;
}

int sendAndPrint(const std::string& host, const std::string& port,
                 const nlohmann::json& command) {
    auto payload = command;
    if (!payload.contains("userId")) payload["userId"] = connection.userId;
    auto response = parseServerResponse(request(host, port, payload), host, port);
    if (response.is_object() && response.contains("error")) response = redactError(std::move(response));
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
    if (!command.contains("userId")) command["userId"] = connection.userId;
    if (context) {
        if (!command.contains("dbName") && !context->database.empty())
            command["dbName"] = context->database;
    }
    auto response = parseServerResponse(request(host, port, command), host, port);
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
                 "[shell|ping|stop|request JSON|put-media|get-media|put-vector|query-vector]\n"
                 "       options: --url URL --host HOST --port PORT --database NAME --no-start\n"
                 "                --help, -h  --version, -V\n";
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

pacificdb::cli::ShellContext loadContext(const std::filesystem::path& home,
                                         bool* hadLegacyToken = nullptr) {
    pacificdb::cli::ShellContext context;
    std::ifstream input(home / "context.json");
    if (!input) return context;
    auto value = nlohmann::json::parse(input, nullptr, false);
    if (!value.is_object()) return context;
    if (value.contains("database") && value["database"].is_string()) {
        context.database = value["database"].get<std::string>();
        for (unsigned char c : context.database) if (c < 32 || c == 127) { context.database.clear(); break; }
    }
    if (hadLegacyToken) *hadLegacyToken = value.size() > (value.contains("database") ? 1 : 0);
    ownerOnly(home / "context.json");
    return context;
}

void saveContext(const std::filesystem::path& home,
                 const pacificdb::cli::ShellContext& context) {
    std::filesystem::create_directories(home);
    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1) throw std::runtime_error("could not save CLI context");
    std::ostringstream suffix;
    for (auto byte : random) suffix << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    const auto directory = home / (".context-" + suffix.str());
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("could not save CLI context");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{directory};
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    const auto temporary = directory / "context.json";
    const auto destination = home / "context.json";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("could not save CLI context");
    output << (context.database.empty() ? nlohmann::json::object() :
        nlohmann::json{{"database", context.database}}).dump(2);
    output.flush();
    if (!output) throw std::runtime_error("could not save CLI context");
    output.close();
    if (!output) throw std::runtime_error("could not save CLI context");
    ownerOnly(temporary);
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("could not save CLI context");
#else
    std::filesystem::rename(temporary, destination);
#endif
    ownerOnly(destination);
}

bool safeHistory(std::string line) {
    std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return line.find("password") == std::string::npos &&
           line.find("\"token\"") == std::string::npos &&
           line.find("pdb_") == std::string::npos &&
           line.find("pacificdb://") == std::string::npos &&
           line.find("pacificdbs://") == std::string::npos;
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
    if (!command.contains("userId")) command["userId"] = connection.userId;
    if (!command.contains("dbName")) command["dbName"] = context.database;
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
    const long long mediaMaximum = capabilities.value(
        "media_chunk_source_max_bytes", 4LL * 1024 * 1024);
    if (mediaMaximum < 64 * 1024)
        throw std::runtime_error("engine media chunk limit is too small");
    const std::size_t chunkBytes = static_cast<std::size_t>(std::min<long long>(
        4LL * 1024 * 1024,
        std::min(mediaMaximum, (maximum - 64LL * 1024) * 3 / 4)));
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

    std::set<long long> received;
    if (manifest.contains("received_indices") && manifest["received_indices"].is_array()) {
        for (const auto& value : manifest["received_indices"])
            if (value.is_number_integer() && value.get<long long>() >= 0)
                received.insert(value.get<long long>());
    }
    long long receivedBytes = manifest.value("received_bytes", 0LL);
    auto nextMissing = [&] {
        for (long long candidate = 0; candidate < chunkCount; ++candidate)
            if (!received.count(candidate)) return candidate;
        return chunkCount;
    };
    auto updateProgress = [&](const nlohmann::json& progress, long long storedIndex,
                              std::size_t storedBytes) {
        if (progress.contains("received_indices") && progress["received_indices"].is_array()) {
            received.clear();
            for (const auto& value : progress["received_indices"])
                if (value.is_number_integer() && value.get<long long>() >= 0)
                    received.insert(value.get<long long>());
        } else {
            received.insert(storedIndex);
        }
        if (progress.contains("received_bytes") && progress["received_bytes"].is_number_integer())
            receivedBytes = progress["received_bytes"].get<long long>();
        else
            receivedBytes += static_cast<long long>(storedBytes);
    };
    auto transportFailure = [](const std::exception& error) {
        const std::string message = error.what();
        return message.find("connection") != std::string::npos ||
               message.find("timed out") != std::string::npos ||
               message.find("complete response") != std::string::npos;
    };
    auto interrupted = [&](const std::exception& error) {
        return pacificdb::cli::MediaUploadInterrupted(
            manifest.at("id").get<std::string>(), nextMissing(),
            static_cast<long long>(received.size()), receivedBytes, error.what());
    };

    std::ifstream input(filename, std::ios::binary);
    std::vector<unsigned char> bytes(chunkBytes);
    long long index = 0;
    while (input) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count == 0) break;
        if (received.count(index)) {
            ++index;
            continue;
        }
        std::vector<unsigned char> chunk(bytes.begin(), bytes.begin() +
                                        static_cast<std::ptrdiff_t>(count));
        auto command = withContext({{"action", "community_media_put_chunk"},
            {"media_id", manifest.at("id")}, {"index", index},
            {"data", encodeBase64(chunk)}, {"size_bytes", count},
            {"sha256", sha256Hex(chunk.data(), chunk.size())}}, context);
        if (static_cast<long long>(command.dump().size() + 1) > maximum)
            throw std::runtime_error("serialized media chunk exceeds engine request limit");
        try {
            const auto stored = sendJson(host, port, std::move(command));
            updateProgress(stored.contains("media") ? stored.at("media") : stored,
                           index, count);
        } catch (const std::exception& error) {
            if (transportFailure(error)) throw interrupted(error);
            throw;
        }
        ++index;
    }
    try {
        return sendJson(host, port, withContext({
            {"action", "community_media_finalize"}, {"media_id", manifest.at("id")}},
            context)).at("media");
    } catch (const std::exception& error) {
        if (transportFailure(error)) throw interrupted(error);
        throw;
    }
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
        std::string url;
        bool urlSupplied = false, hostSupplied = false, portSupplied = false, databaseSupplied = false;
        std::string contentType;
        std::string metric = "cosine";
        bool autoStart = true;
        bool showHelp = false;
        bool showVersion = false;
        int topK = 10;
        nlohmann::json metadata = nlohmann::json::object();
        std::vector<std::string> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--no-start") autoStart = false;
            else if (arg == "--help" || arg == "-h") showHelp = true;
            else if (arg == "--version" || arg == "-V") showVersion = true;
            else if (arg == "--url" || arg == "--host" || arg == "--port" || arg == "--database" ||
                arg == "--content-type" || arg == "--metadata" ||
                arg == "--metric" || arg == "--k") {
                if (++i >= argc || std::string(argv[i]).rfind("--", 0) == 0) throw std::runtime_error(arg + " requires a value");
                if (arg == "--url") { url = argv[i]; urlSupplied = true; }
                else if (arg == "--host") { host = argv[i]; hostSupplied = true; }
                else if (arg == "--port") { port = argv[i]; portSupplied = true; }
                else if (arg == "--database") { database = argv[i]; databaseSupplied = true; }
                else if (arg == "--content-type") contentType = argv[i];
                else if (arg == "--metadata") metadata = parseJson(argv[i]);
                else if (arg == "--metric") metric = argv[i];
                else topK = std::stoi(argv[i]);
            } else {
                positional.push_back(arg);
            }
        }
        if (showVersion) {
            std::cout << "PacificDB " << PACIFICDB_ENGINE_VERSION << '\n';
            return 0;
        }
        if (!urlSupplied) { if (const char* environmentUrl = std::getenv("PACIFICDB_URL")) { url = environmentUrl; urlSupplied = true; } }
        if (urlSupplied) {
            connection = pacificdb::cli::parseConnectionUrl(url);
            if ((hostSupplied && host != connection.host) ||
                (portSupplied && port != std::to_string(connection.port)) ||
                (databaseSupplied && database != connection.database)) throw std::invalid_argument("invalid_connection_url: conflicting options");
            host = connection.host; port = std::to_string(connection.port); database = connection.database;
        } else { connection.host = host; connection.database = database; }
        const int numericPort = std::stoi(port);
        connection.port = numericPort;
        if (numericPort < 1 || numericPort > 65535 || port != std::to_string(numericPort)) {
            throw std::runtime_error("port must be an integer from 1 to 65535");
        }
        if (showHelp) {
            usage(std::cout);
            return 0;
        }
        if (positional.empty()) positional.push_back("shell");
        const auto cliExecutable = executablePath(argv[0]);
        const auto engineExecutable = std::filesystem::absolute(
            configuredEnginePath(cliExecutable));
        if (positional[0] == "stop") {
            if (positional.size() != 1 || !isLocalHost(host)) {
                throw std::runtime_error(
                    "stop requires a local host and no positional arguments");
            }
            const auto metadata = pacificdb::cli::readProcessMetadata(
                localDataHome() / "engine.metadata.json");
            if (!metadata || !pacificdb::cli::processIsLive(metadata->pid)) {
                throw std::runtime_error("engine_not_running: no live local engine");
            }
            const auto runningExecutable =
                pacificdb::cli::processExecutable(metadata->pid);
            if (!runningExecutable ||
                !pacificdb::cli::sameExecutable(
                    *runningExecutable, metadata->executable) ||
                !pacificdb::cli::sameExecutable(
                    *runningExecutable, engineExecutable) ||
                metadata->port != numericPort) {
                throw std::runtime_error(
                    "engine_identity_mismatch: refusing to signal the process");
            }
            if (!pacificdb::cli::requestEngineShutdown(metadata->pid)) {
                throw std::runtime_error(
                    "engine_shutdown_unavailable: explicit shutdown event failed");
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(30);
            while (pacificdb::cli::processIsLive(metadata->pid) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (pacificdb::cli::processIsLive(metadata->pid)) {
                throw std::runtime_error(
                    "engine_shutdown_timeout: engine is still running");
            }
            std::cout << nlohmann::json{{"status", "stopped"},
                {"pid", metadata->pid}}.dump() << '\n';
            return 0;
        }
        const std::vector<std::string> commands{"ping", "request", "shell", "put-media",
                                                "get-media", "put-vector", "query-vector"};
        if (std::find(commands.begin(), commands.end(), positional[0]) != commands.end() &&
            isLocalHost(host) && !connection.tls && autoStart) {
            pacificdb::cli::ensureLocalEngine({
                host,
                numericPort,
                localDataHome(),
                engineExecutable,
                autoStart,
                configuredStartupTimeout(),
            }, std::cout);
        }
        if (std::find(commands.begin(), commands.end(), positional[0]) != commands.end() && !connection.username.empty()) {
            const auto response = pacificdb::cli::requestJson(connection, {{"action", "security_authenticate"},
                {"username", connection.username}, {"password", connection.password}});
            if (response.contains("error")) throw std::runtime_error("authentication_failed");
            if (!response.contains("token") || !response["token"].is_string() || response["token"].get<std::string>().empty())
                throw std::runtime_error("invalid authentication response");
            authenticationToken = response["token"].get<std::string>();
        }
        if (positional[0] == "ping" && positional.size() == 1) {
            return sendAndPrint(host, port, {{"action", "ping"}});
        }
        if (positional[0] == "request" && positional.size() >= 2) {
            std::string json = positional[1];
            for (std::size_t i = 2; i < positional.size(); ++i) json += " " + positional[i];
            return sendAndPrint(host, port, parseJson(json));
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
            metadata["vector"] = parseJson(positional[3]);
            return sendAndPrint(host, port, {{"action", "insertVector"},
                {"dbName", database}, {"collection", positional[1]}, {"data", metadata}});
        }
        if (positional[0] == "query-vector" && positional.size() == 3) {
            if (database.empty()) throw std::runtime_error("--database is required");
            return sendAndPrint(host, port, {{"action", "queryVector"},
                {"dbName", database}, {"collection", positional[1]},
                {"vector", parseJson(positional[2])},
                {"k", topK}, {"metric", metric}});
        }
        if (positional[0] == "shell" && positional.size() == 1) {
            const auto home = cliHome();
            bool hadLegacyToken = false;
            auto context = loadContext(home, &hadLegacyToken);
            if (hadLegacyToken) saveContext(home, context);
            bool needsValidation = database.empty() && !context.database.empty();
            if (!database.empty()) context.database = database;
            std::cout << "\n"
                         "             .--------.\n"
                         "          .-'          '-.\n"
                         "         /                \\\n"
                         "         \\____        ____/\n"
                         "       ~~~~~~~~\\______/~~~~~~~~\n"
                         "         ~~~~~~~~~~~~~~~~~~~~\n"
                         "             PacificDB\n"
                         "               v" PACIFICDB_ENGINE_VERSION "\n"
                         "       Documents · Vectors · Media\n"
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
                    const std::set<std::string> databaseActions{"createCollection", "listCollections", "insert", "find", "count", "explain", "aggregate", "updateOne", "deleteOne", "queryVector"};
                    const auto command = parsed.value("command", nlohmann::json::object());
                    if (needsValidation && (kind == "show_database" || kind.rfind("media_", 0) == 0 || kind == "vector_put" ||
                        (databaseActions.count(command.value("action", "")) && !command.contains("dbName")))) {
                        const auto databases = sendJson(host, port, withContext({{"action", "listDatabases"}}, context));
                        if (!databases.is_array() || std::find(databases.begin(), databases.end(), context.database) == databases.end()) throw std::runtime_error("database_not_found");
                        needsValidation = false;
                    }
                    if (kind == "help") printShellHelp();
                    else if (kind == "clear") std::cout << "\033[2J\033[H";
                    else if (kind == "history") {
                        std::ifstream history(home / "history");
                        for (std::string entry; std::getline(history, entry);) if (safeHistory(entry)) std::cout << entry << '\n';
                    } else if (kind == "context_show") {
                        std::cout << nlohmann::json{{"database", context.database.empty()
                            ? nlohmann::json(nullptr) : nlohmann::json(context.database)}}.dump(2) << '\n';
                    } else if (kind == "context_clear") {
                        context = {}; needsValidation = false;
                        saveContext(home, context);
                        std::cout << nlohmann::json{{"status", "ok"}}.dump(2) << '\n';
                    } else if (kind == "use_database") {
                        const auto databases = sendJson(host, port, withContext({{"action", "listDatabases"}}, context));
                        if (!databases.is_array() || std::find(databases.begin(), databases.end(), parsed.at("name")) == databases.end())
                            throw std::runtime_error("database_not_found");
                        context.database = parsed.at("name"); needsValidation = false;
                        saveContext(home, context);
                        std::cout << nlohmann::json{{"status", "ok"}, {"database", context.database}}.dump(2) << '\n';
                    } else if (kind == "create_database") {
                        const std::string name = parsed.at("name");
                        const auto created = sendJson(host, port, withContext({{"action", "createDatabase"}, {"dbName", name}}, context));
                        context.database = name; needsValidation = false;
                        saveContext(home, context);
                        std::cout << created.dump(2) << '\n';
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
                        try {
                            std::cout << uploadMedia(host, port, parsed.at("filename"),
                                parsed.at("collection"), parsed.value("resume", ""), context)
                                .dump(2) << '\n';
                        } catch (const pacificdb::cli::MediaUploadInterrupted& error) {
                            std::cout << error.publicResponse().dump(2) << '\n';
                        }
                    } else if (kind == "media_download") {
                        std::cout << downloadMedia(host, port, parsed.at("id"),
                            parsed.at("filename"), context).dump(2) << '\n';
                    } else if (kind == "media_find") {
                        std::string query = parsed.at("query");
                        std::transform(query.begin(), query.end(), query.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        nlohmann::json matches = nlohmann::json::array();
                        long long offset = 0;
                        while (true) {
                            auto response = sendJson(host, port, withContext({
                                {"action", "community_media_list"}, {"all", true},
                                {"limit", 100}, {"offset", offset}}, context));
                            for (const auto& media : response.value(
                                     "media", nlohmann::json::array())) {
                                std::string haystack = media.value("id", "") + " " +
                                    media.value("filename", "") + " " +
                                    media.value("content_type", "") + " " +
                                    media.value("collection", "");
                                std::transform(haystack.begin(), haystack.end(),
                                    haystack.begin(), [](unsigned char c) {
                                        return static_cast<char>(std::tolower(c));
                                    });
                                if (haystack.find(query) != std::string::npos)
                                    matches.push_back(media);
                            }
                            if (!response.value("has_more", false)) break;
                            const long long next = response.value("next_offset", -1LL);
                            if (next <= offset) throw std::runtime_error("invalid media page");
                            offset = next;
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
                        if (status == 0 && parsed.contains("clear_database") &&
                            context.database == parsed.at("clear_database")) {
                            context.database.clear();
                            saveContext(home, context);
                        }
                    }
                } catch (const std::exception& error) {
                    std::cerr << "error: " << redact(error.what()) << '\n';
                }
            }
            return 0;
        }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << redact(error.what()) << '\n';
        return 1;
    }
}
