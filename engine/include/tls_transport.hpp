#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace pacificdb::transport {

enum class TlsRole { Server, Client };

struct TlsConfig {
    std::string certificatePath;
    std::string privateKeyPath;
    std::string caPath;
    bool verifyPeer = true;
    bool requirePeerCertificate = false;
};

inline std::string drainOpenSslErrors() {
    std::string result;
    for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
        char buffer[256]{};
        ERR_error_string_n(code, buffer, sizeof(buffer));
        if (!result.empty()) result += "; ";
        result += buffer;
    }
    return result.empty() ? "OpenSSL reported no additional detail" : result;
}

class TlsContext {
public:
    static std::shared_ptr<TlsContext> create(TlsRole role,
                                              const TlsConfig& config) {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                             OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                         nullptr);

        const SSL_METHOD* method = role == TlsRole::Server
            ? TLS_server_method()
            : TLS_client_method();
        SSL_CTX* raw = SSL_CTX_new(method);
        if (!raw) {
            throw std::runtime_error("cannot allocate TLS context: " +
                                     drainOpenSslErrors());
        }
        std::shared_ptr<TlsContext> context(new TlsContext(role, config, raw));
        context->configure();
        return context;
    }

    ~TlsContext() {
        if (context_) SSL_CTX_free(context_);
    }

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* native() const noexcept { return context_; }
    TlsRole role() const noexcept { return role_; }
    bool verifiesPeer() const noexcept { return config_.verifyPeer; }

private:
    TlsContext(TlsRole role, TlsConfig config, SSL_CTX* context)
        : role_(role), config_(std::move(config)), context_(context) {}

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(message + ": " + drainOpenSslErrors());
    }

    void configure() {
        if (SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION) != 1) {
            fail("cannot enforce TLS 1.2 minimum");
        }
        SSL_CTX_set_options(context_, SSL_OP_NO_COMPRESSION |
                                         SSL_OP_CIPHER_SERVER_PREFERENCE);
        if (role_ == TlsRole::Server) {
            static constexpr unsigned char sessionContext[] = "pacificdb-engine";
            if (SSL_CTX_set_session_id_context(
                    context_, sessionContext, sizeof(sessionContext) - 1) != 1) {
                fail("cannot configure TLS session context");
            }
        }
        if (SSL_CTX_set_cipher_list(
                context_,
                "ECDHE-ECDSA-AES256-GCM-SHA384:"
                "ECDHE-RSA-AES256-GCM-SHA384:"
                "ECDHE-ECDSA-CHACHA20-POLY1305:"
                "ECDHE-RSA-CHACHA20-POLY1305:"
                "ECDHE-ECDSA-AES128-GCM-SHA256:"
                "ECDHE-RSA-AES128-GCM-SHA256") != 1) {
            fail("cannot configure TLS 1.2 cipher policy");
        }
#ifdef TLS1_3_VERSION
        if (SSL_CTX_set_ciphersuites(
                context_,
                "TLS_AES_256_GCM_SHA384:"
                "TLS_CHACHA20_POLY1305_SHA256:"
                "TLS_AES_128_GCM_SHA256") != 1) {
            fail("cannot configure TLS 1.3 cipher policy");
        }
#endif

        if (!config_.certificatePath.empty()) {
            if (SSL_CTX_use_certificate_chain_file(
                    context_, config_.certificatePath.c_str()) != 1) {
                fail("cannot load TLS certificate chain");
            }
            if (config_.privateKeyPath.empty() ||
                SSL_CTX_use_PrivateKey_file(context_,
                                            config_.privateKeyPath.c_str(),
                                            SSL_FILETYPE_PEM) != 1) {
                fail("cannot load TLS private key");
            }
            if (SSL_CTX_check_private_key(context_) != 1) {
                fail("TLS certificate and private key do not match");
            }
        } else if (role_ == TlsRole::Server || !config_.privateKeyPath.empty()) {
            throw std::runtime_error(
                "TLS certificate and private key must be configured together");
        }

        if (!config_.caPath.empty()) {
            if (SSL_CTX_load_verify_locations(context_, config_.caPath.c_str(),
                                              nullptr) != 1) {
                fail("cannot load TLS CA bundle");
            }
        } else if (config_.verifyPeer || config_.requirePeerCertificate) {
            throw std::runtime_error(
                "peer verification requires an explicit TLS CA bundle");
        }

        int verifyMode = SSL_VERIFY_NONE;
        if (config_.verifyPeer || config_.requirePeerCertificate) {
            verifyMode = SSL_VERIFY_PEER;
        }
        if (role_ == TlsRole::Server && config_.requirePeerCertificate) {
            verifyMode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        }
        SSL_CTX_set_verify(context_, verifyMode, nullptr);
        SSL_CTX_set_verify_depth(context_, 4);
    }

    TlsRole role_;
    TlsConfig config_;
    SSL_CTX* context_ = nullptr;
};

class TlsSession {
public:
    TlsSession(std::shared_ptr<TlsContext> context, int socket)
        : context_(std::move(context)), socket_(socket) {
        if (!context_) throw std::runtime_error("TLS context is required");
        session_ = SSL_new(context_->native());
        if (!session_) {
            throw std::runtime_error("cannot allocate TLS session: " +
                                     drainOpenSslErrors());
        }
        if (SSL_set_fd(session_, socket_) != 1) {
            const std::string detail = drainOpenSslErrors();
            SSL_free(session_);
            session_ = nullptr;
            throw std::runtime_error("cannot attach TLS session to socket: " +
                                     detail);
        }
    }

    ~TlsSession() {
        if (session_) SSL_free(session_);
    }

    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    bool handshake(const std::string& expectedPeer = {}) {
        if (context_->role() == TlsRole::Client && context_->verifiesPeer()) {
            if (expectedPeer.empty()) {
                lastError_ = "verified TLS client requires an expected peer name";
                return false;
            }
            X509_VERIFY_PARAM* verify = SSL_get0_param(session_);
            X509_VERIFY_PARAM_set_hostflags(verify,
                                            X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            unsigned char address[sizeof(in6_addr)]{};
            const bool isIp =
                inet_pton(AF_INET, expectedPeer.c_str(), address) == 1 ||
                inet_pton(AF_INET6, expectedPeer.c_str(), address) == 1;
            if (isIp) {
                if (X509_VERIFY_PARAM_set1_ip_asc(verify,
                                                  expectedPeer.c_str()) != 1) {
                    lastError_ = "cannot configure TLS peer IP verification";
                    return false;
                }
            } else {
                if (SSL_set1_host(session_, expectedPeer.c_str()) != 1 ||
                    SSL_set_tlsext_host_name(session_, expectedPeer.c_str()) != 1) {
                    lastError_ = "cannot configure TLS peer hostname verification";
                    return false;
                }
            }
        }

        for (;;) {
            ERR_clear_error();
            const int result = context_->role() == TlsRole::Server
                ? SSL_accept(session_)
                : SSL_connect(session_);
            if (result == 1) break;
            const int error = SSL_get_error(session_, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                // Callers use blocking sockets with SO_RCVTIMEO/SO_SNDTIMEO.
                // OpenSSL maps an expired socket wait to WANT_*. Retrying here
                // discards the RPC deadline and can block Raft heartbeats forever.
                lastError_ = "TLS handshake socket wait expired";
                errno = EAGAIN;
                return false;
            }
            lastError_ = "TLS handshake failed: " + drainOpenSslErrors();
            return false;
        }

        if (context_->verifiesPeer() &&
            SSL_get_verify_result(session_) != X509_V_OK) {
            lastError_ = "TLS peer certificate verification failed";
            return false;
        }
        return true;
    }

    int read(void* buffer, size_t length) {
        const int bounded = static_cast<int>(
            std::min(length, static_cast<size_t>(INT_MAX)));
        for (;;) {
            ERR_clear_error();
            const int result = SSL_read(session_, buffer, bounded);
            if (result > 0) return result;
            const int error = SSL_get_error(session_, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                lastError_ = "TLS read socket wait expired";
                errno = EAGAIN;
                return -1;
            }
            if (error == SSL_ERROR_ZERO_RETURN) return 0;
            lastError_ = "TLS read failed: " + drainOpenSslErrors();
            errno = EIO;
            return -1;
        }
    }

    int write(const void* buffer, size_t length) {
        const int bounded = static_cast<int>(
            std::min(length, static_cast<size_t>(INT_MAX)));
        for (;;) {
            ERR_clear_error();
            const int result = SSL_write(session_, buffer, bounded);
            if (result > 0) return result;
            const int error = SSL_get_error(session_, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                lastError_ = "TLS write socket wait expired";
                errno = EAGAIN;
                return -1;
            }
            lastError_ = "TLS write failed: " + drainOpenSslErrors();
            errno = EIO;
            return -1;
        }
    }

    void shutdown() noexcept {
        if (!session_) return;
        ERR_clear_error();
        SSL_shutdown(session_);
    }

    const std::string& lastError() const noexcept { return lastError_; }

private:
    std::shared_ptr<TlsContext> context_;
    int socket_ = -1;
    SSL* session_ = nullptr;
    std::string lastError_;
};

}  // namespace pacificdb::transport
