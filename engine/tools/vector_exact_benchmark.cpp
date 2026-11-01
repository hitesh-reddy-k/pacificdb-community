#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>

// Same source can be linked against the unchanged baseline and the candidate.
// This measures exact ranking only, not ANN or a client/protocol comparison.
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) try {
    const std::size_t count = argc > 1 ? std::stoull(argv[1]) : 10000;
    const std::size_t requests = argc > 2 ? std::stoull(argv[2]) : 10;
    const std::size_t dimensions = argc > 3 ? std::stoull(argv[3]) : 25;
    if (!count || count > 1000000 || !requests || requests > 10000 ||
        !dimensions || dimensions > 1536) throw std::invalid_argument("benchmark bounds exceeded");
    const OwnedTestRoot root("vector-exact-benchmark");
    DatabaseEngine::init(root.dataRoot().string());
    LSM::startBackgroundTasks();
    struct MaintenanceGuard {
        ~MaintenanceGuard() { LSM::stopBackgroundTasks(); }
    } maintenanceGuard;
    DatabaseEngine::ensureUserRoot("bench");
    DatabaseEngine::createDatabase("bench", "bench", "vector");
    DatabaseEngine::createCollection("bench", "bench", "vectors");
    QueryLimiter::setMaxScanRows(count + 1);
    // Baseline treats this as a candidate limit. Raise identically for both.
    QueryLimiter::setMaxResultDocs(count + 1);
    QueryLimiter::setMaxResultBytes(1024ULL * 1024 * 1024);
    std::mt19937 random(20261029);
    std::uniform_real_distribution<double> value(-1, 1);
    const auto loadStart = Clock::now();
    std::size_t syncs = 0;
    for (std::size_t first = 0; first < count; first += 500) {
        std::vector<json> batch;
        for (std::size_t i = first; i < std::min(count, first + 500); ++i) {
            std::vector<double> vector(dimensions);
            double norm = 0;
            for (auto& x : vector) { x = value(random); norm += x*x; }
            for (auto& x : vector) x = static_cast<float>(x / std::sqrt(norm));
            batch.push_back({{"id", "v" + std::to_string(i)}, {"kind", "vector"}, {"vector", vector}});
        }
        auto response = DatabaseEngine::insertMany("bench", "bench", "vectors", std::move(batch));
        syncs += response.value("lsm", json::object()).value("wal_physical_syncs", 0ULL);
    }
    const double loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();
    LSM::flush("bench", "bench", "vectors");
    if (DatabaseEngine::count("bench", "bench", "vectors") != count) throw std::runtime_error("load count mismatch");
    std::vector<double> latencies;
    json answers = json::array();
    for (std::size_t iteration = 0; iteration < requests + 2; ++iteration) {
        std::vector<double> query(dimensions);
        for (auto& x : query) x = value(random);
        const auto start = Clock::now();
        const auto rows = DatabaseEngine::queryVector("bench", "bench", "vectors",
            {{"vector", query}, {"k", 10}, {"metric", "cosine"}, {"search_mode", "exact"}});
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (rows.size() != std::min<std::size_t>(count, 10)) throw std::runtime_error("unexpected result count");
        if (iteration >= 2) {
            latencies.push_back(ms);
            json answer = json::array();
            for (const auto& row : rows) answer.push_back({{"id", row.at("id")}, {"score", row.at("score")}});
            answers.push_back(std::move(answer));
        }
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double p) { return latencies.at(static_cast<std::size_t>(std::ceil(p * requests)) - 1); };
    json result{{"rows", count}, {"dimensions", dimensions}, {"measured_queries", requests},
                {"warmup_queries", 2}, {"seed", 20261029}, {"load_seconds", loadSeconds},
                {"reported_wal_physical_syncs", syncs}, {"latencies_ms", latencies},
                {"answers", std::move(answers)},
                {"p50_ms", requests % 2 ? latencies[requests / 2] : (latencies[requests / 2 - 1] + latencies[requests / 2]) / 2},
                {"p95_ms", requests >= 1000 ? json(percentile(.95)) : json()},
                {"p99_ms", requests >= 1000 ? json(percentile(.99)) : json()}};
    std::cout << "VECTOR_BENCHMARK_JSON " << result.dump() << '\n';
} catch (const std::exception& error) {
    std::cerr << "VECTOR_BENCHMARK_FAILURE " << error.what() << '\n';
    return 1;
}
