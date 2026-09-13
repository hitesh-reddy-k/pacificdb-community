#include "socket_runtime.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

int main() {
    using namespace std::chrono_literals;
    using pacificdb::net::ReceiveFailure;

    const auto encoded = pacificdb::net::makeSocketTimeoutOption(2s);
#ifdef _WIN32
    static_assert(sizeof(DWORD) == sizeof(std::uint32_t));
    assert(encoded.size() == static_cast<int>(sizeof(DWORD)));
    assert(encoded.millisecondsForTest() == 2000);
    assert(pacificdb::net::classifyReceiveFailure(-1, WSAETIMEDOUT) ==
           ReceiveFailure::timeout);
    assert(pacificdb::net::classifyReceiveFailure(-1, WSAECONNRESET) ==
           ReceiveFailure::reset);
#else
    assert(encoded.size() == static_cast<int>(sizeof(timeval)));
    assert(encoded.millisecondsForTest() == 2000);
    assert(pacificdb::net::classifyReceiveFailure(-1, EAGAIN) ==
           ReceiveFailure::timeout);
    assert(pacificdb::net::classifyReceiveFailure(-1, ECONNRESET) ==
           ReceiveFailure::reset);
#endif

    assert(pacificdb::net::classifyReceiveFailure(0, 0) ==
           ReceiveFailure::disconnected);
    assert(pacificdb::net::classifyReceiveFailure(1, 0) ==
           ReceiveFailure::none);

    bool rejectedInvalidSocket = false;
    try {
        pacificdb::net::setSocketTimeouts(
            pacificdb::net::invalidSocket, 2s, 2s);
    } catch (const std::system_error&) {
        rejectedInvalidSocket = true;
    }
    assert(rejectedInvalidSocket);

    int waitError = 0;
    assert(!pacificdb::net::waitForSocketWritable(
        pacificdb::net::invalidSocket, 1ms, &waitError));
    assert(waitError != 0);

#ifndef _WIN32
    int connected[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, connected) == 0);
    waitError = 0;
    assert(pacificdb::net::waitForSocketWritable(
        connected[0], 50ms, &waitError));
    assert(waitError == 0);
    ::close(connected[0]);
    ::close(connected[1]);
#endif

    return 0;
}
