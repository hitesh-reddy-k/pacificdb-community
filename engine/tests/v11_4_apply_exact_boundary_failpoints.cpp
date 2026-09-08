#include "test_failpoint.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (!pacificdb::test::failpointsCompiled()) {
        std::cerr << "test failpoints were not compiled" << std::endl;
        return 1;
    }
#ifdef _WIN32
    std::cout << "FAILPOINT_COMPILE_GUARD_PASS" << std::endl;
    return 0;
#else
    const fs::path evidence = fs::temp_directory_path()
        / ("pacificdb-failpoint-selftest-" + std::to_string(::getpid()) + ".jsonl");
    const pid_t child = ::fork();
    if (child < 0) return 2;
    if (child == 0) {
        ::setenv("PACIFICDB_TEST_MODE", "apply_exact_boundary", 1);
        ::setenv("PACIFICDB_TEST_FAILPOINT_CONFIRM",
                 "I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE", 1);
        ::setenv("PACIFICDB_TEST_FAILPOINT",
                 "FP_APPLY_BEFORE_BASE_MUTATION", 1);
        ::setenv("PACIFICDB_TEST_FAILPOINT_INDEX", "7", 1);
        ::setenv("PACIFICDB_TEST_FAILPOINT_ACTION", "crash", 1);
        ::setenv("PACIFICDB_TEST_FAILPOINT_EVIDENCE",
                 evidence.c_str(), 1);
        pacificdb::test::hitFailpoint("FP_APPLY_BEFORE_BASE_MUTATION", 6);
        pacificdb::test::hitFailpoint("FP_APPLY_BEFORE_BASE_MUTATION", 7);
        return 3;
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child) return 4;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 86) return 5;
    std::ifstream in(evidence);
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    std::error_code ec;
    fs::remove(evidence, ec);
    if (contents.find("\"raftIndex\":7") == std::string::npos
        || contents.find("FP_APPLY_BEFORE_BASE_MUTATION") == std::string::npos) {
        return 6;
    }
    std::cout << "FAILPOINT_COMPILE_GUARD_AND_EXACT_INDEX_PASS" << std::endl;
    return 0;
#endif
}
