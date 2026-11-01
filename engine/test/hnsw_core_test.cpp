#include "read_cache.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <iostream>
#include <limits>
#include <random>
#include <thread>

using namespace pacificdb;

// Independent double-precision ranking, not the graph's distance implementation.
double reference(const std::vector<float>& a, const std::vector<float>& b,
                 DistanceMetric metric) {
    double dot = 0, aa = 0, bb = 0, square = 0;
    for (size_t j = 0; j < a.size(); ++j) {
        dot += double(a[j]) * b[j];
        aa += double(a[j]) * a[j]; bb += double(b[j]) * b[j];
        const double delta = double(a[j]) - b[j]; square += delta * delta;
    }
    if (metric == DistanceMetric::DOT_PRODUCT) return -dot;
    if (metric == DistanceMetric::EUCLIDEAN) return std::sqrt(square);
    return aa == 0 || bb == 0 ? 1.0 : 1.0 - dot / std::sqrt(aa * bb);
}

void validation() {
    HNSWConfig c;
    for (auto bad : {size_t(0), size_t(1), size_t(129)}) {
        c.M = bad;
        bool threw = false;
        try { HNSWIndex index(c); } catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }
    c = HNSWConfig{}; c.maxElements = 2;
    HNSWIndex index(c);
    assert(!index.insert("empty", {})); assert(index.dimensions() == 0);
    assert(!index.insert("nan", {std::numeric_limits<float>::quiet_NaN()}));
    assert(index.insert("a", {1, 2}));
    assert(!index.insert("a", {2, 3}));
    assert(!index.insert("wrong-dimension", {1}));
    assert(!index.insert("", {1, 2}));
    assert(index.insert("b", {2, 3}));
    assert(!index.insert("over-budget", {3, 4}));
    assert(index.size() == 2 && index.dimensions() == 2);
    assert(index.search({1, 2}, 0).empty());
    assert(index.search({1}, 1).empty());
    assert(index.search({INFINITY, 1}, 1).empty());
    for (auto metric : {DistanceMetric::COSINE, DistanceMetric::EUCLIDEAN,
                        DistanceMetric::DOT_PRODUCT}) {
        c = HNSWConfig{}; c.metric = metric;
        HNSWIndex extreme(c);
        const float big = std::numeric_limits<float>::max();
        assert(extreme.insert("big", {big, big}));
        assert(extreme.insert("opposite", {-big, -big}));
        for (const auto& hit : extreme.search({big, big}, 2))
            assert(std::isfinite(hit.second));
    }
}

void recall(DistanceMetric metric, size_t dimensions) {
    constexpr size_t count = 2000, queries = 32, k = 10;
    HNSWConfig c; c.dimensions = dimensions; c.metric = metric;
    c.efConstruction = 200; c.efSearch = 300;
    HNSWIndex index(c);
    std::mt19937 rng(1701); std::uniform_real_distribution<float> value(-1, 1);
    std::vector<std::vector<float>> base(count, std::vector<float>(dimensions));
    for (size_t i = 0; i < count; ++i) {
        for (float& v : base[i]) v = value(rng);
        assert(index.insert(std::to_string(i), base[i]));
    }
    size_t matches = 0;
    for (size_t q = 0; q < queries; ++q) {
        std::vector<float> query(dimensions); for (float& v : query) v = value(rng);
        std::vector<std::pair<double, size_t>> truth;
        for (size_t i = 0; i < count; ++i)
            truth.emplace_back(reference(query, base[i], metric), i);
        std::partial_sort(truth.begin(), truth.begin() + k, truth.end());
        const auto hits = index.search(query, k); assert(hits.size() == k);
        for (const auto& hit : hits) {
            const size_t id = std::stoull(hit.first);
            assert(std::abs(double(hit.second) - reference(query, base[id], metric)) < 1e-4);
            for (size_t j = 0; j < k; ++j) if (id == truth[j].second) ++matches;
        }
    }
    const double score = double(matches) / (queries * k);
    std::cout << "core_recall metric=" << int(metric) << " dimensions=" << dimensions
              << " rows=" << count << " recall_at_10=" << score << '\n';
    assert(score >= .95);
}

void concurrency() {
    HNSWConfig c; c.dimensions = 2; HNSWIndex index(c);
    assert(index.insert("seed", {0, 1}));
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 500; ++i) assert(index.insert(std::to_string(i), {float(i), 1}));
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) readers.emplace_back([&] {
        while (!start.load()) std::this_thread::yield();
        for (int n = 0; n < 300; ++n) {
            assert(index.size() >= 1); assert(index.dimensions() == 2);
            assert(index.getStats().at("size").get<size_t>() >= 1);
            assert(!index.search({1, 1}, 3).empty());
        }
    });
    start.store(true); writer.join(); for (auto& reader : readers) reader.join();
    assert(index.size() == 501);
}

int main() {
    validation(); concurrency();
    for (auto metric : {DistanceMetric::COSINE, DistanceMetric::EUCLIDEAN,
                        DistanceMetric::DOT_PRODUCT}) recall(metric, 12);
    recall(DistanceMetric::COSINE, 64);
    std::cout << "HNSW_CORE_TEST_PASS (experimental core only)\n";
}
