#include "../include/query_cancellation.hpp"
#include <thread>

namespace {
    thread_local CancellationTokenPtr tls_token = nullptr;
}

namespace QueryCancel {
    Scope::Scope(CancellationTokenPtr token) : previous_(tls_token) { tls_token = std::move(token); }
    Scope::~Scope() { tls_token = std::move(previous_); }
    void setToken(const CancellationTokenPtr& t) {
        tls_token = t;
    }

    void clearToken() {
        tls_token = nullptr;
    }

    CancellationTokenPtr getToken() {
        return tls_token;
    }

    bool isCancelled() {
        if (!tls_token) return false;
        if (tls_token->cancelled.load()) return true;
        if (tls_token->deadline != std::chrono::steady_clock::time_point()) {
            if (std::chrono::steady_clock::now() > tls_token->deadline) {
                tls_token->cancelled.store(true);
                return true;
            }
        }
        return false;
    }

    CancellationTokenPtr makeToken() {
        return std::make_shared<CancellationToken>();
    }
}
