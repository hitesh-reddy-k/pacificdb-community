#include "cli_connection.hpp"
#include "socket_runtime.hpp"
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <climits>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace pacificdb::cli {
namespace {
[[noreturn]] void invalid() { throw std::invalid_argument("invalid_connection_url"); }

std::string decode(std::string value, bool query = false) {
    std::string result;
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned char c = value[i];
        if (c == '%') {
            if (i + 2 >= value.size() || hex(value[i+1]) < 0 || hex(value[i+2]) < 0) invalid();
            c = static_cast<unsigned char>(hex(value[i+1]) * 16 + hex(value[i+2])); i += 2;
        } else if (query && c == '+') c = ' ';
        if (c < 32 || c == 127) invalid();
        result += static_cast<char>(c);
    }
    if (result.empty()) invalid();
    try { (void)nlohmann::json(result).dump(); } catch (...) { invalid(); }
    return result;
}

int integer(const std::string& value, int maximum) {
    if (value.empty()) invalid();
    int result = 0;
    for (char c : value) {
        if (c < '0' || c > '9' || result > (maximum - (c - '0')) / 10) invalid();
        result = result * 10 + c - '0';
    }
    if (result < 1 || result > maximum) invalid();
    return result;
}

using Socket = pacificdb::net::NativeSocket;
struct SocketOwner {
    Socket value = pacificdb::net::invalidSocket;
    ~SocketOwner() { reset(); }
    void reset() {
        if (value == pacificdb::net::invalidSocket) return;
#ifdef _WIN32
        closesocket(value);
#else
        close(value);
#endif
        value = pacificdb::net::invalidSocket;
    }
};
using Clock = std::chrono::steady_clock;

void wait(Socket socket, bool writable, Clock::time_point deadline) {
    for (;;) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (ms <= 0) throw std::runtime_error("request_timeout");
#ifdef _WIN32
        WSAPOLLFD descriptor{socket, static_cast<SHORT>(writable ? POLLWRNORM : POLLRDNORM), 0};
        const int result = WSAPoll(&descriptor, 1, static_cast<int>(ms));
        if (result < 0 && WSAGetLastError() == WSAEINTR) continue;
#else
        pollfd descriptor{socket, static_cast<short>(writable ? POLLOUT : POLLIN), 0};
        const int result = poll(&descriptor, 1, static_cast<int>(ms));
        if (result < 0 && errno == EINTR) continue;
#endif
        if (result == 0) throw std::runtime_error("request_timeout");
        if (result < 0 || descriptor.revents & POLLNVAL) throw std::runtime_error("connection_failed");
        return; // HUP/ERR are handled by the next I/O or SO_ERROR.
    }
}

bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

void nonblocking(Socket s) {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(s, FIONBIO, &enabled) != 0) throw std::runtime_error("connection_failed");
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0) throw std::runtime_error("connection_failed");
#ifdef SO_NOSIGPIPE
    int enabled = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
#endif
}

void sslWait(SSL* ssl, int result, Socket socket, Clock::time_point deadline) {
    const int error = SSL_get_error(ssl, result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        wait(socket, error == SSL_ERROR_WANT_WRITE, deadline); return;
    }
    throw std::runtime_error("tls_connection_failed");
}
} // namespace

ConnectionOptions parseConnectionUrl(const std::string& url) {
    for (unsigned char c : url) if (c <= 32 || c == 127) invalid();
    if (url.find('#') != std::string::npos) invalid();
    std::smatch match;
    if (!std::regex_match(url, match, std::regex(R"(^(pacificdbs?)://([^/?#]+)(/[^?#]*)(?:\?([^#]*))?$)"))) invalid();
    ConnectionOptions o;
    o.tls = match[1] == "pacificdbs";
    o.database = decode(match[3].str().substr(1));
    if (o.database.find_first_of("/\\") != std::string::npos || o.database == "." || o.database == "..") invalid();
    std::string authority = match[2];
    const auto at = authority.find('@');
    if (at != std::string::npos) {
        if (authority.find('@', at + 1) != std::string::npos) invalid();
        const auto info = authority.substr(0, at);
        const auto colon = info.find(':');
        if (colon == std::string::npos) invalid();
        o.username = decode(info.substr(0, colon)); o.password = decode(info.substr(colon + 1));
        authority.erase(0, at + 1);
    }
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string::npos) invalid();
        o.host = authority.substr(1, end - 1);
        unsigned char address[16];
        if (inet_pton(AF_INET6, o.host.c_str(), address) != 1) invalid();
        if (end + 1 < authority.size()) {
            if (authority[end+1] != ':') invalid();
            port = authority.substr(end+2); o.port = integer(port, 65535);
        }
    } else {
        const auto colon = authority.find(':');
        o.host = authority.substr(0, colon);
        if (colon != std::string::npos) o.port = integer(authority.substr(colon+1), 65535);
        if (!std::regex_match(o.host, std::regex(R"([A-Za-z0-9._-]+)"))) invalid();
    }
    std::transform(o.host.begin(), o.host.end(), o.host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string query = match[4]; std::set<std::string> seen;
    for (std::size_t offset = 0; offset < query.size();) {
        const auto end = query.find('&', offset);
        const auto pair = query.substr(offset, end == std::string::npos ? end : end-offset);
        const auto equals = pair.find('=');
        if (equals == std::string::npos) invalid();
        const auto key = decode(pair.substr(0, equals), true), value = decode(pair.substr(equals+1), true);
        if (!seen.insert(key).second) invalid();
        if (key == "userId") o.userId = value;
        else if (key == "caFile") o.caFile = value;
        else if (key == "poolSize") o.poolSize = integer(value, 32);
        else if (key == "timeoutMs") o.timeoutMs = integer(value, 1800000);
        else invalid();
        if (end == std::string::npos) break;
        offset = end + 1; if (offset == query.size()) invalid();
    }
    if (!o.tls && !o.caFile.empty()) invalid();
    return o;
}

nlohmann::json requestJson(const ConnectionOptions& options, const nlohmann::json& command) {
    const auto o = options;
    auto payload = command;
    if (!payload.is_object()) throw std::invalid_argument("request must be a JSON object");
    if (!payload.contains("userId")) payload["userId"] = o.userId;
    if (!payload.contains("dbName") && !o.database.empty()) payload["dbName"] = o.database;
    const auto wire = payload.dump() + '\n';
    const auto deadline = Clock::now() + std::chrono::milliseconds(o.timeoutMs);
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw = nullptr;
    if (getaddrinfo(o.host.c_str(), std::to_string(o.port).c_str(), &hints, &raw) != 0) throw std::runtime_error("host_resolution_failed");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
    SocketOwner socket;
    for (auto* a = raw; a; a = a->ai_next) {
        socket.value = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (socket.value == pacificdb::net::invalidSocket) continue;
        nonblocking(socket.value);
        const int result = ::connect(socket.value, a->ai_addr, static_cast<int>(a->ai_addrlen));
        if (result != 0) {
#ifdef _WIN32
            const int error = WSAGetLastError();
            const bool pending = error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
            const bool pending = errno == EINPROGRESS || errno == EINTR;
#endif
            if (!pending) { socket.reset(); continue; }
            wait(socket.value, true, deadline);
            int error = 0;
#ifdef _WIN32
            int length = sizeof(error);
#else
            socklen_t length = sizeof(error);
#endif
            if (getsockopt(socket.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 || error) { socket.reset(); continue; }
        }
        break;
    }
    if (socket.value == pacificdb::net::invalidSocket) throw std::runtime_error("connection_failed");
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(nullptr, SSL_CTX_free);
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(nullptr, SSL_free);
    if (o.tls) {
        ctx.reset(SSL_CTX_new(TLS_client_method()));
        if (!ctx || SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1) throw std::runtime_error("tls_configuration_failed");
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
        if ((o.caFile.empty() ? SSL_CTX_set_default_verify_paths(ctx.get()) :
             SSL_CTX_load_verify_locations(ctx.get(), o.caFile.c_str(), nullptr)) != 1) throw std::runtime_error("tls_trust_failed");
        ssl.reset(SSL_new(ctx.get()));
        if (!ssl || SSL_set_fd(ssl.get(), static_cast<int>(socket.value)) != 1) throw std::runtime_error("tls_configuration_failed");
        unsigned char address[16];
        const bool ip = inet_pton(AF_INET, o.host.c_str(), address) == 1 || inet_pton(AF_INET6, o.host.c_str(), address) == 1;
        if (ip) {
            if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl.get()), o.host.c_str()) != 1) throw std::runtime_error("tls_configuration_failed");
        } else if (SSL_set1_host(ssl.get(), o.host.c_str()) != 1 ||
                   SSL_set_tlsext_host_name(ssl.get(), o.host.c_str()) != 1) throw std::runtime_error("tls_configuration_failed");
        for (;;) { const int result = SSL_connect(ssl.get()); if (result == 1) break; sslWait(ssl.get(), result, socket.value, deadline); }
        if (SSL_get_verify_result(ssl.get()) != X509_V_OK) throw std::runtime_error("tls_verification_failed");
    }
    std::size_t sent = 0;
    while (sent < wire.size()) {
        if (Clock::now() >= deadline) throw std::runtime_error("request_timeout");
        const int count = ssl ? SSL_write(ssl.get(), wire.data()+sent, static_cast<int>(std::min<std::size_t>(wire.size()-sent, INT_MAX))) :
            ::send(socket.value, wire.data()+sent, static_cast<int>(std::min<std::size_t>(wire.size()-sent, INT_MAX)),
#ifdef MSG_NOSIGNAL
                   MSG_NOSIGNAL
#else
                   0
#endif
            );
        if (count > 0) sent += count;
        else if (ssl) sslWait(ssl.get(), count, socket.value, deadline);
        else if (count < 0 && wouldBlock()) wait(socket.value, true, deadline);
        else throw std::runtime_error("connection_closed; write outcome may be unknown");
    }
    std::string response; char buffer[4096];
    while (response.find('\n') == std::string::npos) {
        if (Clock::now() >= deadline) throw std::runtime_error("request_timeout; write outcome may be unknown");
        const int count = ssl ? SSL_read(ssl.get(), buffer, sizeof(buffer)) : ::recv(socket.value, buffer, sizeof(buffer), 0);
        if (count > 0) response.append(buffer, count);
        else if (ssl) sslWait(ssl.get(), count, socket.value, deadline);
        else if (count < 0 && wouldBlock()) wait(socket.value, false, deadline);
        else throw std::runtime_error("connection_closed; write outcome may be unknown");
        if (response.size() > 16*1024*1024) throw std::runtime_error("response exceeded 16 MiB");
    }
    const auto value = nlohmann::json::parse(response.substr(0, response.find('\n')), nullptr, false);
    if (value.is_discarded()) throw std::runtime_error("invalid JSON response; verify server is PacificDB");
    return value;
}
} // namespace pacificdb::cli
