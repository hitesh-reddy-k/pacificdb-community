#include "vector_value.hpp"
#include <cassert>
#include <iostream>
#include <random>

using namespace pacificdb::vectors;
static json sparse(const json& indices, const json& values, uint32_t dimensions = 4) {
    return {{"format", "sparse-v1"}, {"precision", "float64"},
            {"dimensions", dimensions}, {"indices", indices}, {"values", values}};
}
template<class F> void rejects(F function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}
int main() {
    const auto zero = decode(sparse(json::array(), json::array()));
    assert(zero.dimensions == 4 && zero.sparse && zero.values.empty());
    const auto a = decode(sparse({0, 3}, {2., -1.}));
    const auto b = decode(json::array({1., 0., 0., 3.}));
    assert(score(a, b, "dot") == -1);
    assert(std::abs(score(a, b, "l2") + std::sqrt(17.)) < 1e-12);
    assert(std::abs(score(a, b, "cosine") + 1 / std::sqrt(50.)) < 1e-12);
    assert(score(zero, b, "cosine") == 0);
    assert(score(a, decode({1., 0.}), "dot") == -std::numeric_limits<double>::infinity());
    const auto huge = decode(sparse({999999999}, {2}, 1000000000));
    assert(score(huge, huge, "dot") == 4 && huge.values.size() == 1);
    for (const json invalid : {sparse({0, 0}, {1, 2}), sparse({3, 1}, {1, 2}),
            sparse({4}, {1}), sparse({-1}, {1}), sparse({1.5}, {1}),
            sparse({0}, {1}, 0), sparse({0}, json::array()),
            sparse({0}, {std::numeric_limits<double>::infinity()}),
            json::array(), json("invalid")}) rejects([&] { decode(invalid); });
    validateRecord({{"vectors", {{"dense", {1., 2.}}, {"sparse", sparse({1}, {2.})}}}});
    rejects([&] { validateRecord({{"vectors", json::object()}}); });
    // Independent densification/reference for randomized sparse/sparse metrics.
    std::mt19937 rng(1701);
    for (int trial = 0; trial < 1000; ++trial) {
        json ai = json::array(), av = json::array(), bi = json::array(), bv = json::array();
        std::vector<long double> ad(32), bd(32);
        for (int d = 0; d < 32; ++d) {
            if (rng() % 4 == 0) { ad[d] = int(rng() % 11) - 5; ai.push_back(d); av.push_back(double(ad[d])); }
            if (rng() % 4 == 0) { bd[d] = int(rng() % 11) - 5; bi.push_back(d); bv.push_back(double(bd[d])); }
        }
        long double dot = 0, aa = 0, bb = 0, sq = 0;
        for (int d = 0; d < 32; ++d) {
            dot += ad[d]*bd[d]; aa += ad[d]*ad[d]; bb += bd[d]*bd[d];
            sq += (ad[d]-bd[d])*(ad[d]-bd[d]);
        }
        const auto x = decode(sparse(ai, av, 32)), y = decode(sparse(bi, bv, 32));
        assert(std::abs(score(x, y, "dot") - dot) < 1e-10);
        assert(std::abs(score(x, y, "l2") + std::sqrt(sq)) < 1e-10);
        assert(std::abs(score(x, y, "cosine") - (aa*bb == 0 ? 0 : dot/std::sqrt(aa*bb))) < 1e-10);
    }
    std::cout << "VECTOR_VALUE_PASS\n";
}
