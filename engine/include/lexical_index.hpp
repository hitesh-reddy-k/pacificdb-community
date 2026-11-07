#pragma once
#include "vector_memory_budget.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <memory_resource>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pacificdb {
struct LexicalConfig {
    size_t maxMemoryBytes{256ULL*1024*1024};
    size_t maxRecords{1000000}; // Includes tombstoned versions; rebuild to compact.
    size_t maxCandidates{1000000};
    double k1{1.2}, b{0.75};
};
// Derived tenant/collection-local inverted index. Authoritative text stays in
// WAL/LSM documents. No claim of phrases, stemming or linguistic tokenization.
class LexicalIndex {
public:
    using Check = std::function<void()>;
    explicit LexicalIndex(LexicalConfig config = {},
        std::shared_ptr<VectorAllocationBudget> sharedBudget = {});
    bool upsert(const std::string& id, const std::string& text, const Check& check = {});
    bool erase(const std::string& id);
    std::vector<std::pair<std::string, double>> search(const std::string& query,
        size_t k, bool requireAllTerms = false, const Check& check = {},
        const std::function<bool(const std::string&)>& eligible = {}) const;
    nlohmann::json stats() const;
    static std::vector<std::string> tokenize(const std::string& text, size_t maximumBytes,
                                           size_t maximumTokens);
private:
    struct Posting { size_t document; uint32_t frequency; };
    struct Term {
        explicit Term(std::pmr::memory_resource* resource) : postings(resource) {}
        size_t liveFrequency{0};
        std::pmr::vector<Posting> postings;
    };
    struct Document {
        explicit Document(std::pmr::memory_resource* resource) : id(resource), terms(resource) {}
        std::pmr::string id;
        std::pmr::vector<size_t> terms;
        size_t length{0};
        bool live{true};
    };
    void remove(size_t document);
    LexicalConfig config_;
    mutable VectorBudgetResource memory_;
    std::pmr::vector<Document> documents_;
    std::pmr::vector<Term> terms_;
    std::pmr::unordered_map<std::pmr::string, size_t> vocabulary_, ids_;
    mutable std::shared_mutex mutex_;
    size_t liveRecords_{0};
    uint64_t totalLength_{0};
    bool failed_{false};
};

// Ranked input IDs must already share authorization, filter and snapshot scope.
// The coordinator is responsible for authoritative visibility and final fencing.
std::vector<std::pair<std::string, double>> reciprocalRankFusion(
    const std::vector<std::pair<std::string, double>>& vectors,
    const std::vector<std::pair<std::string, double>>& lexical, size_t k,
    double vectorWeight = 1., double lexicalWeight = 1., double rankConstant = 60.);
}
