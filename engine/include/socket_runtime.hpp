#pragma once

#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace pacificdb::net {

#ifdef _WIN32
using NativeSocket = SOCKET;
inline constexpr NativeSocket invalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr NativeSocket invalidSocket = -1;
#endif

enum class ReceiveFailure {
    none,
    timeout,
    disconnected,
    reset,
    other,
};

class SocketTimeoutOption {
public:
    const char* data() const noexcept;
    int size() const noexcept;
    long long millisecondsForTest() const noexcept;

private:
#ifdef _WIN32
    DWORD value_ = 0;
#else
    timeval value_{};
#endif

    friend SocketTimeoutOption makeSocketTimeoutOption(
        std::chrono::milliseconds timeout) noexcept;
};

SocketTimeoutOption makeSocketTimeoutOption(
    std::chrono::milliseconds timeout) noexcept;

void setSocketTimeouts(
    NativeSocket socket,
    std::chrono::milliseconds receive,
    std::chrono::milliseconds send);

bool waitForSocketWritable(
    NativeSocket socket,
    std::chrono::milliseconds timeout,
    int* nativeError = nullptr) noexcept;

ReceiveFailure classifyReceiveFailure(int result, int nativeError) noexcept;

}  // namespace pacificdb::net
