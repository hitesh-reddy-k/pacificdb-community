/**
 * @file hnsw_index.cpp
 * @description Phase E: HNSW (Hierarchical Navigable Small World) vector index
 *              implementation for the PacificDB C++ core engine.
 *
 * Derived graph core used by certificate-gated native ANN generations.
 * Authoritative records remain in WAL/LSM.
 */

#include "read_cache.hpp"
#include "vector_distance_kernels.hpp"
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
        config.maxElements == 0 || config.maxElements > std::numeric_limits<uint32_t>::max() || config.maxMemoryBytes < 1024 ||
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
        return static_cast<float>(1.0 - std::clamp(vector_kernels::dot(a,b,count), -1.0, 1.0));
    }
    }
}

std::vector<float> HNSWIndex::prepare(const std::vector<float>& vector) const {
    auto out=vector;
    if(config_.metric==DistanceMetric::COSINE){
        double norm=0.;for(float v:vector)norm+=double(v)*double(v);
        norm=std::sqrt(norm);
        if(norm>0.)for(float& v:out)v=static_cast<float>(double(v)/norm);
    }
    return out;
}
std::pmr::vector<size_t> HNSWIndex::selectNeighbors(
        const std::pmr::vector<std::pair<size_t,float>>& candidates,size_t maximum) const {
    std::pmr::vector<size_t> selected(&memory_);
    selected.reserve(std::min(maximum,candidates.size()));
    for(const auto& candidate:candidates){
        bool diverse=true;
        for(const auto other:selected)
            if(distance(nodes_[candidate.first].vector,nodes_[other].vector)<candidate.second){diverse=false;break;}
        if(diverse)selected.push_back(candidate.first);
        if(selected.size()==maximum)break;
    }
    // M is an upper bound. Leave rejected, dominated neighbors out so popular
    // lists do not require another pruning pass for every incoming edge.
    return selected;
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
    const std::vector<float>& query, size_t entryIdx, size_t ef, int level, const Check& check, bool liveOnly,VisitedScratch* reusable) const
{
    // Priority queues: candidates (min-heap) and results (max-heap)
    using PairDI = std::pair<float, size_t>; // (distance, index)

    auto cmpMin = [](const PairDI& a, const PairDI& b) { return a.first > b.first; };
    auto cmpMax = [](const PairDI& a, const PairDI& b) { return a.first < b.first; };

    std::priority_queue<PairDI, std::pmr::vector<PairDI>, decltype(cmpMin)>
        candidates(cmpMin, std::pmr::vector<PairDI>(&memory_));
    std::priority_queue<PairDI, std::pmr::vector<PairDI>, decltype(cmpMax)>
        results(cmpMax, std::pmr::vector<PairDI>(&memory_));

    // Bulk construction reuses one budgeted bitmap. Clear only touched words,
    // avoiding a full O(N) bitmap initialization for each inserted vertex.
    VisitedScratch local(&memory_);auto& scratch=reusable?*reusable:local;
    if(scratch.bits.size()<(nodes_.size()+63)/64)scratch.bits.resize((nodes_.size()+63)/64,0);
    struct Reset{VisitedScratch& scratch;~Reset(){for(auto word:scratch.words)scratch.bits[word]=0;scratch.words.clear();}}reset{scratch};
    const auto mark=[&](size_t index){auto& word=scratch.bits[index/64];const auto bit=uint64_t{1}<<(index%64);
        const bool seen=(word&bit)!=0;if(!word)scratch.words.push_back(index/64);word|=bit;return seen;};

    float d = distance(query, nodes_[entryIdx].vector);
    candidates.push({d, entryIdx});
    if(!liveOnly||!nodes_[entryIdx].deleted)results.push({d, entryIdx});
    mark(entryIdx);

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
                if (mark(nIdx)) continue;

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
bool HNSWIndex::insert(const std::string& id, const std::vector<float>& vec, const Check& check,const std::string& record) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    return insertLocked(id,vec,check,false,record);
}
bool HNSWIndex::upsert(const std::string& id,const std::vector<float>& vec,const Check& check,const std::string& record){
    std::unique_lock<std::shared_mutex> lock(mu_);
    return insertLocked(id,vec,check,true,record);
}
bool HNSWIndex::remove(const std::string& id,const Check& check){
    std::unique_lock<std::shared_mutex> lock(mu_);
    if(failed_)throw std::runtime_error("vector index unhealthy after failed mutation");
    if(check)check();
    const auto found=idToIndex_.find(std::pmr::string(id,&memory_));
    if(found==idToIndex_.end())return false;
    nodes_[found->second].deleted=true;idToIndex_.erase(found);return true;
}
bool HNSWIndex::insertLocked(const std::string& id,const std::vector<float>& vec,const Check& check,bool replace,const std::string& record){
    if (failed_ || staged_) throw std::runtime_error("vector index unhealthy or not built");

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

    const auto prepared=prepare(vec);
    if(old!=std::numeric_limits<size_t>::max() && nodes_[old].vector.size()==prepared.size() &&
       std::equal(prepared.begin(),prepared.end(),nodes_[old].vector.begin())){
        nodes_[old].record.assign(record.data(),record.size());return true;
    }
    if(nodes_.size()>=config_.maxElements)return false;

    const auto idx=nodes_.size();
    if(!appendLocked(id,vec,record))return false;
    if(old!=std::numeric_limits<size_t>::max())nodes_[old].deleted=true;
    connectLocked(idx,check);
    return true;
    } catch (...) {
        failed_ = true;
        throw;
    }
}
bool HNSWIndex::appendLocked(const std::string& id,const std::vector<float>& vec,const std::string& record){
    if(id.empty()||!validVector(vec)||nodes_.size()>=config_.maxElements)return false;
    if(config_.dimensions==0)config_.dimensions=vec.size();
    if(vec.size()!=config_.dimensions)return false;
    const auto prepared=prepare(vec);
    HNSWNode node(&memory_);
    node.id.assign(id.data(),id.size());node.vector.assign(prepared.begin(),prepared.end());
    node.record.assign(record.data(),record.size());node.neighbors.resize(randomLevel()+1);
    nodes_.push_back(std::move(node));idToIndex_.insert_or_assign(nodes_.back().id,nodes_.size()-1);
    return true;
}
bool HNSWIndex::stage(const std::string& id,const std::vector<float>& vec,const std::string& record,const Check& check){
    std::unique_lock<std::shared_mutex> lock(mu_);
    if(failed_||(!staged_&&!nodes_.empty()))throw std::runtime_error("invalid staged graph state");
    if(check)check();
    if(idToIndex_.count(std::pmr::string(id,&memory_)))return false;
    try{staged_=true;return appendLocked(id,vec,record);}catch(...){failed_=true;throw;}
}
void HNSWIndex::buildStaged(const Check& check){
    std::unique_lock<std::shared_mutex> lock(mu_);
    if(failed_)throw std::runtime_error("unhealthy staged graph");
    try{VisitedScratch scratch(&memory_);scratch.bits.resize((nodes_.size()+63)/64,0);
        for(size_t i=0;i<nodes_.size();++i){if(check)check();connectLocked(i,check,&scratch);}staged_=false;}
    catch(...){failed_=true;throw;}
}
void HNSWIndex::connectLocked(size_t idx,const Check& check,VisitedScratch* scratch){
    const auto level=static_cast<int>(nodes_[idx].neighbors.size()-1);
    const std::vector<float> prepared(nodes_[idx].vector.begin(),nodes_[idx].vector.end());
    if(idx==0){maxLevel_=level;entryPoint_=0;return;}
    // Navigate from top level to the node's level
    size_t ep = entryPoint_;
    for (int l = static_cast<int>(maxLevel_); l > level; --l) {
        auto results = searchLayer(prepared, ep, 1, l, check,false,scratch);
        if (!results.empty()) ep = results[0].first;
    }

    // Insert into each level
    for (int l = std::min(level, static_cast<int>(maxLevel_)); l >= 0; --l) {
        auto results = searchLayer(prepared, ep, config_.efConstruction, l, check,false,scratch);

        // Connect to nearest M neighbors
        size_t M = config_.M;
        const auto chosen=selectNeighbors(results,M);
        for (size_t neighborIdx:chosen) {

            // Bidirectional connection
            if (l < static_cast<int>(nodes_[idx].neighbors.size())) {
                nodes_[idx].neighbors[l].push_back(neighborIdx);
            }
            if (l < static_cast<int>(nodes_[neighborIdx].neighbors.size())) {
                nodes_[neighborIdx].neighbors[l].push_back(idx);

                // Prune if too many connections
                const size_t maximum=l==0?M*2:M;
                if (nodes_[neighborIdx].neighbors[l].size() > maximum) {
                    auto& nbs=nodes_[neighborIdx].neighbors[l];
                    std::pmr::vector<std::pair<size_t,float>> scored(&memory_);
                    for(auto nb:nbs){if(check)check();scored.push_back({nb,distance(nodes_[neighborIdx].vector,nodes_[nb].vector)});}
                    std::sort(scored.begin(),scored.end(),[](const auto& a,const auto& b){return a.second<b.second;});
                    const auto kept=selectNeighbors(scored,maximum);nbs.assign(kept.begin(),kept.end());
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
    const auto hits=searchRecordsLocked(query,k,check,efSearch,0,false);
    std::vector<std::pair<std::string,float>> out;out.reserve(hits.size());
    for(const auto& hit:hits)out.emplace_back(hit.id,hit.distance);
    return out;
}
std::vector<HNSWIndex::Hit> HNSWIndex::searchRecords(const std::vector<float>& query,size_t k,const Check& check,size_t efSearch,size_t maximumRecordBytes) const{
    std::shared_lock<std::shared_mutex> lock(mu_);
    return searchRecordsLocked(query,k,check,efSearch,maximumRecordBytes,true);
}
std::vector<HNSWIndex::Hit> HNSWIndex::searchRecordsLocked(const std::vector<float>& query,size_t k,const Check& check,size_t efSearch,size_t maximumRecordBytes,bool withRecords) const{
    if(check)check();
    if(failed_||staged_)throw std::runtime_error("vector index unhealthy or not built");
    if(!k||idToIndex_.empty()||!validVector(query)||query.size()!=config_.dimensions)return {};
    if(efSearch>config_.efSearch)throw std::invalid_argument("query ef_search exceeds configured bound");
    const auto prepared=prepare(query);size_t ep=entryPoint_;
    for(int l=static_cast<int>(maxLevel_);l>0;--l){auto r=searchLayer(prepared,ep,1,l,check);if(!r.empty())ep=r[0].first;}
    const auto results=searchLayer(prepared,ep,std::max(k,efSearch?efSearch:config_.efSearch),0,check,true);
    std::vector<Hit> out;out.reserve(std::min(results.size(),k));
    size_t recordBytes=0;
    for(size_t i=0;i<std::min(results.size(),k);++i){const auto& n=nodes_[results[i].first];
        if(withRecords&&(n.record.size()>maximumRecordBytes||recordBytes>maximumRecordBytes-n.record.size()))throw std::runtime_error("vector_candidate_bytes_exceeded");
        recordBytes+=n.record.size();
        out.push_back({std::string(n.id),results[i].second,withRecords?std::string(n.record):std::string()});}
    return out;
}
void HNSWIndex::visitLive(const Visit& visit,const Check& check) const{
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::pmr::vector<size_t> order(&memory_);order.reserve(idToIndex_.size());
    for(const auto& item:idToIndex_)order.push_back(item.second);
    std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return nodes_[a].id<nodes_[b].id;});
    for(auto idx:order){if(check)check();const auto& n=nodes_[idx];visit(n.id,n.vector.data(),n.vector.size(),n.record);}
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

namespace {
void put64(std::ostream& out,uint64_t n){for(unsigned i=0;i<8;++i)out.put(static_cast<char>(n>>(8*i)));if(!out)throw std::runtime_error("vector checkpoint write failed");}
uint64_t get64(std::istream& in){uint64_t n=0;for(unsigned i=0;i<8;++i){const auto c=in.get();if(c==std::char_traits<char>::eof())throw std::runtime_error("truncated vector checkpoint");n|=uint64_t(static_cast<unsigned char>(c))<<(8*i);}return n;}
void putText(std::ostream& out,std::string_view s){put64(out,s.size());out.write(s.data(),s.size());if(!out)throw std::runtime_error("vector checkpoint write failed");}
std::string getText(std::istream& in){const auto n=get64(in);if(n>1024)throw std::runtime_error("invalid checkpoint ID");std::string s(n,'\0');in.read(s.data(),n);if(!in)throw std::runtime_error("truncated checkpoint ID");return s;}
}
void HNSWIndex::writeLinks(std::ostream& out,const Check& check) const{
    std::shared_lock<std::shared_mutex> lock(mu_);
    if(failed_||staged_)throw std::runtime_error("checkpoint requires a healthy built graph");
    put64(out,0x31534b4e494c4450ULL); // PDLINKS1, little endian
    put64(out,nodes_.size());put64(out,idToIndex_.size());put64(out,maxLevel_);put64(out,entryPoint_);
    for(const auto& n:nodes_){
        if(check)check();putText(out,n.id);put64(out,n.deleted?1:0);put64(out,n.neighbors.size());
        for(float value:n.vector){uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));put64(out,bits);}
    }
    for(const auto& n:nodes_){if(check)check();for(const auto& layer:n.neighbors){put64(out,layer.size());for(auto id:layer)put64(out,id);}}
}
void HNSWIndex::readLinks(std::istream& in,const Check& check){
    std::unique_lock<std::shared_mutex> lock(mu_);
    if(failed_||(!staged_&&!nodes_.empty()))throw std::runtime_error("checkpoint restore requires staged authoritative records");
    const auto original=nodes_.size();
    try{
        if(get64(in)!=0x31534b4e494c4450ULL)throw std::runtime_error("unsupported graph checkpoint format");
        const auto count=get64(in),live=get64(in),maximum=get64(in),entry=get64(in);
        if(count>config_.maxElements||live!=idToIndex_.size()||maximum>16||(count&&entry>=count)||(!count&&(entry||maximum)))
            throw std::runtime_error("invalid graph checkpoint shape");
        std::pmr::vector<uint32_t> mapping(&memory_);mapping.reserve(count);
        std::pmr::vector<uint8_t> seen(original,0,&memory_);
        for(size_t i=0;i<count;++i){
            if(check)check();const auto id=getText(in);const auto deleted=get64(in),levels=get64(in);
            if(id.empty()||deleted>1||levels<1||levels>17||levels>maximum+1)throw std::runtime_error("invalid graph checkpoint node");
            size_t current;
            if(!deleted){const auto found=idToIndex_.find(std::pmr::string(id,&memory_));
                if(found==idToIndex_.end()||seen[found->second]++)throw std::runtime_error("checkpoint source ID mismatch");current=found->second;
            }else{if(nodes_.size()>=config_.maxElements)throw std::runtime_error("checkpoint physical capacity exceeded");
                HNSWNode node(&memory_);node.id.assign(id.data(),id.size());node.deleted=true;node.vector.resize(config_.dimensions);
                nodes_.push_back(std::move(node));current=nodes_.size()-1;}
            auto& n=nodes_[current];n.neighbors.clear();n.neighbors.resize(levels);
            for(size_t d=0;d<config_.dimensions;++d){const auto raw=get64(in);if(raw>UINT32_MAX)throw std::runtime_error("invalid checkpoint float");
                const uint32_t bits=raw;float value;std::memcpy(&value,&bits,sizeof(value));
                if(!std::isfinite(value)||(!deleted&&value!=n.vector[d]))throw std::runtime_error("checkpoint source vector mismatch");
                if(deleted)n.vector[d]=value;
            }mapping.push_back(current);
        }
        if(std::find(seen.begin(),seen.end(),0)!=seen.end())throw std::runtime_error("checkpoint omits source records");
        for(size_t i=0;i<count;++i){if(check)check();auto& n=nodes_[mapping[i]];
            for(size_t l=0;l<n.neighbors.size();++l){const auto degree=get64(in);
                if(degree>(l?config_.M:2*config_.M))throw std::runtime_error("invalid checkpoint degree");
                auto& links=n.neighbors[l];links.reserve(degree);
                for(size_t j=0;j<degree;++j){const auto target=get64(in);
                    if(target>=count||target==i||nodes_[mapping[target]].neighbors.size()<=l||
                       std::find(links.begin(),links.end(),mapping[target])!=links.end())throw std::runtime_error("invalid checkpoint edge");
                    links.push_back(mapping[target]);}
            }
        }
        if(in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("trailing graph checkpoint bytes");
        if(count&&nodes_[mapping[entry]].neighbors.size()!=maximum+1)throw std::runtime_error("invalid checkpoint entry point");
        maxLevel_=maximum;entryPoint_=count?mapping[entry]:0;staged_=false;
    }catch(...){
        nodes_.resize(original);
        for(auto& n:nodes_){for(auto& layer:n.neighbors)layer.clear();if(n.neighbors.empty())n.neighbors.resize(1);}
        maxLevel_=entryPoint_=0;staged_=!nodes_.empty();throw;
    }
}

} // namespace pacificdb
