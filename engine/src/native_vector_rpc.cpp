#include "native_vector_rpc.hpp"
#include "database_engine.hpp"
#include "query_cancellation.hpp"
#include "query_limiter.hpp"
#include "storage_path.hpp"
#include "vector_value.hpp"
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <thread>

namespace pacificdb::native_vector_rpc {
static bool transientFreshness(const std::string& reason){
    return reason=="vector_index_source_changing"||reason=="vector_index_freshness_changed"||
        reason=="vector_index_candidate_visibility_changed"||
        reason=="vector_index_unavailable: not READY/fresh"||
        reason=="vector_index_unavailable: STALE (index not READY/fresh)";
}
bool enabled(){const char* value=std::getenv("PACIFICDB_VECTOR_PREVIEW_ENABLED");
    return value&&(std::string(value)=="1"||std::string(value)=="true");}
bool annEnabled(){
    const char* raw=std::getenv("VECTOR_HNSW_ENABLED");
    if(!raw)return true;
    std::string value(raw);std::transform(value.begin(),value.end(),value.begin(),
        [](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value!="0"&&value!="false"&&value!="off"&&value!="no";
}
vector_query::Deadline deadline(const json& query,vector_query::Deadline parent){
    const auto start=vector_query::Clock::now();
    auto result=vector_query::bound(start,QueryLimiter::getMaxQueryTime(),parent);
    if(query.contains("timeout_ms")){
        const auto& value=query.at("timeout_ms");
        if(!value.is_number_integer()||value<0||value>3600000)
            throw std::invalid_argument("timeout_ms must be an integer between 0 and 3600000");
        result=std::min(result,start+std::chrono::milliseconds(value.get<int64_t>()));
    }
    return result;
}
static std::vector<json> searchAttempt(const std::string& user,const std::string& database,
        const std::string& collection,const json& query,json* execution,vector_query::Deadline parent,
        bool& mayWait){
    const auto requestDeadline=deadline(query,parent);
    const auto check=[&]{
        if(QueryCancel::isCancelled())throw std::runtime_error("vector_query_cancelled");
        if(vector_query::Clock::now()>=*requestDeadline)throw std::runtime_error("vector_query_timeout");
    };
    check();
    const bool named=query.contains("index_name");
    if(query.contains("require_index")&&!query.at("require_index").is_boolean())
        throw std::invalid_argument("require_index must be a boolean");
    const bool required=query.value("require_index",false);
    if(!enabled()){
        if(named||required)throw std::runtime_error("vector_index_preview_disabled");
        return DatabaseEngine::queryVector(user,database,collection,query,execution,requestDeadline);
    }
    if(!named){
        if(required)throw std::invalid_argument("index_name required when require_index is true");
        return DatabaseEngine::queryVector(user,database,collection,query,execution,requestDeadline);
    }
    const auto name=query.at("index_name").get<std::string>();
    validateStorageIdentifier(name,"vectorIndexName");
    const auto mode=query.value("search_mode",std::string("auto"));
    if(mode!="exact"&&mode!="auto"&&mode!="ann")throw std::invalid_argument("search_mode must be auto, exact or ann");
    const bool allowExact=DatabaseEngine::vectorExactFallbackAllowed(query);
    const bool mayScan=!required&&(mode=="exact"||allowExact);
    const auto value=vectors::decode(query.at("vector"));
    if(query.contains("vector_name"))vectors::validateName(query.at("vector_name").get<std::string>());
    (void)value;
    std::string state="ABSENT",reason="no qualified index generation";
    const auto scan=[&]()->std::vector<json>{
        if(!mayScan)throw std::runtime_error("vector_index_unavailable: "+state+" ("+reason+")");
        check();auto exact=query;exact["search_mode"]="exact";exact.erase("index_name");exact.erase("require_index");
        auto results=DatabaseEngine::queryVector(user,database,collection,exact,execution,requestDeadline);
        if(execution){(*execution)["requested_mode"]=mode;(*execution)["index_name"]=name;
            (*execution)["index_state"]=state;(*execution)["fallback"]=true;(*execution)["fallback_reason"]=reason;}
        return results;
    };
    const auto preflightStart=vector_query::Clock::now();
    json status;
    // Definition capture can race a write before the maintenance policy is
    // available. Only a bounded read-only retry is allowed at this boundary.
    mayWait=mode!="exact";
    try{status=DatabaseEngine::nativeVectorIndexStatus(user,database,collection,name);}
    catch(const std::runtime_error& error){
        reason=error.what();if(reason.find("vector_index_unavailable")==std::string::npos)throw;
        return scan();
    }
    mayWait=false;
    state=status.at("state").get<std::string>();
    const bool flat=status.at("spec").value("algorithm",std::string("hnsw"))=="flat";
    const bool incremental=!flat&&status.at("spec").value("maintenance",std::string("manual"))=="incremental";
    const bool recoverable=state=="READY"||state=="STALE";
    mayWait=mode!="exact"&&incremental&&recoverable&&annEnabled()&&
        status.value("generation",uint64_t(0))>0&&!status.contains("error");
    if(state!="READY"){
        reason=status.value("error",std::string("index not READY/fresh"));return scan();
    }
    if(!flat&&!annEnabled()){reason="ANN disabled by VECTOR_HNSW_ENABLED";return scan();}
    if((mode=="exact"&&!flat)||(mode!="exact"&&flat&&!allowExact)){
        reason="index algorithm does not satisfy requested mode/fallback policy";return scan();
    }
    const auto preflightEnd=vector_query::Clock::now();
    try{
        auto results=flat
            ? DatabaseEngine::queryNativeExactIndex(user,database,collection,name,query,execution,requestDeadline)
            : DatabaseEngine::queryNativeVectorIndex(user,database,collection,name,query,execution,requestDeadline);
        if(execution){(*execution)["rpc_preflight_ms"]=std::chrono::duration<double,std::milli>(preflightEnd-preflightStart).count();
            (*execution)["requested_mode"]=mode;(*execution)["fallback"]=flat&&mode!="exact";
            if(flat&&mode!="exact")(*execution)["fallback_reason"]="requested ANN/auto used eligible exact flat index";}
        return results;
    }catch(const std::exception& error){
        reason=error.what();
        if(mayWait&&!mayScan&&transientFreshness(reason))throw;
        // Only pre-search eligibility failures permit a fallback. Runtime memory,
        // timeout/cancellation and post-search freshness errors remain explicit;
        // their work must not disappear from this request's resource budget.
        const bool ineligible=reason.find("vector_index_unavailable")!=std::string::npos||
            reason.find("requires_scan")!=std::string::npos||reason.find("requires_exact")!=std::string::npos||
            reason.find("vector_index_source_changing")!=std::string::npos||
            reason.find("space/dimension/metric mismatch")!=std::string::npos;
        if(!ineligible)throw;
        if (reason.find("vector_index_unavailable") != std::string::npos ||
            reason.find("vector_index_source_changing") != std::string::npos) state="STALE";
        return scan();
    }
}

std::vector<json> search(const std::string& user,const std::string& database,
        const std::string& collection,const json& query,json* execution,vector_query::Deadline parent){
    const auto start=vector_query::Clock::now();
    const auto requestDeadline=deadline(query,parent);
    const auto waitValue=query.value("index_wait_ms",json(1000));
    if(!waitValue.is_number_integer()||waitValue<0||waitValue>5000)
        throw std::invalid_argument("index_wait_ms must be an integer between 0 and 5000");
    const auto waitDeadline=std::min(*requestDeadline,start+std::chrono::milliseconds(waitValue.get<int>()));
    // One budget across all attempts, not a new budget for each retry. Failed
    // attempt candidates are destroyed before another attempt is admitted.
    QueryLimiter::capture()->scannedRows.store(0);
    size_t retries=0;
    for(;;){
        bool mayWait=false;
        if(QueryCancel::isCancelled())throw std::runtime_error("vector_query_cancelled");
        if(vector_query::Clock::now()>=*requestDeadline)throw std::runtime_error("vector_query_timeout");
        try{
            auto result=searchAttempt(user,database,collection,query,execution,requestDeadline,mayWait);
            if(execution)(*execution)["rpc_total_ms"]=std::chrono::duration<double,std::milli>(vector_query::Clock::now()-start).count();
            if(execution&&retries){
                (*execution)["freshness_retries"]=retries;
                (*execution)["freshness_elapsed_ms"]=std::chrono::duration<double,std::milli>(
                    vector_query::Clock::now()-start).count();
            }
            return result;
        }catch(const std::runtime_error& error){
            if(QueryCancel::isCancelled())throw std::runtime_error("vector_query_cancelled");
            if(vector_query::Clock::now()>=*requestDeadline)throw std::runtime_error("vector_query_timeout");
            const std::string reason=error.what();
            if(!mayWait||!transientFreshness(reason)||retries>=128||vector_query::Clock::now()>=waitDeadline)throw;
            ++retries;
            // Request-local, bounded backoff; no detached work, global locks or
            // mutation retries. Cancellation is checked at most 10ms later.
            const auto delay=std::chrono::milliseconds(std::min<size_t>(10,retries));
            std::this_thread::sleep_until(std::min(waitDeadline,vector_query::Clock::now()+delay));
        }
    }
}
}
