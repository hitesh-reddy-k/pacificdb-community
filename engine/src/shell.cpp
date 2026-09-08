#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
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

std::string request(const std::string& host, const std::string& port,
                    const nlohmann::json& command) {
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
    DWORD timeout = 30000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{30, 0};
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
    const auto response = nlohmann::json::parse(request(host, port, payload));
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
                        nlohmann::json command) {
    if (!command.contains("userId")) command["userId"] = "system";
    auto response = nlohmann::json::parse(request(host, port, command));
    if (response.contains("error")) throw std::runtime_error(response.dump());
    return response;
}

void usage() {
    std::cerr << "usage: pacificdb [options] "
                 "ping|request JSON|shell|put-media|get-media|put-vector|query-vector\n";
}

void printShellHelp() {
    std::cout <<
        "Shell commands:\n"
        "  help                         Show this help\n"
        "  quit | exit                  Close the shell\n"
        "  {\"action\":\"ping\"}          Check the server\n"
        "  {\"action\":\"createDatabase\",\"dbName\":\"app\"}\n"
        "  {\"action\":\"createCollection\",\"dbName\":\"app\",\"collection\":\"users\"}\n"
        "  {\"action\":\"insert\",\"dbName\":\"app\",\"collection\":\"users\",\"data\":{\"id\":\"1\",\"name\":\"Ada\"}}\n"
        "  {\"action\":\"find\",\"dbName\":\"app\",\"collection\":\"users\",\"filter\":{}}\n";
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
        int topK = 10;
        nlohmann::json metadata = nlohmann::json::object();
        std::vector<std::string> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--host" || arg == "--port" || arg == "--database" ||
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
        if (positional.empty()) {
            usage();
            return 2;
        }
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
            std::cout << "PacificDB shell. Type help for commands; quit to exit.\n";
            for (std::string line; std::cout << "pacificdb> " && std::getline(std::cin, line);) {
                if (line == "exit" || line == "quit") break;
                if (line.empty()) continue;
                if (line == "help") {
                    printShellHelp();
                    continue;
                }
                try {
                    sendAndPrint(host, port, nlohmann::json::parse(line));
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
