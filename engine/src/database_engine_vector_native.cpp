#include "database_engine.hpp"
#include "native_vector_index_service.hpp"
#include "vector_index_catalog.hpp"
#include "vector_value.hpp"
#include "vector_schema.hpp"
#include "vector_change_registry.hpp"
#include "vector_top_k.hpp"
#include "native_vector_record.hpp"
#include "native_vector_checkpoint.hpp"
#include "storage_format_v2.hpp"
#include "lsm.hpp"
#include "memory_manager.hpp"
#include "query_cancellation.hpp"
#include "query_limiter.hpp"
#include "query.hpp"
#include "raft_core.hpp"
#include "test_failpoint.hpp"
#include <chrono>
#include <mutex>

namespace {
using namespace pacificdb;
std::mutex serviceMutex;
std::shared_ptr<NativeVectorIndexService> activeService;
std::shared_ptr<NativeVectorIndexService> service() {
    std::lock_guard<std::mutex> lock(serviceMutex);
    if (!activeService) {
        const auto budget = MemoryManager::getMaxMemoryBytes();
        activeService = std::make_shared<NativeVectorIndexService>(2,
            std::min<size_t>(1024ULL*1024*1024,budget/4),
            std::min<size_t>(512ULL*1024*1024,budget/8));
    }
    return activeService;
}
void admit(const json& spec) {
    const auto budget = MemoryManager::getMaxMemoryBytes();
    const auto allowed = budget - budget/5;
    const auto rss = MemoryManager::getProcessMemory();
    const size_t peak = spec.at("max_index_bytes").get<size_t>() + spec.at("max_source_bytes").get<size_t>() +
        (spec.at("text_field").get<std::string>().empty() ? 0 : spec.at("max_lexical_bytes").get<size_t>());
    if (MemoryManager::getPressureState() >= MemoryPressureState::THROTTLED || rss >= allowed || peak > allowed-rss)
        throw std::runtime_error("vector_index_global_memory_admission_exceeded");
    const auto limits = service()->metrics();
    if (spec.at("max_index_bytes").get<size_t>() > limits.at("graph_limit_bytes").get<size_t>() ||
        spec.at("max_source_bytes").get<size_t>() > limits.at("source_limit_bytes").get<size_t>())
        throw std::runtime_error("vector_index_global_resource_budget_exceeded");
}
std::string key(const std::string& user, const std::string& db, const std::string& collection,
                const std::string& name) {
    return json::array({DatabaseEngine::getDataRoot(), user, db, collection, name}).dump();
}
std::optional<VectorFreshness::Stamp> certificate(const std::string& user, const std::string& db,
                                                 const std::string& collection,bool requireApplied=true) {
    // Background work may inspect a stable physical source during unrelated
    // apply traffic. Only query/status certificates may authorize a response,
    // and those still require the exact committed/applied read boundary.
    for(size_t attempt=0;attempt<(requireApplied?1:32);++attempt){
        const auto before=LSM::vectorCertificate(user,db,collection);
        const auto spec=LSM::vectorCertificate(user,"pacificdb_meta","vector_index_specs_v1");
        auto after=LSM::vectorCertificate(user,db,collection);
        if(!before||!spec||!after||before->root!=spec->root)continue;
        const bool stable=requireApplied
            ? before==after && before->visibility==spec->visibility
            : after->sameSourceAtOrAfter(*before) && before->visibility<=spec->visibility && spec->visibility<=after->visibility;
        if(!stable)continue;
        // A committed vector entry can precede its LSM apply. It cannot be
        // declared irrelevant merely because the physical revision is stable.
        const auto& raft=RaftCore::instance();
        if(requireApplied && raft.isEnabled() && RaftCore::instance().getLastApplied()<after->visibility)return {};
        after->configuration=spec->collection;return after;
    }
    return {};
}
struct Definition { json spec; VectorFreshness::Stamp stamp; };
Definition definition(const std::string& user, const std::string& db,
                      const std::string& collection, const std::string& name,bool requireApplied=true) {
    const auto before = certificate(user, db, collection,requireApplied);
    const auto matches=[&](const std::optional<VectorFreshness::Stamp>& after){
        return before && after && (*after==*before || (!requireApplied && after->sameSourceAtOrAfter(*before)));
    };
    // A READY generation certifies this process-local catalog copy. A metadata,
    // root, collection or visibility change forces the authoritative slow path.
    if(before){
        const auto current=service();const auto identity=key(user,db,collection,name);
        const auto manager=current->get(identity);
        if(manager&&manager->acquire(*before)){
            const auto cached=current->definition(identity,manager);
            const auto after=certificate(user,db,collection,requireApplied);
            if(cached&&matches(after))return {*cached,*after};
        }
    }
    const auto definitions = VectorIndexCatalog::list(user, db, collection);
    const auto after = certificate(user, db, collection,requireApplied);
    if (!matches(after)) throw std::runtime_error("vector_index_source_changing");
    for (const auto& spec : definitions) if (spec.at("name") == name) return {spec, *after};
    throw std::runtime_error("vector_index_unavailable: ABSENT");
}
const json* selectedVector(const json& document, const json& spec) {
    const auto name = spec.at("vector_name").get<std::string>();
    if (name.empty()) {
        const auto found = document.find("vector");
        return found == document.end() ? nullptr : &*found;
    }
    const auto named = document.find("vectors");
    if (named == document.end() || !named->is_object() || !named->contains(name)) return nullptr;
    return &named->at(name);
}
std::string stateName(VectorIndexGeneration::State state) {
    using State = VectorIndexGeneration::State;
    switch (state) {
        case State::ABSENT: return "ABSENT";
        case State::QUEUED: return "QUEUED";
        case State::BUILDING: return "BUILDING";
        case State::READY: return "READY";
        case State::STALE: return "STALE";
        case State::FAILED: return "FAILED";
    }
    throw std::runtime_error("invalid vector index state");
}
std::vector<float> derived(const vectors::Value& value) {
    if (value.sparse) throw std::invalid_argument("native HNSW requires dense vectors");
    std::vector<float> result; result.reserve(value.values.size());
    for (double number : value.values) {
        if (std::abs(number) > std::numeric_limits<float>::max())
            throw std::invalid_argument("native HNSW float32 range exceeded");
        result.push_back(static_cast<float>(number));
    }
    return result;
}
}

void DatabaseEngine::retireNativeVectorIndexes(const std::string& user,const std::string& db,
                                               const std::string& collection) {
    std::shared_ptr<pacificdb::NativeVectorIndexService> current;
    { std::lock_guard<std::mutex> lock(serviceMutex); current=activeService; }
    if (!current) return;
    const auto root=DatabaseEngine::getDataRoot();
    current->retire([&](const std::string& identity){
        const auto parts=json::parse(identity);
        return parts.at(0)==root&&parts.at(1)==user&&parts.at(2)==db&&parts.at(3)==collection;
    });
}
void DatabaseEngine::stopNativeVectorIndexes() {
    std::shared_ptr<pacificdb::NativeVectorIndexService> previous;
    { std::lock_guard<std::mutex> lock(serviceMutex); previous.swap(activeService); }
    if (previous) previous->shutdown();
}
json DatabaseEngine::buildNativeVectorIndex(const std::string& user, const std::string& db,
                                           const std::string& collection, const json& options) {
    const auto spec = pacificdb::VectorIndexCatalog::validate(options);
    if (spec.at("field") != "vector") throw std::invalid_argument("native HNSW field must be vector");
    const auto name = spec.at("name").get<std::string>();
    admit(spec);
    pacificdb::VectorIndexCatalog::configure(user, db, collection, spec);
    service()->unload(key(user, db, collection, name));
    return rebuildNativeVectorIndex(user, db, collection, name);
}
json DatabaseEngine::rebuildNativeVectorIndex(const std::string& user, const std::string& db,
                                             const std::string& collection, const std::string& name) {
    const auto loaded = definition(user, db, collection, name,false);
    const auto spec = loaded.spec;
    admit(spec);
    if (spec.at("field") != "vector") throw std::invalid_argument("native HNSW field must be vector");
    const bool incremental=spec.value("maintenance",std::string())=="incremental";
    std::shared_ptr<pacificdb::VectorChangeJournal> journal;
    if(incremental){
        const auto schema=pacificdb::VectorSchemaRegistry::get(getDataRoot(),user,db,collection);
        if(!schema)throw std::runtime_error("vector_schema required for incremental maintenance");
        pacificdb::VectorSchema::index(*schema,spec);
        journal=pacificdb::VectorChangeRegistry::subscribe(getDataRoot(),user,db,collection);
    }
    struct Anchor {pacificdb::VectorChangeJournal::Cursor cursor;};
    auto anchor=std::make_shared<Anchor>();
    const auto config = pacificdb::VectorIndexCatalog::graphConfig(spec);
    std::optional<pacificdb::LexicalConfig> lexical;
    if (!spec.at("text_field").get<std::string>().empty()) {
        lexical = pacificdb::LexicalConfig{};
        lexical->maxMemoryBytes = spec.at("max_lexical_bytes").get<size_t>();
        lexical->maxRecords = config.maxElements;
        lexical->maxCandidates = config.maxElements;
    }
    const auto stamp = loaded.stamp;
    auto provider = [user, db, collection, stamp]() -> std::optional<pacificdb::VectorFreshness::Stamp> {
        auto current = certificate(user, db, collection,false);
        if (!current || current->root != stamp.root || current->configuration != stamp.configuration) return {};
        return current;
    };
    auto maintenanceCertificate=provider;
    auto source = [user, db, collection, spec, incremental, journal, anchor](const pacificdb::VectorIndexGeneration::Emit& emit,
                                              const std::atomic<bool>& cancelled) {
        if(journal)anchor->cursor=journal->capture();
        // No socket/worker context or borrowed request memory crosses this handoff.
        QueryLimiter::Scope request;
        QueryCancel::Scope token(QueryCancel::makeToken());
        DatabaseEngine::clearReadContext();
        struct Cleanup { ~Cleanup() { DatabaseEngine::clearReadContext(); } } cleanup;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(spec.at("max_build_ms").get<size_t>());
        std::vector<std::string> fields{"id", "tenant_id", "_deleted", "_visibility_state", "committed",
            "_raft_commit_index", "_visibility_floor", "version", "_mvcc_commit_ms"};
        fields.push_back(spec.at("vector_name").get<std::string>().empty() ? "vector" : "vectors");
        const auto textField = spec.at("text_field").get<std::string>();
        if (!textField.empty() && std::find(fields.begin(), fields.end(), textField) == fields.end())
            fields.push_back(textField);
        const bool exactOnly = spec.value("algorithm",std::string("hnsw")) == "flat";
        size_t scanned = 0;
        LSM::visitLatest(user, db, collection, [&](const LSM::LatestRowView& row) {
            if (cancelled.load()) throw std::runtime_error("vector build cancelled");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("vector_index_build_timeout");
            if (++scanned > spec.at("max_scan_rows").get<size_t>())
                throw std::runtime_error("vector_index_source_scan_limit");
            auto document = row.project(fields);
            if(incremental&&document.value("tenant_id",user)==user){
                auto visibilityProbe=document;visibilityProbe.erase("_deleted");
                const auto visible=DatabaseEngine::nativeVectorRowVisible(user,visibilityProbe);
                const bool pending=document.value("_visibility_state",std::string())=="PENDING"||
                    (document.contains("committed")&&document.at("committed")==false);
                if(pending||!visible)throw std::runtime_error("vector_index_incremental_pending_source_requires_rebuild");
            }
            if (!DatabaseEngine::nativeVectorRowVisible(user, document)) return true;
            const auto selected = selectedVector(document, spec);
            std::optional<std::string> text;
            if (!textField.empty()) {
                const auto found = document.find(textField);
                if (found != document.end() && found->is_string()) {
                    const auto& raw = found->get_ref<const std::string&>();
                    if (raw.size() > 1024*1024) throw std::runtime_error("lexical text byte limit exceeded");
                    text = raw;
                }
            }
            std::vector<float> vector;
            std::vector<double> precise;
            if (selected) {
                if (!selected->is_array()) throw std::invalid_argument("native index requires dense vectors");
                if (selected->size() == spec.at("dimensions").get<size_t>()) {
                    const auto decoded = pacificdb::vectors::decode(*selected);
                    if (exactOnly) precise = decoded.values;
                    else vector = derived(decoded);
                }
            }
            if (vector.empty() && precise.empty() && !text) return true;
            if (!document.contains("id") || !document.at("id").is_string())
                throw std::runtime_error("vector index source ID invalid");
            const auto id = document.at("id").get<std::string>();
            if (id.empty() || id.size() > 1024) throw std::runtime_error("vector index source ID limit");
            auto packed = !exactOnly && textField.empty() ? pacificdb::packNativeVectorRecord(row.materialize()) : std::string();
            emit({id, std::move(vector), std::move(text), std::move(precise),std::move(packed)});
            return true;
        },incremental,incremental);
    };
    pacificdb::VectorIndexGeneration::Ready ready;
    if(incremental){
        ready=[user,db,collection,spec,journal,anchor,maintenanceCertificate](pacificdb::VectorIndexGeneration& manager){
            auto resolve=[user,db,collection,spec](const std::string& id)->std::optional<pacificdb::VectorIndexGeneration::Record>{
                QueryLimiter::Scope request;
                QueryCancel::Scope cancellation(QueryCancel::makeToken());
                DatabaseEngine::clearReadContext();
                struct Cleanup{~Cleanup(){DatabaseEngine::clearReadContext();}}cleanup;
                if(MemoryManager::getPressureState()>=MemoryPressureState::THROTTLED)
                    throw std::runtime_error("vector_index_global_memory_pressure");
                LSM::invalidateIdCache(user,db,collection,id);
                const auto fetched=LSM::findStrongSnapshotBound(user,db,collection,id,0);
                if(!fetched.value("found",false))return {};
                if(!fetched.contains("documents")||fetched.at("documents").size()!=1)
                    throw std::runtime_error("vector incremental ambiguous source version");
                const auto& document=fetched.at("documents").front();
                if(!DatabaseEngine::nativeVectorRowVisible(user,document))return {};
                const auto selected=selectedVector(document,spec);if(!selected)return {};
                const auto decoded=pacificdb::vectors::decode(*selected);
                if(decoded.sparse||decoded.dimensions!=spec.at("dimensions"))
                    throw std::runtime_error("vector incremental source shape mismatch");
                return pacificdb::VectorIndexGeneration::Record{id,derived(decoded),{},{},pacificdb::packNativeVectorRecord(document)};
            };
            // The incremental full-source pass rejects every pending/invisible
            // row, and the journal blocks changes until their visibility floor.
            // A later unrelated Raft commit therefore cannot add an omitted row.
            manager.enableMaintenance(journal,anchor->cursor,maintenanceCertificate,std::move(resolve),128,1000,true);
        };
    }
    auto checkpoints = !lexical && spec.value("algorithm",std::string("hnsw"))!="flat"
        ? pacificdb::nativeVectorCheckpoints(getDataRoot(),key(user,db,collection,name),spec,config.maxMemoryBytes)
        : pacificdb::VectorIndexGeneration::Checkpoints{};
    service()->queue(key(user, db, collection, name), spec, config,
        spec.at("max_source_bytes").get<size_t>(), std::move(provider), std::move(source), lexical,
        spec.value("algorithm",std::string("hnsw")) == "flat", [] {
            if (MemoryManager::getPressureState() >= MemoryPressureState::THROTTLED)
                throw std::runtime_error("vector_index_global_memory_pressure");
        },std::move(ready),std::move(checkpoints),incremental);
    return nativeVectorIndexStatus(user, db, collection, name);
}
json DatabaseEngine::nativeVectorIndexStatus(const std::string& user, const std::string& db,
                                            const std::string& collection, const std::string& name) {
    const auto loaded = definition(user, db, collection, name,false);
    const auto manager = service()->get(key(user, db, collection, name));
    json result{{"name", name}, {"spec", loaded.spec}, {"state", "ABSENT"}, {"generation", 0},
                {"resources", service()->metrics()}};
    if (!manager) {result["rebuild_required"]=true;return result;}
    const auto status = manager->status();
    result["state"] = stateName(status.state); result["generation"] = status.generation;
    if (!status.error.empty()) result["error"] = status.error;
    // Metadata remains observable during unrelated apply traffic, but READY
    // still requires the same applied/source/journal fence as public queries.
    const auto eligible=certificate(user,db,collection);
    auto generation = eligible ? manager->acquire(*eligible) : nullptr;
    if (status.state == pacificdb::VectorIndexGeneration::State::READY && !generation) result["state"] = "STALE";
    if (result.at("state") == "STALE") manager->requestMaintenance();
    if (generation) {
        result["checkpoint_restored"]=generation->checkpointRestored;
        if(!generation->checkpointError.empty())result["checkpoint_error"]=generation->checkpointError;
        if (generation->graph) result["graph"] = generation->graph->getStats();
        if (generation->flat) result["flat"] = generation->flat->stats();
        if (generation->lexical) result["lexical"] = generation->lexical->stats();
    }
    result["rebuild_required"]=result.at("state")!="READY";
    result["maintenance_policy"]=loaded.spec.value("maintenance",std::string("manual"));
    return result;
}
bool DatabaseEngine::dropNativeVectorIndex(const std::string& user, const std::string& db,
                                          const std::string& collection, const std::string& name) {
    const auto removed = pacificdb::VectorIndexCatalog::erase(user, db, collection, name);
    service()->unload(key(user, db, collection, name));
    return removed;
}
std::vector<json> DatabaseEngine::queryNativeVectorIndex(const std::string& user, const std::string& db,
    const std::string& collection, const std::string& name, const json& query, json* execution, DatabaseEngine::VectorDeadline deadline) {
    const auto start = std::chrono::steady_clock::now();
    const auto queryDeadline = pacificdb::vector_query::bound(start,QueryLimiter::getMaxQueryTime(),deadline);
    const auto check = [&] {
        if (QueryCancel::isCancelled()) throw std::runtime_error("vector_query_cancelled");
        if (std::chrono::steady_clock::now() > queryDeadline)
            throw std::runtime_error("vector_query_timeout");
    };
    check();
    const auto requestState = QueryLimiter::capture();
    // The public RPC owns this counter across bounded freshness retries.
    // Resetting it here would let retries multiply the per-request scan budget.
    if (!nativeVectorReadContextEligible()) throw std::runtime_error("vector_index_read_context_requires_exact");
    if ((query.contains("filter") && !query.at("filter").empty()) ||
        !query.value("modality", std::string()).empty())
        throw std::runtime_error("vector_index_filter_requires_exact");
    const auto loaded = definition(user, db, collection, name); check();
    const auto value = pacificdb::vectors::decode(query.at("vector"));
    if (query.contains("vector_name")) pacificdb::vectors::validateName(query.at("vector_name").get<std::string>());
    const auto floatQuery = derived(value);
    auto metric = query.value("metric", std::string("cosine"));
    if (metric == "euclidean") metric = "l2";
    if (metric == "dot_product") metric = "dot";
    if (const auto schema=pacificdb::VectorSchemaRegistry::get(getDataRoot(),user,db,collection))
        pacificdb::VectorSchema::query(*schema,query.value("vector_name",std::string()),value,metric);
    if (metric != loaded.spec.at("metric") || value.dimensions != loaded.spec.at("dimensions") ||
        query.value("vector_name", std::string()) != loaded.spec.at("vector_name"))
        throw std::invalid_argument("vector index space/dimension/metric mismatch");
    const auto kValue = query.value("k", json(10));
    if (!kValue.is_number_integer() || (!kValue.is_number_unsigned() && kValue.get<int64_t>() <= 0) ||
        kValue.get<uint64_t>() > 10000 || kValue.get<uint64_t>() > QueryLimiter::getMaxResultDocs())
        throw std::invalid_argument("native vector k exceeds query limits");
    const auto k = kValue.get<size_t>();
    const auto manager = service()->get(key(user, db, collection, name));
    const auto generation = manager ? manager->acquire(loaded.stamp) : nullptr;
    if (!generation) {
        if(manager)manager->requestMaintenance();
        throw std::runtime_error("vector_index_unavailable: not READY/fresh");
    }
    if (!generation->graph) throw std::runtime_error("vector_index_unavailable: requested ANN on exact-only index");
    const auto efValue=query.value("ef_search",loaded.spec.at("ef_search"));
    if(!efValue.is_number_integer()||(!efValue.is_number_unsigned()&&efValue.get<int64_t>()<=0)||
       efValue.get<uint64_t>()>loaded.spec.at("ef_search").get<size_t>())
        throw std::invalid_argument("query ef_search must be within configured positive bound");
    const auto efSearch=efValue.get<size_t>();
    const auto graphStart=std::chrono::steady_clock::now();
    const auto count = std::min(generation->graph->size(),
        std::max(k, std::min(efSearch, k*4)));
    auto candidates = generation->graph->searchRecords(floatQuery, count, check,efSearch,QueryLimiter::getMaxResultBytes());
    const auto graphEnd=std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration resolutionTime{},scoringTime{};
    pacificdb::vector_search::TopK top(k, QueryLimiter::getMaxResultBytes());
    size_t cacheHits=0,generationHits=0,authoritativeFetches=0,materializations=0;
    struct Scored {double score;size_t candidate;};std::vector<Scored> scored;scored.reserve(candidates.size());
    const std::vector<std::string> visibilityFields{"id","tenant_id","_deleted","_visibility_state","committed",
        "_raft_commit_index","_visibility_floor","version","_mvcc_commit_ms"};
    size_t candidateBytes=0;
    for(size_t index=0;index<candidates.size();++index){
        auto& candidate=candidates[index];check();
        if(requestState->scannedRows.fetch_add(1)>=QueryLimiter::getMaxScanRows())throw std::runtime_error("vector_scan_limit_exceeded");
        const auto fetchStart=std::chrono::steady_clock::now();
        if(candidate.record.empty()){
            ++authoritativeFetches;
            const auto fetched=LSM::findStrongSnapshotBound(user,db,collection,candidate.id,0);
            if(!fetched.value("found",false)||!fetched.contains("documents")||fetched.at("documents").size()!=1)
                throw std::runtime_error("vector_index_candidate_visibility_changed");
            if(fetched.value("visibility_source",std::string())=="CACHE")++cacheHits;
            candidate.record=pacificdb::packNativeVectorRecord(fetched.at("documents").front());
        }else {++cacheHits;++generationHits;}
        if(candidate.record.size()>QueryLimiter::getMaxResultBytes()||candidateBytes>QueryLimiter::getMaxResultBytes()-candidate.record.size())
            throw std::runtime_error("vector_candidate_bytes_exceeded");
        candidateBytes+=candidate.record.size();
        const auto raw=pacificdb::unpackNativeVectorRecord(candidate.record);
        const auto fetchEnd=std::chrono::steady_clock::now();resolutionTime+=fetchEnd-fetchStart;
        const pacificdb::storage_v2::MsgpackRowView row(reinterpret_cast<const uint8_t*>(raw.data()),raw.size(),true);
        const auto visibility=row.project(visibilityFields);
        if(visibility.value("id",std::string())!=candidate.id||!nativeVectorRowVisible(user,visibility))
            throw std::runtime_error("vector_index_candidate_visibility_changed");
        std::optional<double> score;
        if(loaded.spec.at("vector_name").get<std::string>().empty())score=row.densePreciseScore("vector",value.values,metric);
        if(!score){const auto fields=row.project({"vector","vectors"});const auto selected=selectedVector(fields,loaded.spec);
            if(!selected)throw std::runtime_error("vector_index_candidate_visibility_changed");
            score=pacificdb::vectors::score(value,pacificdb::vectors::decode(*selected),metric);
        }
        if(!std::isfinite(*score))throw std::runtime_error("vector_index_candidate_score_invalid");
        scored.push_back({*score,index});scoringTime+=std::chrono::steady_clock::now()-fetchEnd;
    }
    std::sort(scored.begin(),scored.end(),[&](const auto& a,const auto& b){return a.score>b.score||(a.score==b.score&&candidates[a.candidate].id<candidates[b.candidate].id);});
    for(size_t i=0;i<std::min(k,scored.size());++i){check();const auto& candidate=candidates[scored[i].candidate];
        top.consider(scored[i].score,candidate.id,[&]{++materializations;
            const auto raw=pacificdb::unpackNativeVectorRecord(candidate.record);
            const auto document=json::from_msgpack(raw.begin(),raw.end());
            if(!nativeVectorRowVisible(user,document))throw std::runtime_error("vector_index_candidate_visibility_changed");
            return document;});
    }
    const auto validationStart=std::chrono::steady_clock::now();
    pacificdb::test::hitFailpoint("FP_VECTOR_QUERY_BEFORE_FINAL_VALIDATION",1);
    const auto finalStamp = certificate(user, db, collection);
    if (!finalStamp || !generation->accepts(*finalStamp) || finalStamp->visibility < loaded.stamp.visibility ||
        (generation->visibilityComplete && !manager->acquire(*finalStamp)))
        throw std::runtime_error("vector_index_freshness_changed");
    check();
    auto results = top.finish();
    if (results.size() != std::min(k, generation->graph->size()))
        throw std::runtime_error("vector_index_incomplete_candidates");
    QueryLimiter::checkResultSize(results);
    if (execution) *execution = {{"executed_mode", "ann"}, {"ef_search",efSearch}, {"index_state", "READY"},
        {"index_name", name}, {"generation", generation->number}, {"fallback", false},
        {"payload_fetches", candidates.size()}, {"payload_cache_hits", cacheHits},
        {"authoritative_fetches",authoritativeFetches},{"document_materializations",materializations},{"generation_record_hits",generationHits},
        {"source_revision", loaded.stamp.collection}, {"commit_index", loaded.stamp.visibility}};
    if(execution){
        const auto end=std::chrono::steady_clock::now();
        const auto ms=[](auto value){return std::chrono::duration<double,std::milli>(value).count();};
        (*execution)["timings_ms"]={{"definition",ms(graphStart-start)},{"graph",ms(graphEnd-graphStart)},
            {"record_resolution",ms(resolutionTime)},{"scoring",ms(scoringTime)},
            {"final_validation",ms(end-validationStart)},{"total",ms(end-start)}};
    }
    return results;
}

std::vector<json> DatabaseEngine::queryNativeExactIndex(const std::string& user, const std::string& db,
    const std::string& collection, const std::string& name, const json& query, json* execution, DatabaseEngine::VectorDeadline deadline) {
    const auto start = std::chrono::steady_clock::now();
    const auto queryDeadline = pacificdb::vector_query::bound(start,QueryLimiter::getMaxQueryTime(),deadline);
    const auto check = [&] {
        if (QueryCancel::isCancelled()) throw std::runtime_error("vector_query_cancelled");
        if (std::chrono::steady_clock::now() > queryDeadline)
            throw std::runtime_error("vector_query_timeout");
    };
    check();
    if (!nativeVectorReadContextEligible()) throw std::runtime_error("vector_index_read_context_requires_scan");
    if ((query.contains("filter") && !query.at("filter").empty()) ||
        !query.value("modality",std::string()).empty())
        throw std::runtime_error("vector_index_filter_requires_scan");
    const auto loaded = definition(user,db,collection,name);
    const auto value = pacificdb::vectors::decode(query.at("vector"));
    if (query.contains("vector_name")) pacificdb::vectors::validateName(query.at("vector_name").get<std::string>());
    if (value.sparse) throw std::runtime_error("vector_index_sparse_requires_scan");
    auto metric = query.value("metric",std::string("cosine"));
    if (metric == "euclidean") metric = "l2";
    if (metric == "dot_product") metric = "dot";
    if (const auto schema=pacificdb::VectorSchemaRegistry::get(getDataRoot(),user,db,collection))
        pacificdb::VectorSchema::query(*schema,query.value("vector_name",std::string()),value,metric);
    if (metric != loaded.spec.at("metric") || value.dimensions != loaded.spec.at("dimensions") ||
        query.value("vector_name",std::string()) != loaded.spec.at("vector_name"))
        throw std::invalid_argument("flat index space/dimension/metric mismatch");
    const auto requested = query.value("k",json(10));
    if (!requested.is_number_integer() || (!requested.is_number_unsigned() && requested.get<int64_t>() <= 0) ||
        requested.get<uint64_t>() > 10000 || requested.get<uint64_t>() > QueryLimiter::getMaxResultDocs())
        throw std::invalid_argument("flat index k exceeds query limits");
    const auto manager = service()->get(key(user,db,collection,name));
    const auto generation = manager ? manager->acquire(loaded.stamp) : nullptr;
    if (!generation || !generation->flat) throw std::runtime_error("vector_index_unavailable: exact index not READY/fresh");
    auto state = QueryLimiter::capture(); state->scannedRows.store(0);
    // This plan visits every packed entry. Reject an insufficient scan budget
    // before computing any prefix; cancellation/time polling then batches at
    // most 4096 numeric components (one row for larger dimensions).
    if (generation->flat->size() > QueryLimiter::getMaxScanRows())
        throw std::runtime_error("vector_scan_limit_exceeded");
    const auto hits = generation->flat->search(value.values,requested.get<size_t>(),[&](size_t scanned) {
        check();
        if (scanned > QueryLimiter::getMaxScanRows()) throw std::runtime_error("vector_scan_limit_exceeded");
        state->scannedRows.store(scanned,std::memory_order_relaxed);
    }, std::max<size_t>(1,4096 / value.dimensions));
    std::vector<json> result; result.reserve(hits.size()); size_t bytes = 0, cacheHits = 0;
    for (const auto& hit : hits) {
        check();
        const auto fetched = LSM::findStrongSnapshotBound(user,db,collection,hit.first,0);
        if (!fetched.value("found",false) || !fetched.contains("documents") || fetched.at("documents").size() != 1)
            throw std::runtime_error("flat_index_candidate_visibility_changed");
        if (fetched.value("visibility_source", std::string()) == "CACHE") ++cacheHits;
        auto document = fetched.at("documents").front();
        if (!nativeVectorRowVisible(user,document)) throw std::runtime_error("flat_index_candidate_visibility_changed");
        const auto selected = selectedVector(document,loaded.spec);
        if (!selected || !selected->is_array() ||
            !generation->flat->matches(hit.first,pacificdb::vectors::decode(*selected).values))
            throw std::runtime_error("flat_index_candidate_visibility_changed");
        document["score"] = hit.second;
        const auto size = document.dump().size();
        if (size > QueryLimiter::getMaxResultBytes() || bytes > QueryLimiter::getMaxResultBytes() - size)
            throw std::runtime_error("vector_candidate_bytes_exceeded");
        bytes += size; result.push_back(std::move(document));
    }
    const auto finalStamp = certificate(user,db,collection);
    if (!finalStamp || *finalStamp != loaded.stamp) throw std::runtime_error("flat_index_freshness_changed");
    check(); QueryLimiter::checkResultSize(result);
    if (execution) *execution = {{"executed_mode","exact"},{"index_type","flat"},{"index_name",name},
        {"index_state","READY"},{"generation",generation->number},{"precision","float64"},
        {"fallback",false},{"payload_fetches",hits.size()},{"payload_cache_hits",cacheHits},{"source_revision",loaded.stamp.collection},
        {"commit_index",loaded.stamp.visibility}};
    return result;
}

std::vector<json> DatabaseEngine::queryNativeHybridIndex(const std::string& user, const std::string& db,
    const std::string& collection, const std::string& name, const json& query, json* execution, DatabaseEngine::VectorDeadline deadline) {
    const auto start = std::chrono::steady_clock::now();
    const auto queryDeadline = pacificdb::vector_query::bound(start,QueryLimiter::getMaxQueryTime(),deadline);
    const auto check = [&] {
        if (QueryCancel::isCancelled()) throw std::runtime_error("vector_query_cancelled");
        if (std::chrono::steady_clock::now() > queryDeadline)
            throw std::runtime_error("vector_query_timeout");
    };
    check();
    if (query.contains("offset") || query.contains("cursor"))
        throw std::invalid_argument("hybrid pagination is not supported");
    const auto loaded = definition(user, db, collection, name);
    const auto manager = service()->get(key(user, db, collection, name));
    const auto generation = manager ? manager->acquire(loaded.stamp) : nullptr;
    if (!generation || !generation->lexical)
        throw std::runtime_error("hybrid_index_unavailable: fresh vector/lexical generation required");
    const auto text = query.at("text").get<std::string>();
    pacificdb::LexicalIndex::tokenize(text, 4096, 64);
    const auto countParameter = [&](const char* field, size_t fallback, size_t maximum) {
        const auto input = query.value(field, json(fallback));
        if (!input.is_number_integer() || (!input.is_number_unsigned() && input.get<int64_t>() <= 0) ||
            input.get<uint64_t>() > maximum)
            throw std::invalid_argument(std::string("hybrid query limit invalid: ")+field);
        return input.get<size_t>();
    };
    const size_t maximum = std::min<size_t>(1000, QueryLimiter::getMaxResultDocs());
    const auto k = countParameter("k", 10, maximum);
    const auto candidates = countParameter("candidates", std::max<size_t>(k, std::min<size_t>(100, maximum)), maximum);
    if (candidates < k) throw std::invalid_argument("hybrid candidates must be at least k");
    const auto vectorWeight = query.value("vector_weight", 1.);
    const auto lexicalWeight = query.value("lexical_weight", 1.);
    const auto constant = query.value("rank_constant", 60.);
    pacificdb::reciprocalRankFusion({}, {}, k, vectorWeight, lexicalWeight, constant);
    const auto value = pacificdb::vectors::decode(query.at("vector"));
    if (query.contains("vector_name")) pacificdb::vectors::validateName(query.at("vector_name").get<std::string>());
    auto metric = query.value("metric", loaded.spec.at("metric").get<std::string>());
    if (metric == "euclidean") metric = "l2";
    if (metric == "dot_product") metric = "dot";
    if (const auto schema=pacificdb::VectorSchemaRegistry::get(getDataRoot(),user,db,collection))
        pacificdb::VectorSchema::query(*schema,query.value("vector_name",std::string()),value,metric);
    if (metric != loaded.spec.at("metric") || value.dimensions != loaded.spec.at("dimensions") ||
        query.value("vector_name", std::string()) != loaded.spec.at("vector_name"))
        throw std::invalid_argument("hybrid index space/dimension/metric mismatch");
    const auto filter = query.value("filter", json::object());
    const auto filterQuery = parseQuery(filter);
    const auto modality = query.value("modality", std::string());
    const bool filtering = !filterQuery.isMatchAll() || !modality.empty() || !nativeVectorReadContextEligible();
    const auto state = QueryLimiter::capture(); state->scannedRows.store(0);
    std::vector<std::pair<std::string,double>> vectorRanks, lexicalRanks;
    std::string vectorMode = "disabled";
    if (vectorWeight > 0) {
        auto vectorQuery = query; vectorQuery["k"] = candidates; vectorQuery["metric"] = metric;
        std::vector<json> hits;
        if (filtering || value.sparse) {
            vectorQuery["search_mode"] = "exact";
            hits = queryVector(user, db, collection, vectorQuery, nullptr, queryDeadline); vectorMode = "exact";
        } else if (generation->flat) {
            hits = queryNativeExactIndex(user, db, collection, name, vectorQuery, nullptr, queryDeadline); vectorMode = "exact_flat";
        } else {
            hits = queryNativeVectorIndex(user, db, collection, name, vectorQuery, nullptr, queryDeadline); vectorMode = "ann";
        }
        for (const auto& hit : hits) vectorRanks.emplace_back(hit.at("id").get<std::string>(), hit.at("score").get<double>());
    }
    const auto fetch = [&](const std::string& id) {
        check();
        if (state->scannedRows.fetch_add(1) >= QueryLimiter::getMaxScanRows())
            throw std::runtime_error("vector_scan_limit_exceeded");
        const auto fetched = LSM::findStrongSnapshotBound(user, db, collection, id, 0);
        if (!fetched.value("found", false) || fetched.at("documents").size() != 1)
            throw std::runtime_error("hybrid_candidate_visibility_changed");
        return fetched.at("documents").front();
    };
    const auto eligible = [&](const json& document) {
        if (!nativeVectorRowVisible(user, document) || !evalQuery(filterQuery, document)) return false;
        return modality.empty() || (document.contains("modality") && document.at("modality").is_string() &&
                                    document.at("modality") == modality);
    };
    if (lexicalWeight > 0) {
        std::function<bool(const std::string&)> accept;
        if (filtering) accept = [&](const std::string& id) { return eligible(fetch(id)); };
        lexicalRanks = generation->lexical->search(text, candidates, query.value("require_all_terms", false), check, accept);
    }
    const auto fused = pacificdb::reciprocalRankFusion(vectorRanks, lexicalRanks, k,
        vectorWeight, lexicalWeight, constant);
    std::vector<json> result; result.reserve(fused.size());
    size_t bytes = 0;
    for (const auto& hit : fused) {
        auto document = fetch(hit.first);
        if (!eligible(document)) throw std::runtime_error("hybrid_candidate_visibility_changed");
        document["score"] = hit.second;
        const auto size = document.dump().size();
        if (size > QueryLimiter::getMaxResultBytes() || bytes > QueryLimiter::getMaxResultBytes()-size)
            throw std::runtime_error("vector_candidate_bytes_exceeded");
        bytes += size; result.push_back(std::move(document));
    }
    const auto finalStamp = certificate(user, db, collection);
    if (!finalStamp || *finalStamp != loaded.stamp) throw std::runtime_error("hybrid_index_freshness_changed");
    check();
    if (execution) *execution = {{"executed_mode", "hybrid"}, {"fusion", "rrf"},
        {"lexical_scoring", "bm25"}, {"vector_mode", vectorMode}, {"generation", generation->number},
        {"candidates_per_ranker", candidates}, {"index_state", "READY"}};
    return result;
}
