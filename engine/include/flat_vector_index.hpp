#pragma once
#include "vector_memory_budget.hpp"
#include <cmath>
#include <functional>
#include <memory_resource>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace pacificdb {
struct FlatVectorConfig {
    size_t dimensions{0};
    size_t maxElements{1000000};
    size_t maxMemoryBytes{256ULL * 1024 * 1024};
    std::string metric{"cosine"};
};
// Derived float64 exact index. The engine's WAL/LSM records stay authoritative.
// Build privately, then freeze and publish through the generation manager.
class FlatVectorIndex {
public:
    using Check = std::function<void(size_t)>;
    explicit FlatVectorIndex(FlatVectorConfig config,
        std::shared_ptr<VectorAllocationBudget> sharedBudget = {});
    void reserve(size_t elements);
    bool append(const std::string& id, const std::vector<double>& vector);
    void freeze();
    std::vector<std::pair<std::string,double>> search(const std::vector<double>& query,
                                                   size_t k, const Check& check = {}, size_t checkEvery = 1) const;
    bool matches(const std::string& id, const std::vector<double>& values) const;
    size_t size() const;
    nlohmann::json stats() const;
private:
    FlatVectorConfig config_;
    mutable VectorBudgetResource memory_;
    std::pmr::vector<double> values_;
    std::pmr::vector<double> norms_;
    std::pmr::vector<std::pmr::string> ids_;
    std::pmr::unordered_map<std::pmr::string,size_t> byId_;
    mutable std::shared_mutex mutex_;
    bool frozen_{false};
    bool failed_{false};
};
}
