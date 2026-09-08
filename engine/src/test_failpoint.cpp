#include "test_failpoint.hpp"

#ifdef PACIFICDB_TEST_FAILPOINTS

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kRequiredConfirmation =
    "I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE";

std::mutex g_failpointMutex;
std::set<std::string> g_fired;
volatile std::sig_atomic_t g_shutdownRequested = 0;

bool exactEnv(const char* name, const char* expected) {
    const char* value = std::getenv(name);
    return value && std::strcmp(value, expected) == 0;
}

bool allowedTestMode() {
    const char* mode = std::getenv("PACIFICDB_TEST_MODE");
    if (!mode) return false;
    return std::strcmp(mode, "apply_exact_boundary") == 0
        || std::strcmp(mode, "engine_shutdown_completion") == 0
        || std::strcmp(mode, "div001_progress_guard") == 0
        || std::strcmp(mode, "div001_durability_crash") == 0
        || std::strcmp(mode, "div001_safety_fences") == 0
        || std::strcmp(mode, "lsm_checkpoint_crash") == 0;
}

std::uint64_t selectedIndex() {
    const char* value = std::getenv("PACIFICDB_TEST_FAILPOINT_INDEX");
    if (!value || !*value) return 0;
    try {
        return static_cast<std::uint64_t>(std::stoull(value));
    } catch (...) {
        return 0;
    }
}

void appendEvidence(const std::string& name,
                    std::uint64_t index,
                    const std::string& action) {
    const char* evidence = std::getenv("PACIFICDB_TEST_FAILPOINT_EVIDENCE");
    if (!evidence || !*evidence) return;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string line =
        "{\"event\":\"failpoint_reached\",\"name\":\"" + name
        + "\",\"raftIndex\":" + std::to_string(index)
        + ",\"action\":\"" + action
        + "\",\"timestampMs\":" + std::to_string(now) + "}\n";
#ifndef _WIN32
    const int fd = ::open(evidence, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd >= 0) {
        const char* cursor = line.data();
        std::size_t remaining = line.size();
        while (remaining > 0) {
            const ssize_t written = ::write(fd, cursor, remaining);
            if (written <= 0) break;
            cursor += written;
            remaining -= static_cast<std::size_t>(written);
        }
        ::fsync(fd);
        ::close(fd);
    }
#else
    std::ofstream out(evidence, std::ios::app);
    out << line;
    out.flush();
#endif
}

void waitAtNamedBarrier() {
    const char* fifo = std::getenv("PACIFICDB_TEST_FAILPOINT_BARRIER_FIFO");
    if (!fifo || !*fifo) {
        std::cerr << "[TEST_FAILPOINT] pause requested without named FIFO" << std::endl;
        std::_Exit(87);
    }
#ifndef _WIN32
    const int fd = ::open(fifo, O_RDONLY | O_NONBLOCK);
    if (fd < 0) std::_Exit(87);
    ssize_t result = -1;
    while (!g_shutdownRequested) {
        char byte = 0;
        result = ::read(fd, &byte, 1);
        if (result == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    if (result != 1 && !g_shutdownRequested) std::_Exit(87);
#else
    std::ifstream in(fifo, std::ios::binary);
    char byte = 0;
    if (!in.get(byte)) std::_Exit(87);
#endif
}

}  // namespace

namespace pacificdb::test {

bool failpointsCompiled() {
    return true;
}

void requestShutdown() noexcept {
    g_shutdownRequested = 1;
}

// V11.4-DIV-001. Deliberately does NOT dedupe through g_fired: recovery replay of the same
// committed index must fail on every restart, otherwise a second attempt would silently
// succeed and the test could not prove the engine refuses to skip the failed record.
bool injectApplyFailure(const char* name, std::uint64_t raftIndex) {
    if (!name || !*name) return false;
    if (!allowedTestMode()
        || !exactEnv("PACIFICDB_TEST_FAILPOINT_CONFIRM", kRequiredConfirmation)) {
        return false;
    }
    const char* selected = std::getenv("PACIFICDB_TEST_FAILPOINT");
    if (!selected || std::strcmp(selected, name) != 0) return false;
    const std::uint64_t wanted = selectedIndex();
    if (wanted == 0 || wanted != raftIndex) return false;
    appendEvidence(name, raftIndex, "inject_apply_failure");
    std::cerr << "[TEST_FAILPOINT] injecting apply failure name=" << name
              << " raftIndex=" << raftIndex << std::endl;
    return true;
}

void hitFailpoint(const char* name, std::uint64_t raftIndex) {
    if (!name || !*name) return;
    if (!allowedTestMode()
        || !exactEnv("PACIFICDB_TEST_FAILPOINT_CONFIRM", kRequiredConfirmation)) {
        return;
    }
    const char* selected = std::getenv("PACIFICDB_TEST_FAILPOINT");
    if (!selected || std::strcmp(selected, name) != 0) return;
    const std::uint64_t wanted = selectedIndex();
    if (wanted == 0 || wanted != raftIndex) return;

    const std::string key = std::string(name) + ":" + std::to_string(raftIndex);
    {
        std::lock_guard<std::mutex> lock(g_failpointMutex);
        if (!g_fired.insert(key).second) return;
    }

    const char* configuredAction = std::getenv("PACIFICDB_TEST_FAILPOINT_ACTION");
    const std::string action = configuredAction ? configuredAction : "crash";
    appendEvidence(name, raftIndex, action);
    std::cerr << "[TEST_FAILPOINT] reached name=" << name
              << " raftIndex=" << raftIndex
              << " action=" << action << std::endl;
    if (action == "pause") {
        waitAtNamedBarrier();
        return;
    }
    if (action == "record") return;
    if (action != "crash") std::_Exit(87);
    std::_Exit(86);
}

}  // namespace pacificdb::test

#else

namespace pacificdb::test {

bool failpointsCompiled() {
    return false;
}

void requestShutdown() noexcept {}

void hitFailpoint(const char*, std::uint64_t) {}

bool injectApplyFailure(const char*, std::uint64_t) {
    return false;
}

}  // namespace pacificdb::test

#endif
