#include "flat_vector_index.hpp"
#include "vector_numeric.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace pacificdb {
FlatVectorIndex::FlatVectorIndex(FlatVectorConfig config,
        std::shared_ptr<VectorAllocationBudget> sharedBudget)
    : config_(std::move(config)), memory_(config_.maxMemoryBytes,std::move(sharedBudget)),
      values_(&memory_), norms_(&memory_), ids_(&memory_), byId_(&memory_) {
    if (!config_.dimensions || config_.dimensions > 65536 || !config_.maxElements ||
        config_.maxElements > 10000000 || config_.maxMemoryBytes < 1024 ||
        (config_.metric != "cosine" && config_.metric != "l2" && config_.metric != "dot"))
        throw std::invalid_argument("invalid flat vector configuration");
}
void FlatVectorIndex::reserve(size_t elements) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (frozen_ || failed_) throw std::runtime_error("flat index is not buildable");
    if (elements > config_.maxElements || elements > std::numeric_limits<size_t>::max() / config_.dimensions)
        throw std::invalid_argument("flat index reservation exceeds limits");
    try {
        values_.reserve(elements * config_.dimensions);
        norms_.reserve(elements); ids_.reserve(elements); byId_.reserve(elements);
    } catch (...) { failed_ = true; throw; }
}
bool FlatVectorIndex::append(const std::string& id,const std::vector<double>& vector) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (frozen_ || failed_) throw std::runtime_error("flat index is not buildable");
    if (id.empty() || id.size() > 1024 || vector.size() != config_.dimensions ||
        ids_.size() >= config_.maxElements ||
        !std::all_of(vector.begin(),vector.end(),[](double x){return std::isfinite(x);})) return false;
    try {
        const std::pmr::string key(id,&memory_);
        if (byId_.count(key)) return false;
        double norm = 0; for(double value:vector) norm += value*value;
        const size_t index = ids_.size();
        values_.insert(values_.end(),vector.begin(),vector.end());
        norms_.push_back(std::sqrt(norm));
        ids_.emplace_back(id.data(),id.size());
        byId_.emplace(ids_.back(),index);
        return true;
    } catch (...) { failed_ = true; throw; }
}
void FlatVectorIndex::freeze() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (failed_) throw std::runtime_error("flat index unhealthy after failed build");
    frozen_ = true;
}
std::vector<std::pair<std::string,double>> FlatVectorIndex::search(
        const std::vector<double>& query,size_t k,const Check& check,size_t checkEvery) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (!frozen_ || failed_) throw std::runtime_error("flat index is not READY");
    if (query.size() != config_.dimensions || k == 0 || k > 10000 || checkEvery == 0 || checkEvery > 65536 ||
        !std::all_of(query.begin(),query.end(),[](double x){return std::isfinite(x);}))
        throw std::invalid_argument("invalid flat vector query");
    if(check)check(0);
    struct Candidate {double score;size_t row;};
    const auto better=[&](const Candidate& left,const Candidate& right){
        return left.score > right.score || (left.score == right.score && ids_[left.row] < ids_[right.row]);
    };
    std::pmr::vector<Candidate> top(&memory_);
    top.reserve(std::min(k,ids_.size()));
    double queryNorm = 0; for(double x:query)queryNorm += x*x;
    queryNorm=std::sqrt(queryNorm);
    const bool l2 = config_.metric=="l2", dot=config_.metric=="dot";
    for(size_t row=0;row<ids_.size();++row){
        if(check && row % checkEvery == 0)check(row);
        const double* vector=values_.data()+row*config_.dimensions;
        double score=0;
        for(size_t d=0;d<config_.dimensions;++d){
            if(l2){const double delta=query[d]-vector[d];score+=delta*delta;}
            else score+=query[d]*vector[d];
        }
        bool wide=false;
        if(l2)score=-std::sqrt(score);
        else if(!dot){const double denominator=queryNorm*norms_[row];wide=!std::isfinite(denominator);score=denominator<1e-12?0.:score/denominator;}
        if(wide||!std::isfinite(score))score=vectors::wideDenseScore(query,[&](size_t d){return vector[d];},config_.metric);
        Candidate candidate{score,row};
        if(top.size()==k){
            if(!better(candidate,top.front()))continue;
            std::pop_heap(top.begin(),top.end(),better);top.pop_back();
        }
        top.push_back(candidate);std::push_heap(top.begin(),top.end(),better);
    }
    if(check)check(ids_.size());
    std::sort(top.begin(),top.end(),better);
    // Bounded result ownership belongs to the caller, like HNSW search results.
    std::vector<std::pair<std::string,double>> result;result.reserve(top.size());
    for(const auto& hit:top)result.emplace_back(std::string(ids_[hit.row]),hit.score);
    return result;
}
bool FlatVectorIndex::matches(const std::string& id,const std::vector<double>& values) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (!frozen_ || failed_ || values.size() != config_.dimensions) return false;
    const auto found = byId_.find(std::pmr::string(id,&memory_));
    if (found == byId_.end()) return false;
    const double* stored = values_.data() + found->second * config_.dimensions;
    return std::equal(values.begin(),values.end(),stored);
}
size_t FlatVectorIndex::size() const {std::shared_lock<std::shared_mutex> lock(mutex_);return ids_.size();}
nlohmann::json FlatVectorIndex::stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return {{"size",ids_.size()},{"dimensions",config_.dimensions},{"metric",config_.metric},
        {"precision","float64"},{"frozen",frozen_},{"healthy",!failed_},
        {"allocated_bytes",memory_.accounting().used()},{"peak_allocated_bytes",memory_.accounting().peak()},
        {"memory_limit_bytes",config_.maxMemoryBytes}};
}
}
