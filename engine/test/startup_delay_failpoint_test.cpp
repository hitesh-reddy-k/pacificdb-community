#include "test_failpoint.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void environment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}
long long elapsed() {
    const auto begin = std::chrono::steady_clock::now();
    pacificdb::test::hitFailpoint("FP_SHUTDOWN_BEFORE_LISTENER_BIND", 1);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
}
}  // namespace

int main() {
    environment("PACIFICDB_TEST_MODE", "local_engine_startup");
    environment("PACIFICDB_TEST_FAILPOINT", "FP_SHUTDOWN_BEFORE_LISTENER_BIND");
    environment("PACIFICDB_TEST_FAILPOINT_INDEX", "1");
    environment("PACIFICDB_TEST_FAILPOINT_ACTION", "delay");
    environment("PACIFICDB_TEST_STARTUP_DELAY_MS", "250");
    environment("PACIFICDB_TEST_FAILPOINT_CONFIRM", nullptr);
    assert(elapsed() < 100);

    environment("PACIFICDB_TEST_STARTUP_DELAY_MS", "25");
    environment("PACIFICDB_TEST_FAILPOINT_CONFIRM",
        "I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE");
    assert(elapsed() >= 20);
    std::cout << "STARTUP_DELAY_FAILPOINT_PASS\n";
}
