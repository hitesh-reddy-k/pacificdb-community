/**
 * @file hnsw_index.cpp
 * @description Phase E: HNSW (Hierarchical Navigable Small World) vector index
 *              implementation for the PacificDB C++ core engine.
 *
 * Experimental derived graph core. Not connected to public ANN routing.
 * Lifecycle, byte budgeting and recovery qualification remain incomplete.
 */

#include "read_cache.hpp"
#include <cmath>
#include <algorithm>
#include <queue>
#include <random>
#include <limits>
#include <iostream>
#include <stdexcept>

namespace pacificdb {

HNSWIndex::HNSWIndex(HNSWConfig config)
    : HNSWIndex(config, {}) {}
HNSWIndex::HNSWIndex(HNSWConfig config, std::shared_ptr<VectorAllocationBudget> sharedBudget)
    : config_(config), memory_(config.maxMemoryBytes, std::move(sharedBudget)),
      nodes_(&memory_), maxLevel_(0), entryPoint_(0), idToIndex_(&memory_) {
    if (config.M < 2 || config.M > 128 || config.efConstruction < config.M ||
        config.efConstruction > 100000 || config.efSearch == 0 ||
        config.efSearch > 100000 || config.dimensions > 65536 ||
        config.maxElements == 0 || config.maxMemoryBytes < 1024 ||
        config.maxBuildMilliseconds == 0 || config.maxBuildMilliseconds > 3600000 ||
        (config.metric != DistanceMetric::COSINE &&
         config.metric != DistanceMetric::EUCLIDEAN &&
         config.metric != DistanceMetric::DOT_PRODUCT)) {
        throw std::invalid_argument("invalid HNSW configuration");
    }
}

namespace {
bool validVector(const std::vector<float>& vector) {
    return !vector.empty() && vector.size() <= 65536 &&
        std::all_of(vector.begin(), vector.end(), [](float v) { return std::isfinite(v); });
}
float boundedDistance(double value) {
    const double bound = std::numeric_limits<float>::max();
    return static_cast<float>(std::clamp(value, -bound, bound));
}
}

// ═══════════════════════════════════════════════════════════════════════════
// Distance Computations
// ═══════════════════════════════════════════════════════════════════════════

float HNSWIndex::distanceRaw(const float* a, const float* b, size_t count) const {

    switch (config_.metric) {
    case DistanceMetric::EUCLIDEAN: {
        double sum = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double d = static_cast<double>(a[i]) - b[i];
            sum += d * d;
        }
        return boundedDistance(std::sqrt(sum));
    }
    case DistanceMetric::DOT_PRODUCT: {
        double dot = 0.0;
        for (size_t i = 0; i < count; ++i) {
            dot += static_cast<double>(a[i]) * b[i];
        }
        return boundedDistance(-dot); // Negate so smaller = more similar
    }
    case DistanceMetric::COSINE:
    default: {
        double dot = 0.0, normA = 0.0, normB = 0.0;
        for (size_t i = 0; i < count; ++i) {
            dot += static_cast<double>(a[i]) * b[i];
            normA += static_cast<double>(a[i]) * a[i];
            normB += static_cast<double>(b[i]) * b[i];
        }
        double denom = std::sqrt(normA) * std::sqrt(normB);
        if (denom == 0.0) return 1.0f;
        return static_cast<float>(1.0 - std::clamp(dot / denom, -1.0, 1.0));
    }
    }
}

int HNSWIndex::randomLevel() const {
    static thread_local std::mt19937 gen(std::random_device{}());
    static thread_local std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    int level = 0;
    const float probability = 1.0f / static_cast<float>(config_.M);
    while (level < 16 && dist(gen) < probability) {
        level++;
    }
    return level;
}

// ═══════════════════════════════════════════════════════════════════════════
// Layer Search
// ═══════════════════════════════════════════════════════════════════════════

std::pmr::vector<std::pair<size_t, float>> HNSWIndex::searchLayer(
    const std::vector<float>& query, size_t entryIdx, size_t ef, int level, const Check& check, bool liveOnly) const
{
    // Priority queues: candidates (min-heap) and results (max-heap)
    using PairDI = std::pair<float, size_t>; // (distance, index)

    auto cmpMin = [](const PairDI& a, const PairDI& b) { return a.first > b.first; };
    auto cmpMax = [](const PairDI& a, const PairDI& b) { return a.first < b.first; };

    std::priority_queue<PairDI, std::pmr::vector<PairDI>, decltype(cmpMin)>
        candidates(cmpMin, std::pmr::vector<PairDI>(&memory_));
    std::priority_queue<PairDI, std::pmr::vector<PairDI>, decltype(cmpMax)>
        results(cmpMax, std::pmr::vector<PairDI>(&memory_));

    std::pmr::unordered_map<size_t, bool> visited(&memory_);

    float d = distance(query, nodes_[entryIdx].vector);
    candidates.push({d, entryIdx});
    if(!liveOnly||!nodes_[entryIdx].deleted)results.push({d, entryIdx});
    visited[entryIdx] = true;

    while (!candidates.empty()) {
        if (check) check();
        auto [cDist, cIdx] = candidates.top();
        candidates.pop();

        float farthest = results.empty()?std::numeric_limits<float>::infinity():results.top().first;
        if (results.size() >= ef && cDist > farthest) break;

        // Explore neighbors at this level
        if (level < static_cast<int>(nodes_[cIdx].neighbors.size())) {
            for (size_t nIdx : nodes_[cIdx].neighbors[level]) {
                if (check) check();
                if (visited.count(nIdx)) continue;
                visited[nIdx] = true;

                float nDist = distance(query, nodes_[nIdx].vector);
                farthest = results.empty()?std::numeric_limits<float>::infinity():results.top().first;

                if (nDist < farthest || results.size() < ef) {
                    candidates.push({nDist, nIdx});
                    if(!liveOnly||!nodes_[nIdx].deleted){
                        results.push({nDist, nIdx});
                        if (results.size() > ef) results.pop();
                    }
                }
            }
        }
    }

    // Extract results
    std::pmr::vector<std::pair<size_t, float>> out(&memory_);
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
    return insert(id, vec, {});
}
bool HNSWIndex::insert(const std::string& id, const std::vector<float>& vec, const Check& check) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    return insertLocked(id,vec,check,false);
}
bool HNSWIndex::upsert(const std::string& id,const std::vector<float>& vec,const Check& check){
    std::unique_lock<std::shared_mutex> lock(mu_);
    return insertLocked(id,vec,check,true);
}
bool HNSWIndex::remove(const std::string& id,const Check& check){
    std::unique_lock<std::shared_mutex> lock(mu_);
    if(failed_)throw std::runtime_error("vector index unhealthy after failed mutation");
    if(check)check();
    const auto found=idToIndex_.find(std::pmr::string(id,&memory_));
    if(found==idToIndex_.end())return false;
    nodes_[found->second].deleted=true;idToIndex_.erase(found);return true;
}
bool HNSWIndex::insertLocked(const std::string& id,const std::vector<float>& vec,const Check& check,bool replace){
    if (failed_) throw std::runtime_error("vector index unhealthy after failed mutation");

    if (id.empty() || !validVector(vec)) return false;
    try {
    if (check) check();
    const std::pmr::string key(id, &memory_);
    const auto previous=idToIndex_.find(key);
    if(previous!=idToIndex_.end()&&!replace)return false;
    const size_t old=previous==idToIndex_.end()?std::numeric_limits<size_t>::max():previous->second;

    // Set dimensions on first insert
    if (config_.dimensions == 0) {
        config_.dimensions = vec.size();
    } else if (vec.size() != config_.dimensions) {
        return false; // Dimension mismatch
    }

    if(old!=std::numeric_limits<size_t>::max() && nodes_[old].vector.size()==vec.size() &&
       std::equal(vec.begin(),vec.end(),nodes_[old].vector.begin()))return true;
    if(nodes_.size()>=config_.maxElements)return false;

    int level = randomLevel();
    size_t idx = nodes_.size();

    HNSWNode node(&memory_);
    node.id.assign(id.data(), id.size());
    node.vector.assign(vec.begin(), vec.end());
    node.neighbors.resize(level + 1);
    nodes_.push_back(std::move(node));
    idToIndex_.insert_or_assign(nodes_.back().id, idx);
    if(old!=std::numeric_limits<size_t>::max())nodes_[old].deleted=true;

    if (nodes_.size() == 1) {
        maxLevel_ = level;
        entryPoint_ = idx;
        return true;
    }

    // Navigate from top level to the node's level
    size_t ep = entryPoint_;
    for (int l = static_cast<int>(maxLevel_); l > level; --l) {
        auto results = searchLayer(vec, ep, 1, l, check);
        if (!results.empty()) ep = results[0].first;
    }

    // Insert into each level
    for (int l = std::min(level, static_cast<int>(maxLevel_)); l >= 0; --l) {
        auto results = searchLayer(vec, ep, config_.efConstruction, l, check);

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
                    std::pmr::vector<std::pair<float, size_t>> scored(&memory_);
                    for (size_t nb : nbs) {
                        if (check) check();
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
    } catch (...) {
        // A failed allocation may leave a partially edited unpublished graph.
        // Never permit successful queries or further mutation on that instance.
        failed_ = true;
        throw;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Search
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::pair<std::string, float>> HNSWIndex::search(
    const std::vector<float>& query, size_t k) const
{
    return search(query, k, {});
}
std::vector<std::pair<std::string, float>> HNSWIndex::search(
    const std::vector<float>& query, size_t k, const Check& check) const
{
    return search(query,k,check,0);
}
std::vector<std::pair<std::string,float>> HNSWIndex::search(
    const std::vector<float>& query,size_t k,const Check& check,size_t efSearch) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (check) check();
    if (failed_) throw std::runtime_error("vector index unhealthy after failed mutation");

    if (k == 0 || idToIndex_.empty() || !validVector(query)) return {};
    if (query.size() != config_.dimensions) return {};

    if(efSearch>config_.efSearch)throw std::invalid_argument("query ef_search exceeds configured bound");
    // Navigate from top to level 0
    size_t ep = entryPoint_;
    for (int l = static_cast<int>(maxLevel_); l > 0; --l) {
        auto results = searchLayer(query, ep, 1, l, check);
        if (!results.empty()) ep = results[0].first;
    }

    // Search at level 0 with ef
    auto results = searchLayer(query, ep, std::max(k, efSearch?efSearch:config_.efSearch), 0, check, true);

    // Convert to output format
    std::vector<std::pair<std::string, float>> out;
    for (size_t i = 0; i < std::min(results.size(), k); ++i) {
        out.push_back({std::string(nodes_[results[i].first].id), results[i].second});
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
