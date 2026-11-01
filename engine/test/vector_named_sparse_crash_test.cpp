#include "database_engine.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#endif

static json sparse() {
    return {{"format", "sparse-v1"}, {"precision", "float64"}, {"dimensions", 1000000000},
            {"indices", {0, 999999999}}, {"values", {0.123456789012345, -2.}}};
}
int main(int argc, char** argv) {
#ifdef _WIN32
    std::cout << "BLOCKED: SIGKILL recovery fixture requires POSIX\n"; return 77;
#else
    setenv("WAL_ENABLED", "true", 1);
    setenv("WAL_FSYNC_ENABLED", "true", 1);
    setenv("WAL_FLUSH_ON_COMMIT", "true", 1);
    setenv("WAL_GROUP_COMMIT", "false", 1);
    if (argc == 3 && std::string(argv[1]) == "--write-child") {
        DatabaseEngine::init(argv[2]);
        DatabaseEngine::ensureUserRoot("alice");
        DatabaseEngine::createDatabase("alice", "app", "vector");
        DatabaseEngine::createCollection("alice", "app", "vectors");
        DatabaseEngine::insertVector("alice", "app", "vectors", {{"id", "named"},
            {"vectors", {{"dense", {0.123456789012345, 1.}}, {"terms", sparse()}}}});
        DatabaseEngine::insertVector("alice", "app", "vectors", {{"id", "sparse"}, {"vector", sparse()}});
        // Acknowledged writes reached WAL; no graceful shutdown or explicit SST flush.
        ::kill(::getpid(), SIGKILL);
        return 1;
    }
    OwnedTestRoot root("named-sparse-crash");
    const std::string path = root.dataRoot().string();
    const pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        execl(argv[0], argv[0], "--write-child", path.c_str(), static_cast<char*>(nullptr));
        _exit(2);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    DatabaseEngine::init(path);
    const auto documents = DatabaseEngine::find("alice", "app", "vectors", {});
    assert(documents.size() == 2);
    const json request{{"vector", sparse()}, {"vector_name", "terms"},
                       {"metric", "dot"}, {"k", 10}, {"search_mode", "exact"}};
    const auto hits = DatabaseEngine::queryVector("alice", "app", "vectors", request);
    assert(hits.size() == 1 && hits.front().at("id") == "named");
    assert(hits.front().at("vectors").at("terms") == sparse());
    assert(hits.front().at("vectors").at("dense")[0] == 0.123456789012345);
    auto defaultRequest = request; defaultRequest.erase("vector_name");
    const auto defaultHits = DatabaseEngine::queryVector("alice", "app", "vectors", defaultRequest);
    assert(defaultHits.size() == 1 && defaultHits.front().at("id") == "sparse");
    assert(defaultHits.front().at("vector") == sparse());
    std::cout << "NAMED_SPARSE_CRASH_RECOVERY_PASS\n";
#endif
}
