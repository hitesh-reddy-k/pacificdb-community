/**
 * @file hnsw_index.cpp
 * @description Phase E: HNSW (Hierarchical Navigable Small World) vector index
 *              implementation for the PacificDB C++ core engine.
 *
 *   ✔ Multi-layer navigable graph for O(log n) ANN search
 *   ✔ Cosine, Euclidean, and Dot Product distance metrics
 *   ✔ Lock-protected for concurrent access
 *   ✔ Batch insert support
 *   ✔ No external dependencies (pure C++)
 */

#include "read_cache.hpp"
#include <cmath>
#include <algorithm>
#include <queue>
#include <random>
#include <limits>
#include <iostream>

namespace pacificdb {

// ═══════════════════════════════════════════════════════════════════════════
// Distance Computations
// ═══════════════════════════════════════════════════════════════════════════

float HNSWIndex::distance(const std::vector<float>& a, const std::vector<float>& b) const {
    if (a.size() != b.size()) return std::numeric_limits<float>::max();

    switch (config_.metric) {
    case DistanceMetric::EUCLIDEAN: {
        float sum = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            float d = a[i] - b[i];
            sum += d * d;
        }
        return std::sqrt(sum);
    }
    case DistanceMetric::DOT_PRODUCT: {
        float dot = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
        }
        return -dot; // Negate so smaller = more similar
    }
    case DistanceMetric::COSINE:
    default: {
        float dot = 0.0f, normA = 0.0f, normB = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            dot += a[i] * b[i];
            normA += a[i] * a[i];
            normB += b[i] * b[i];
        }
        float denom = std::sqrt(normA) * std::sqrt(normB);
        if (denom < 1e-10f) return 1.0f;
        return 1.0f - (dot / denom); // 0 = identical, 2 = opposite
    }
    }
}

int HNSWIndex::randomLevel() const {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    int level = 0;
    float mL = 1.0f / std::log(static_cast<float>(config_.M));
    while (dist(gen) < std::exp(-static_cast<float>(level + 1) / mL) && level < 16) {
        level++;
    }
    return level;
}

// ═══════════════════════════════════════════════════════════════════════════
// Layer Search
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::pair<size_t, float>> HNSWIndex::searchLayer(
    const std::vector<float>& query, size_t entryIdx, size_t ef, int level) const
{
    // Priority queues: candidates (min-heap) and results (max-heap)
    using PairDI = std::pair<float, size_t>; // (distance, index)

    auto cmpMin = [](const PairDI& a, const PairDI& b) { return a.first > b.first; };
    auto cmpMax = [](const PairDI& a, const PairDI& b) { return a.first < b.first; };

    std::priority_queue<PairDI, std::vector<PairDI>, decltype(cmpMin)> candidates(cmpMin);
    std::priority_queue<PairDI, std::vector<PairDI>, decltype(cmpMax)> results(cmpMax);

    std::unordered_map<size_t, bool> visited;

    float d = distance(query, nodes_[entryIdx].vector);
    candidates.push({d, entryIdx});
    results.push({d, entryIdx});
    visited[entryIdx] = true;

    while (!candidates.empty()) {
        auto [cDist, cIdx] = candidates.top();
        candidates.pop();

        float farthest = results.top().first;
        if (cDist > farthest) break;

        // Explore neighbors at this level
        if (level < static_cast<int>(nodes_[cIdx].neighbors.size())) {
            for (size_t nIdx : nodes_[cIdx].neighbors[level]) {
                if (visited.count(nIdx)) continue;
                visited[nIdx] = true;

                float nDist = distance(query, nodes_[nIdx].vector);
                farthest = results.top().first;

                if (nDist < farthest || results.size() < ef) {
                    candidates.push({nDist, nIdx});
                    results.push({nDist, nIdx});
                    if (results.size() > ef) results.pop();
                }
            }
        }
    }

    // Extract results
    std::vector<std::pair<size_t, float>> out;
    while (!results.empty()) {
        out.push_back({results.top().second, results.top().first});
        results.pop();
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// Insert
// ═══════════════════════════════════════════════════════════════════════════

bool HNSWIndex::insert(const std::string& id, const std::vector<float>& vec) {
    std::lock_guard<std::mutex> lock(mu_);

    if (idToIndex_.count(id)) return false; // Duplicate

    // Set dimensions on first insert
    if (config_.dimensions == 0) {
        config_.dimensions = vec.size();
    } else if (vec.size() != config_.dimensions) {
        return false; // Dimension mismatch
    }

    int level = randomLevel();
    size_t idx = nodes_.size();

    HNSWNode node;
    node.id = id;
    node.vector = vec;
    node.neighbors.resize(level + 1);
    nodes_.push_back(std::move(node));
    idToIndex_[id] = idx;

    if (nodes_.size() == 1) {
        maxLevel_ = level;
        entryPoint_ = idx;
        return true;
    }

    // Navigate from top level to the node's level
    size_t ep = entryPoint_;
    for (int l = static_cast<int>(maxLevel_); l > level; --l) {
        auto results = searchLayer(vec, ep, 1, l);
        if (!results.empty()) ep = results[0].first;
    }

    // Insert into each level
    for (int l = std::min(level, static_cast<int>(maxLevel_)); l >= 0; --l) {
        auto results = searchLayer(vec, ep, config_.efConstruction, l);

        // Connect to nearest M neighbors
        size_t M = config_.M;
        for (size_t i = 0; i < std::min(results.size(), M); ++i) {
            size_t neighborIdx = results[i].first;

            // Bidirectional connection
            if (l < static_cast<int>(nodes_[idx].neighbors.size())) {
                nodes_[idx].neighbors[l].push_back(neighborIdx);
            }
            if (l < static_cast<int>(nodes_[neighborIdx].neighbors.size())) {
                nodes_[neighborIdx].neighbors[l].push_back(idx);

                // Prune if too many connections
                if (nodes_[neighborIdx].neighbors[l].size() > M * 2) {
                    // Keep closest M connections
                    auto& nbs = nodes_[neighborIdx].neighbors[l];
                    std::vector<std::pair<float, size_t>> scored;
                    for (size_t nb : nbs) {
                        scored.push_back({distance(nodes_[neighborIdx].vector, nodes_[nb].vector), nb});
                    }
                    std::sort(scored.begin(), scored.end());
                    nbs.clear();
                    for (size_t j = 0; j < M; ++j) {
                        nbs.push_back(scored[j].second);
                    }
                }
            }
        }

        if (!results.empty()) ep = results[0].first;
    }

    // Update entry point if new node has higher level
    if (static_cast<size_t>(level) > maxLevel_) {
        maxLevel_ = level;
        entryPoint_ = idx;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Search
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::pair<std::string, float>> HNSWIndex::search(
    const std::vector<float>& query, size_t k) const
{
    std::lock_guard<std::mutex> lock(mu_);

    if (nodes_.empty()) return {};
    if (query.size() != config_.dimensions) return {};

    // Navigate from top to level 0
    size_t ep = entryPoint_;
    for (int l = static_cast<int>(maxLevel_); l > 0; --l) {
        auto results = searchLayer(query, ep, 1, l);
        if (!results.empty()) ep = results[0].first;
    }

    // Search at level 0 with ef
    auto results = searchLayer(query, ep, std::max(k, config_.efSearch), 0);

    // Convert to output format
    std::vector<std::pair<std::string, float>> out;
    for (size_t i = 0; i < std::min(results.size(), k); ++i) {
        out.push_back({nodes_[results[i].first].id, results[i].second});
    }

    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// Batch Insert
// ═══════════════════════════════════════════════════════════════════════════

size_t HNSWIndex::batchInsert(
    const std::vector<std::pair<std::string, std::vector<float>>>& items)
{
    size_t inserted = 0;
    for (const auto& [id, vec] : items) {
        if (insert(id, vec)) inserted++;
    }
    return inserted;
}

} // namespace pacificdb
