#include "socket_runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <system_error>

namespace pacificdb::net {

const char* SocketTimeoutOption::data() const noexcept {
    return reinterpret_cast<const char*>(&value_);
}

int SocketTimeoutOption::size() const noexcept {
    return static_cast<int>(sizeof(value_));
}

long long SocketTimeoutOption::millisecondsForTest() const noexcept {
#ifdef _WIN32
    return static_cast<long long>(value_);
#else
    return static_cast<long long>(value_.tv_sec) * 1000LL +
           static_cast<long long>(value_.tv_usec) / 1000LL;
#endif
}

SocketTimeoutOption makeSocketTimeoutOption(
    std::chrono::milliseconds timeout) noexcept {
    SocketTimeoutOption option;
    const auto milliseconds = std::max<std::chrono::milliseconds::rep>(
        0, timeout.count());

#ifdef _WIN32
    const auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<DWORD>::max());
    option.value_ = static_cast<DWORD>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(milliseconds), maximum));
#else
    using Seconds = decltype(option.value_.tv_sec);
    const auto wholeSeconds = static_cast<std::uint64_t>(milliseconds / 1000);
    const auto maximumSeconds = static_cast<std::uint64_t>(
        std::numeric_limits<Seconds>::max());
    if (wholeSeconds >= maximumSeconds) {
        option.value_.tv_sec = std::numeric_limits<Seconds>::max();
        option.value_.tv_usec = 999999;
    } else {
        option.value_.tv_sec = static_cast<Seconds>(wholeSeconds);
        option.value_.tv_usec = static_cast<decltype(option.value_.tv_usec)>(
            (milliseconds % 1000) * 1000);
    }
#endif
    return option;
}

namespace {

int lastSocketError() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void setSocketTimeout(
    NativeSocket socket,
    int optionName,
    std::chrono::milliseconds timeout,
    const char* operation) {
    const auto option = makeSocketTimeoutOption(timeout);
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            optionName,
            option.data(),
            option.size()) != 0) {
        throw std::system_error(
            lastSocketError(), std::system_category(), operation);
    }
}

}  // namespace

void setSocketTimeouts(
    NativeSocket socket,
    std::chrono::milliseconds receive,
    std::chrono::milliseconds send) {
    setSocketTimeout(socket, SO_RCVTIMEO, receive, "set receive timeout");
    setSocketTimeout(socket, SO_SNDTIMEO, send, "set send timeout");
}

bool waitForSocketWritable(
    NativeSocket socket,
    std::chrono::milliseconds timeout,
    int* nativeError) noexcept {
    auto setError = [nativeError](int error) {
        if (nativeError) *nativeError = error;
    };
    if (socket == invalidSocket) {
#ifdef _WIN32
        setError(WSAENOTSOCK);
#else
        setError(EBADF);
#endif
        return false;
    }

    const auto milliseconds = std::max<std::chrono::milliseconds::rep>(
        0, timeout.count());
    timeval wait{};
    wait.tv_sec = static_cast<decltype(wait.tv_sec)>(milliseconds / 1000);
    wait.tv_usec = static_cast<decltype(wait.tv_usec)>((milliseconds % 1000) * 1000);
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket, &writable);
#ifdef _WIN32
    const int result = ::select(0, nullptr, &writable, nullptr, &wait);
#else
    const int result = ::select(socket + 1, nullptr, &writable, nullptr, &wait);
#endif
    if (result > 0 && FD_ISSET(socket, &writable)) {
        setError(0);
        return true;
    }
    if (result == 0) {
#ifdef _WIN32
        setError(WSAEWOULDBLOCK);
#else
        setError(EAGAIN);
#endif
        return false;
    }
    setError(lastSocketError());
    return false;
}

ReceiveFailure classifyReceiveFailure(int result, int nativeError) noexcept {
    if (result > 0) return ReceiveFailure::none;
    if (result == 0) return ReceiveFailure::disconnected;

#ifdef _WIN32
    if (nativeError == WSAETIMEDOUT || nativeError == WSAEWOULDBLOCK) {
        return ReceiveFailure::timeout;
    }
    if (nativeError == WSAENOTCONN || nativeError == WSAESHUTDOWN) {
        return ReceiveFailure::disconnected;
    }
    if (nativeError == WSAECONNRESET || nativeError == WSAECONNABORTED) {
        return ReceiveFailure::reset;
    }
#else
    if (nativeError == EAGAIN || nativeError == EWOULDBLOCK ||
        nativeError == ETIMEDOUT) {
        return ReceiveFailure::timeout;
    }
    if (nativeError == ENOTCONN) return ReceiveFailure::disconnected;
    if (nativeError == ECONNRESET || nativeError == EPIPE) {
        return ReceiveFailure::reset;
    }
#endif
    return ReceiveFailure::other;
}

}  // namespace pacificdb::net
