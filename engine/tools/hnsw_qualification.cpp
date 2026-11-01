#include "read_cache.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <set>
#ifdef __linux__
#include <sys/resource.h>
#endif

using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) try {
    const size_t count = argc > 1 ? std::stoull(argv[1]) : 10000;
    const size_t dimensions = argc > 2 ? std::stoull(argv[2]) : 25;
    const size_t queries = argc > 3 ? std::stoull(argv[3]) : 100;
    const std::string metric = argc > 4 ? argv[4] : "cosine";
    if (count < 10 || count > 1000000 || !dimensions || dimensions > 1024 ||
        !queries || queries > 10000 || (metric != "cosine" && metric != "l2" && metric != "dot"))
        throw std::invalid_argument("qualification bounds/metric invalid");
    pacificdb::HNSWConfig config; config.dimensions = dimensions;
    config.metric = metric == "l2" ? pacificdb::DistanceMetric::EUCLIDEAN :
        metric == "dot" ? pacificdb::DistanceMetric::DOT_PRODUCT : pacificdb::DistanceMetric::COSINE;
    config.M = 16; config.efConstruction = 200; config.efSearch = 300;
    config.maxMemoryBytes = 1024ULL * 1024 * 1024;
    pacificdb::HNSWIndex index(config);
    std::mt19937 rng(1701); std::uniform_real_distribution<float> random(-1, 1);
    std::vector<std::vector<float>> records(count, std::vector<float>(dimensions));
    for (auto& vector : records) {
        double norm = 0;
        for (auto& value : vector) { value = random(rng); norm += double(value)*value; }
        for (auto& value : vector) value = static_cast<float>(value/std::sqrt(norm));
    }
    const auto start = Clock::now();
    for (size_t i = 0; i < count; ++i) {
        if (!index.insert(std::to_string(i), records[i])) throw std::runtime_error("insert rejected");
        if ((i + 1) % 10000 == 0) std::cerr << "inserted=" << i + 1 << "\n";
    }
    const auto buildSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    size_t matches = 0; std::vector<double> latencies;
    for (size_t q = 0; q < queries; ++q) {
        std::vector<float> query(dimensions); double qq = 0;
        for (auto& value : query) { value = random(rng); qq += double(value)*value; }
        std::vector<std::pair<double, size_t>> truth; truth.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            double dot = 0, norm = 0, square = 0;
            for (size_t d = 0; d < dimensions; ++d) {
                const double x = query[d], y = records[i][d];
                dot += x*y; norm += y*y; square += (x-y)*(x-y);
            }
            truth.emplace_back(metric == "l2" ? std::sqrt(square) : metric == "dot" ? -dot :
                               1.0 - dot/std::sqrt(qq*norm), i);
        }
        std::partial_sort(truth.begin(), truth.begin() + 10, truth.end());
        const auto queryStart = Clock::now();
        const auto hits = index.search(query, 10);
        latencies.push_back(std::chrono::duration<double, std::milli>(Clock::now() - queryStart).count());
        if (hits.size() != 10) throw std::runtime_error("incomplete ANN result count");
        std::set<size_t> seen;
        for (const auto& hit : hits) {
            const auto id = std::stoull(hit.first);
            if (!seen.insert(id).second) throw std::runtime_error("duplicate hit");
            for (size_t k = 0; k < 10; ++k) if (truth[k].second == id) ++matches;
        }
    }
    std::sort(latencies.begin(), latencies.end());
    const double recall = double(matches)/(queries*10);
    nlohmann::json report{{"status", recall >= .95 ? "PASS" : "FAIL"},
        {"scope", "standalone graph recall/allocation diagnostic, not database readiness"},
        {"rows", count}, {"dimensions", dimensions}, {"queries", queries}, {"metric", metric},
        {"input_seed", 1701}, {"graph_levels", "nondeterministic"}, {"recall_at_10", recall},
        {"build_seconds", buildSeconds}, {"p50_ms", latencies[queries/2]},
        {"p95_ms", queries >= 1000 ? nlohmann::json(latencies[size_t(std::ceil(.95*queries))-1]) : nlohmann::json()},
        {"p99_ms", queries >= 1000 ? nlohmann::json(latencies[size_t(std::ceil(.99*queries))-1]) : nlohmann::json()},
        {"graph", index.getStats()}};
#ifdef __linux__
    rusage usage{}; if (getrusage(RUSAGE_SELF, &usage) == 0) report["process_peak_rss_kib"] = usage.ru_maxrss;
#endif
    std::cout << report.dump() << '\n';
    return recall >= .95 ? 0 : 1;
} catch (const std::exception& e) {
    std::cerr << "HNSW_QUALIFICATION_FAIL " << e.what() << '\n'; return 1;
}
