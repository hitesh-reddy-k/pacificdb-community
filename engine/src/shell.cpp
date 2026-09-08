#include <nlohmann/json.hpp>

#include <csignal>
#include <cstring>
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

void usage() {
    std::cerr << "usage: pacificdb [--host HOST] [--port PORT] "
                 "ping|request JSON|shell\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Network network;
        std::string host = "127.0.0.1";
        std::string port = "9000";
        std::vector<std::string> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--host" || arg == "--port") {
                if (++i >= argc) throw std::runtime_error(arg + " requires a value");
                (arg == "--host" ? host : port) = argv[i];
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
        if (positional[0] == "shell" && positional.size() == 1) {
            for (std::string line; std::cout << "pacificdb> " && std::getline(std::cin, line);) {
                if (line == "exit" || line == "quit") break;
                if (line.empty()) continue;
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
