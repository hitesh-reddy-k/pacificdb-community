#include "lexical_index.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>

namespace pacificdb {
LexicalIndex::LexicalIndex(LexicalConfig config, std::shared_ptr<VectorAllocationBudget> shared)
    : config_(config), memory_(config.maxMemoryBytes, std::move(shared)), documents_(&memory_),
      terms_(&memory_), vocabulary_(&memory_), ids_(&memory_) {
    if (config.maxMemoryBytes < 1024 || !config.maxRecords || config.maxRecords > 10000000 ||
        !config.maxCandidates || config.maxCandidates > 10000000 || !std::isfinite(config.k1) ||
        config.k1 < 0 || config.k1 > 10 || !std::isfinite(config.b) || config.b < 0 || config.b > 1)
        throw std::invalid_argument("invalid lexical index configuration");
}
std::vector<std::string> LexicalIndex::tokenize(const std::string& text, size_t maximumBytes,
                                              size_t maximumTokens) {
    if (text.size() > maximumBytes) throw std::invalid_argument("lexical text byte limit exceeded");
    std::vector<std::string> result; std::string token;
    const auto flush = [&] {
        if (token.empty()) return;
        if (result.size() == maximumTokens) throw std::invalid_argument("lexical token limit exceeded");
        result.push_back(std::move(token)); token.clear();
    };
    for (unsigned char value : text) {
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value >= 128) {
            token.push_back(value >= 'A' && value <= 'Z' ? char(value + ('a'-'A')) : char(value));
        } else flush();
    }
    flush(); return result;
}
void LexicalIndex::remove(size_t index) {
    auto& document = documents_[index];
    if (!document.live) return;
    document.live = false; --liveRecords_; totalLength_ -= document.length;
    for (size_t term : document.terms) --terms_[term].liveFrequency;
}
bool LexicalIndex::upsert(const std::string& id, const std::string& text, const Check& check) {
    if (id.empty() || id.size() > 1024) throw std::invalid_argument("invalid lexical record ID");
    const auto tokens = tokenize(text, 1024*1024, 65536);
    std::unordered_map<std::string, uint32_t> frequencies;
    for (const auto& token : tokens) ++frequencies[token];
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (failed_) throw std::runtime_error("lexical_index_unhealthy");
    if (documents_.size() == config_.maxRecords) throw std::runtime_error("lexical_record_limit_exceeded");
    try {
        if (check) check();
        const std::pmr::string identity(id, &memory_);
        const auto previous = ids_.find(identity);
        const auto previousIndex = previous == ids_.end() ? size_t(-1) : previous->second;
        Document document(&memory_); document.id.assign(id.data(), id.size()); document.length = tokens.size();
        const auto index = documents_.size(); documents_.push_back(std::move(document));
        for (const auto& frequency : frequencies) {
            if (check) check();
            const std::pmr::string term(frequency.first, &memory_);
            auto found = vocabulary_.find(term);
            if (found == vocabulary_.end()) {
                const auto termIndex = terms_.size(); terms_.emplace_back(&memory_);
                found = vocabulary_.emplace(term, termIndex).first;
            }
            auto& entry = terms_[found->second];
            entry.postings.push_back({index, frequency.second}); ++entry.liveFrequency;
            documents_[index].terms.push_back(found->second);
        }
        if (previousIndex != size_t(-1)) remove(previousIndex);
        ids_[identity] = index; ++liveRecords_; totalLength_ += tokens.size();
        return true;
    } catch (...) { failed_ = true; throw; }
}
bool LexicalIndex::erase(const std::string& id) {
    if (id.empty() || id.size() > 1024) throw std::invalid_argument("invalid lexical record ID");
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (failed_) throw std::runtime_error("lexical_index_unhealthy");
    const auto found = ids_.find(std::pmr::string(id, &memory_));
    if (found == ids_.end() || !documents_[found->second].live) return false;
    remove(found->second); return true;
}
std::vector<std::pair<std::string, double>> LexicalIndex::search(const std::string& query,
    size_t k, bool all, const Check& check,
    const std::function<bool(const std::string&)>& eligible) const {
    if (!k || k > 10000) throw std::invalid_argument("lexical k out of range");
    const auto tokens = tokenize(query, 4096, 64);
    const std::set<std::string> unique(tokens.begin(), tokens.end());
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (failed_) throw std::runtime_error("lexical_index_unhealthy");
    if (check) check();
    if (unique.empty() || !liveRecords_ || !totalLength_) return {};
    struct Score { double score{0}; size_t terms{0}; };
    std::pmr::unordered_map<size_t, Score> scores(&memory_);
    const double average = double(totalLength_)/liveRecords_;
    for (const auto& term : unique) {
        if (check) check();
        const auto found = vocabulary_.find(std::pmr::string(term, &memory_));
        if (found == vocabulary_.end()) { if (all) return {}; else continue; }
        const auto& entry = terms_[found->second];
        const auto idf = std::log1p((double(liveRecords_)-entry.liveFrequency+.5)/(entry.liveFrequency+.5));
        for (const auto& posting : entry.postings) {
            if (check) check();
            const auto& document = documents_[posting.document];
            if (!document.live) continue;
            auto foundScore = scores.find(posting.document);
            if (foundScore == scores.end()) {
                if (scores.size() == config_.maxCandidates)
                    throw std::runtime_error("lexical_candidate_limit_exceeded");
                foundScore = scores.emplace(posting.document, Score{}).first;
            }
            const double norm = config_.k1*(1.-config_.b+config_.b*document.length/average);
            foundScore->second.score += idf*(posting.frequency*(config_.k1+1.))/(posting.frequency+norm);
            ++foundScore->second.terms;
        }
    }
    using Candidate = std::pair<size_t, double>;
    const auto better = [&](const Candidate& a, const Candidate& b) {
        return a.second > b.second || (a.second == b.second && documents_[a.first].id < documents_[b.first].id);
    };
    std::priority_queue<Candidate, std::pmr::vector<Candidate>, decltype(better)>
        heap(better, std::pmr::vector<Candidate>(&memory_));
    for (const auto& score : scores) {
        if (check) check();
        if (all && score.second.terms != unique.size()) continue;
        if (eligible && !eligible(std::string(documents_[score.first].id))) continue;
        Candidate candidate{score.first, score.second.score};
        if (heap.size() == k && !better(candidate, heap.top())) continue;
        if (heap.size() == k) heap.pop();
        heap.push(candidate);
    }
    std::vector<std::pair<std::string, double>> result; result.reserve(heap.size());
    while (!heap.empty()) {
        result.emplace_back(std::string(documents_[heap.top().first].id), heap.top().second); heap.pop();
    }
    std::reverse(result.begin(), result.end()); return result;
}
nlohmann::json LexicalIndex::stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return {{"live_records", liveRecords_}, {"stored_versions", documents_.size()},
        {"terms", vocabulary_.size()}, {"total_tokens", totalLength_}, {"healthy", !failed_},
        {"allocated_bytes", memory_.accounting().used()}, {"peak_bytes", memory_.accounting().peak()},
        {"limit_bytes", config_.maxMemoryBytes}, {"k1", config_.k1}, {"b", config_.b}};
}
std::vector<std::pair<std::string, double>> reciprocalRankFusion(
    const std::vector<std::pair<std::string, double>>& vectors,
    const std::vector<std::pair<std::string, double>>& lexical, size_t k,
    double vectorWeight, double lexicalWeight, double rankConstant) {
    if (!k || k > 10000 || vectors.size() > 10000 || lexical.size() > 10000 ||
        !std::isfinite(vectorWeight) || !std::isfinite(lexicalWeight) || vectorWeight < 0 || lexicalWeight < 0 ||
        vectorWeight > 1000000 || lexicalWeight > 1000000 || vectorWeight+lexicalWeight <= 0 ||
        !std::isfinite(rankConstant) || rankConstant < 1 || rankConstant > 1000000)
        throw std::invalid_argument("invalid reciprocal rank fusion parameters");
    std::map<std::string, double> scores;
    const auto combine = [&](const auto& input, double weight) {
        std::set<std::string> seen;
        for (size_t rank = 0; rank < input.size(); ++rank) {
            if (input[rank].first.empty() || input[rank].first.size() > 1024 ||
                !std::isfinite(input[rank].second) || !seen.insert(input[rank].first).second)
                throw std::invalid_argument("invalid or duplicate fusion input ID/score");
            if (weight > 0) scores[input[rank].first] += weight/(rankConstant+rank+1);
        }
    };
    combine(vectors, vectorWeight); combine(lexical, lexicalWeight);
    std::vector<std::pair<std::string, double>> result(scores.begin(), scores.end());
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.second > b.second || (a.second == b.second && a.first < b.first);
    });
    if (result.size() > k) result.resize(k);
    return result;
}
}
