#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace pacificdb::vector_search {

// Retains at most k documents from a version-resolved iterator. Materialize
// while the row is pinned: a later ID lookup could fetch a different version.
class TopK {
public:
    explicit TopK(std::size_t k, std::size_t maxDocumentBytes)
        : k_(k), maxBytes_(maxDocumentBytes) {}

    template<class Materialize>
    void consider(double score, std::string id, Materialize&& materialize) {
        if (!std::isfinite(score) || k_ == 0) return;
        Entry candidate{score, std::move(id), {}, 0};
        if (heap_.size() == k_ && !better(candidate, heap_.front())) return;
        candidate.doc = materialize();
        candidate.bytes = candidate.doc.dump().size();
        const std::size_t removed = heap_.size() == k_ ? heap_.front().bytes : 0;
        if (candidate.bytes > maxBytes_ || bytes_ - removed > maxBytes_ - candidate.bytes) {
            throw std::runtime_error("vector_candidate_bytes_exceeded");
        }
        bytes_ = bytes_ - removed + candidate.bytes;
        if (heap_.size() == k_) {
            std::pop_heap(heap_.begin(), heap_.end(), better);
            heap_.pop_back();
        }
        heap_.push_back(std::move(candidate));
        std::push_heap(heap_.begin(), heap_.end(), better);
    }

    std::size_t size() const { return heap_.size(); }

    std::vector<nlohmann::json> finish() {
        std::sort(heap_.begin(), heap_.end(), better);
        std::vector<nlohmann::json> result;
        result.reserve(heap_.size());
        for (auto& entry : heap_) {
            entry.doc["score"] = entry.score;
            result.push_back(std::move(entry.doc));
        }
        heap_.clear();
        bytes_ = 0;
        return result;
    }

private:
    struct Entry {
        double score;
        std::string id;
        nlohmann::json doc;
        std::size_t bytes;
    };
    static bool better(const Entry& a, const Entry& b) {
        return a.score > b.score || (a.score == b.score && a.id < b.id);
    }
    std::size_t k_;
    std::size_t maxBytes_;
    std::size_t bytes_{0};
    std::vector<Entry> heap_;
};

} // namespace pacificdb::vector_search
