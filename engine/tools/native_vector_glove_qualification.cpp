// Trusted C++ qualification only. Never opens the source snapshot for mutation.
#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sys/resource.h>
#endif
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) try {
#ifndef _WIN32
    if (argc != 4 && argc != 5) throw std::invalid_argument("usage: native_vector_glove_qualification stopped-data-root queries.json records.jsonl [hnsw|flat]");
    const std::string algorithm = argc == 5 ? argv[4] : "hnsw";
    if (algorithm != "hnsw" && algorithm != "flat") throw std::invalid_argument("unknown algorithm");
    const auto source = std::filesystem::canonical(argv[1]);
    const auto input = json::parse(std::ifstream(argv[2]));
    if (input.at("rows") != 1000000 || input.at("dimensions") != 25 ||
        !input.at("queries").is_array() || input.at("queries").empty() || input.at("queries").size() > 1000)
        throw std::invalid_argument("invalid qualification dataset");
    const int sourceLock = open((source / ".pacificdb-root.lock").c_str(), O_RDONLY | O_CLOEXEC);
    if (sourceLock < 0 || flock(sourceLock, LOCK_SH | LOCK_NB) != 0)
        throw std::runtime_error("source snapshot must be stopped");
    struct Close { int fd; ~Close(){ if(fd >= 0)close(fd); } } closeSource{sourceLock};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source))
        if (entry.is_symlink()) throw std::runtime_error("source snapshot symlinks are unsupported");
    OwnedTestRoot root("native-glove-qualification");
    std::filesystem::copy(source, root.dataRoot(), std::filesystem::copy_options::recursive);
    auto identity = json::parse(std::ifstream(root.dataRoot() / ".pacificdb-root-identity.json"));
    identity["canonicalRoot"] = std::filesystem::canonical(root.dataRoot()).string();
    { std::ofstream stream(root.dataRoot() / ".pacificdb-root-identity.json"); stream << identity.dump(2); }
    flock(sourceLock, LOCK_UN);
    setenv("RAFT_CLUSTER_ID", identity.at("clusterId").get<std::string>().c_str(), 1);
    setenv("RAFT_NODE_ID", identity.at("nodeId").get<std::string>().c_str(), 1);
    setenv("WAL_ENABLED", "true", 1); setenv("WAL_FSYNC_ENABLED", "true", 1);
    setenv("WAL_FLUSH_ON_COMMIT", "true", 1); setenv("WAL_CHECKSUM_ENABLED", "true", 1);
    DatabaseEngine::init(root.dataRoot().string());
    LSM::startBackgroundTasks();
    struct Stop { ~Stop(){ DatabaseEngine::stopNativeVectorIndexes(); LSM::stopBackgroundTasks(); } } stop;
    QueryLimiter::setMaxScanRows(1001000);
    if (DatabaseEngine::count("system", "publish_bench", "vectors") != 1000000)
        throw std::runtime_error("stored corpus count mismatch");
    const auto buildStart = Clock::now();
    DatabaseEngine::buildNativeVectorIndex("system", "publish_bench", "vectors",
        {{"name", "glove"}, {"algorithm", algorithm}, {"dimensions", 25}, {"metric", "cosine"}, {"M", 16},
         {"ef_construction", 200}, {"ef_search", 300}, {"max_elements", 1000000},
         {"max_scan_rows", 1001000}, {"max_source_bytes", 512ULL * 1024 * 1024},
         {"max_index_bytes", 1024ULL * 1024 * 1024}, {"max_build_ms", 3600000}});
    json status;
    size_t poll = 0;
    for (;;) {
        status = DatabaseEngine::nativeVectorIndexStatus("system", "publish_bench", "vectors", "glove");
        if (status.at("state") == "READY") break;
        if (status.at("state") == "FAILED" || status.at("state") == "STALE")
            throw std::runtime_error(status.dump());
        if (Clock::now() - buildStart > std::chrono::hours(1)) throw std::runtime_error("build deadline exceeded");
        if (++poll % 6 == 0) std::cerr << "BUILDING seconds="
            << std::chrono::duration<double>(Clock::now() - buildStart).count() << '\n';
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    const double buildSeconds = std::chrono::duration<double>(Clock::now() - buildStart).count();
    std::cerr << "READY build_seconds=" << buildSeconds << '\n';
    std::ofstream records(argv[3]);
    if (!records) throw std::runtime_error("cannot create raw result file");
    size_t matches = 0, strict = 0, measured = 0;
    std::vector<double> latency;
    for (const auto& item : input.at("queries")) {
        const auto query = item.at("vector").get<std::vector<double>>();
        const auto start = Clock::now();
        const auto request = json{{"vector", query}, {"metric", "cosine"}, {"k", 10}};
        const auto hits = algorithm == "flat"
            ? DatabaseEngine::queryNativeExactIndex("system", "publish_bench", "vectors", "glove", request)
            : DatabaseEngine::queryNativeVectorIndex("system", "publish_bench", "vectors", "glove", request);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (hits.size() != 10) throw std::runtime_error("incomplete query result");
        std::set<size_t> expected;
        for (const auto& id : item.at("ids")) expected.insert(id.get<size_t>());
        size_t recalled = 0, exactIds = 0;
        for (const auto& hit : hits) {
            const auto vector = hit.at("vector").get<std::vector<double>>();
            if (vector.size() != query.size()) throw std::runtime_error("returned dimensions mismatch");
            double dot = 0, aa = 0, bb = 0;
            for (size_t d = 0; d < query.size(); ++d) { dot += query[d] * vector[d]; aa += query[d] * query[d]; bb += vector[d] * vector[d]; }
            const double score = std::sqrt(aa * bb) < 1e-12 ? 0. : dot / (std::sqrt(aa) * std::sqrt(bb));
            if (std::abs(score - hit.at("score").get<double>()) > 1e-12) throw std::runtime_error("score differs from returned payload");
            if (score >= item.at("cutoff").get<double>() - 1e-6) ++recalled;
            if (expected.count(std::stoull(hit.at("id").get<std::string>()))) ++exactIds;
        }
        const bool warmup = item.value("warmup", false);
        records << json{{"query", item.at("query")}, {"warmup", warmup}, {"ms", ms},
            {"recall_at_10", recalled / 10.}, {"strict_recall", exactIds / 10.}, {"documents", hits}}.dump() << '\n';
        records.flush();
        if (!records) throw std::runtime_error("cannot persist raw query result");
        if (!warmup) { latency.push_back(ms); matches += recalled; strict += exactIds; ++measured; }
    }
    if (!measured) throw std::runtime_error("no measured queries");
    std::sort(latency.begin(), latency.end());
    const double recall = matches / double(measured * 10);
    json result{{"scope", algorithm == "flat" ? "trusted C++ internal packed exact; not public/SDK readiness" : "trusted C++ internal ANN; not public/SDK readiness"},
        {"status", recall >= (algorithm == "flat" ? 1. : .95) ? "PASS" : "FAIL"}, {"algorithm", algorithm}, {"rows", 1000000}, {"dimensions", 25},
        {"queries", measured}, {"recall_at_10", recall}, {"strict_recall", strict / double(measured * 10)},
        {"source_and_build_seconds", buildSeconds}, {"p50_ms", latency[latency.size() / 2]},
        {"p95_ms", nullptr}, {"p99_ms", nullptr}, {"index", status}};
    rusage usage{}; if (getrusage(RUSAGE_SELF, &usage) == 0) result["peak_rss_kib"] = usage.ru_maxrss;
    std::cout << "GLOVE_NATIVE_QUALIFICATION_JSON " << result.dump() << '\n';
    return recall >= (algorithm == "flat" ? 1. : .95) ? 0 : 1;
#else
    throw std::runtime_error("POSIX source locking required for this diagnostic");
#endif
} catch (const std::exception& error) { std::cerr << "GLOVE_NATIVE_QUALIFICATION_FAIL " << error.what() << '\n'; return 1; }
