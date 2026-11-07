#include "database_engine.hpp"
#include "owned_test_root.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

void ready() {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(30);
    for (;;) {
        const auto status = DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native");
        if (status.at("state") == "READY") return;
        if (status.at("state") == "FAILED") throw std::runtime_error(status.dump());
        assert(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
}
int main(int argc, char** argv) {
#ifdef _WIN32
    std::cout << "BLOCKED: process-crash fixture requires POSIX\n"; return 77;
#else
    setenv("WAL_ENABLED", "true", 1); setenv("WAL_FSYNC_ENABLED", "true", 1);
    setenv("WAL_FLUSH_ON_COMMIT", "true", 1); setenv("WAL_GROUP_COMMIT", "false", 1);
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--crash") {
        setenv("PACIFICDB_TEST_MODE", "vector_index_crash", 1);
        setenv("PACIFICDB_TEST_FAILPOINT_CONFIRM", "I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE", 1);
        setenv("PACIFICDB_TEST_FAILPOINT_INDEX", "1", 1);
        setenv("PACIFICDB_TEST_FAILPOINT_ACTION", "crash", 1);
        setenv("PACIFICDB_TEST_FAILPOINT", argv[3], 1);
        DatabaseEngine::init(argv[2]);
        DatabaseEngine::ensureUserRoot("alice");
        DatabaseEngine::createDatabase("alice", "app", "vector");
        DatabaseEngine::createCollection("alice", "app", "vectors");
        std::vector<json> docs;
        for (int i = 0; i < 200; ++i) docs.push_back({{"id", std::to_string(i)},
            {"vector", {double(i), 1.}}, {"title", i == 199 ? "rare shared" : "shared"}});
        DatabaseEngine::insertMany("alice", "app", "vectors", std::move(docs));
        DatabaseEngine::buildNativeVectorIndex("alice", "app", "vectors",
            {{"name", "native"}, {"dimensions", 2}, {"metric", "dot"}, {"ef_search", 200}, {"text_field", "title"},
             {"algorithm", argc == 5 ? argv[4] : "hnsw"}});
        ready(); DatabaseEngine::stopNativeVectorIndexes();
        return 2; // selected failpoint must terminate first
    }
    if (argc == 3 && std::string(argv[1]) == "--verify") {
        DatabaseEngine::init(argv[2]);
        assert(DatabaseEngine::count("alice", "app", "vectors") == 200);
        assert(DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native").at("state") == "ABSENT");
        DatabaseEngine::rebuildNativeVectorIndex("alice", "app", "vectors", "native"); ready();
        const bool flat = DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native")
            .at("spec").value("algorithm", std::string("hnsw")) == "flat";
        const auto request = json{{"vector", {1., 0.}}, {"k", 10}, {"metric", "dot"}};
        const auto hits = flat
            ? DatabaseEngine::queryNativeExactIndex("alice", "app", "vectors", "native", request)
            : DatabaseEngine::queryNativeVectorIndex("alice", "app", "vectors", "native", request);
        assert(hits.size() == 10);
        for (int k = 0; k < 10; ++k) {
            assert(hits[k].at("id") == std::to_string(199-k));
            assert(hits[k].at("score") == double(199-k));
        }
        assert(DatabaseEngine::nativeVectorIndexStatus("alice", "app", "vectors", "native").at("lexical").at("live_records") == 200);
        const auto hybrid = DatabaseEngine::queryNativeHybridIndex("alice", "app", "vectors", "native",
            {{"vector", {1., 0.}}, {"text", "rare"}, {"metric", "dot"}, {"k", 10}, {"candidates", 20}});
        assert(hybrid.size() == 10 && hybrid.front().at("id") == "199");
        assert(std::abs(hybrid.front().at("score").get<double>() - 2./61) < 1e-15);
        DatabaseEngine::stopNativeVectorIndexes(); return 0;
    }
    const auto spawn = [&](const std::vector<std::string>& arguments) {
        std::vector<char*> params{argv[0]};
        for (const auto& argument : arguments) params.push_back(const_cast<char*>(argument.c_str()));
        params.push_back(nullptr);
        const auto pid = fork(); assert(pid >= 0);
        if (pid == 0) { execv(argv[0], params.data()); _exit(3); }
        int status = 0; assert(waitpid(pid, &status, 0) == pid);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    };
    for (const std::string algorithm : {"hnsw", "flat"}) {
    for (const std::string phase : {"FP_VECTOR_BUILD_SOURCE_COMPLETE", "FP_VECTOR_BUILD_GRAPH_COMPLETE",
            "FP_VECTOR_BEFORE_PUBLICATION", "FP_VECTOR_DURING_PUBLICATION", "FP_VECTOR_AFTER_PUBLICATION"}) {
        OwnedTestRoot root("native-vector-crash");
        const auto path = root.dataRoot().string();
        assert(spawn({"--crash", path, phase, algorithm}) == 86);
        assert(spawn({"--verify", path}) == 0);
        assert(spawn({"--verify", path}) == 0);
        std::cout << "NATIVE_VECTOR_CRASH_PHASE_PASS " << algorithm << " " << phase << '\n';
    }
    }
    std::cout << "NATIVE_VECTOR_CRASH_RECOVERY_PASS\n";
#endif
}
