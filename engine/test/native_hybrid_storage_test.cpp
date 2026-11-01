#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <cmath>
#include <limits>
#include <thread>

void ready() {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    for (;;) {
        const auto status=DatabaseEngine::nativeVectorIndexStatus("alice","app","docs","hybrid");
        if(status.at("state")=="READY") { assert(status.at("lexical").at("healthy")==true); return; }
        if(status.at("state")=="FAILED") throw std::runtime_error(status.dump());
        assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();
    }
}
std::vector<std::string> ids(const std::vector<json>& rows) {
    std::vector<std::string> result;
    for(const auto& row:rows){assert(row.at("tenant_id")=="alice");result.push_back(row.at("id").get<std::string>());}
    return result;
}
int main() {
    for (const std::string algorithm : {"hnsw", "flat"}) {
    OwnedTestRoot root("native-hybrid-storage");DatabaseEngine::init(root.dataRoot().string());
    struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();}} stop;
    for(const std::string user:{"alice","bob"}){
        DatabaseEngine::ensureUserRoot(user);DatabaseEngine::createDatabase(user,"app","binary");
        DatabaseEngine::createCollection(user,"app","docs");
    }
    DatabaseEngine::insertMany("alice","app","docs",{
        {{"id","a"},{"vector",{1.,0.}},{"title","alpha alpha common"},{"category","news"}},
        {{"id","b"},{"vector",{0.,1.}},{"title","beta common"},{"category","news"}},
        {{"id","c"},{"vector",{-1.,0.}},{"title","alpha beta common"},{"category","private"}},
        {{"id","d"},{"title","alpha alpha alpha"},{"category","news"}},
        {{"id","e"},{"vector",{1.,1.}},{"category","news"}}});
    DatabaseEngine::insert("bob","app","docs",{{"id","private"},{"title","alpha alpha alpha alpha"},{"vector",{100.,0.}}});
    DatabaseEngine::buildNativeVectorIndex("alice","app","docs",{{"name","hybrid"},{"algorithm",algorithm},{"dimensions",2},
        {"metric","dot"},{"ef_search",200},{"text_field","title"}});ready();
    json request{{"vector",{1.,0.}},{"text","alpha"},{"k",3},{"candidates",4}};
    const auto query=[&](const json& input){return DatabaseEngine::queryNativeHybridIndex("alice","app","docs","hybrid",input);};
    const auto baseline=query(request);
    assert(ids(baseline)==std::vector<std::string>({"a","c","d"}));
    assert(std::abs(baseline[0].at("score").get<double>()-(1./61+1./62))<1e-15);
    auto filtered=request;filtered["filter"]={{"category","news"}};
    assert(ids(query(filtered))==std::vector<std::string>({"a","d","e"}));
    auto lexical=request;lexical["vector_weight"]=0.;
    assert(ids(query(lexical))==std::vector<std::string>({"d","a","c"}));
    auto vector=request;vector["lexical_weight"]=0.;
    assert(ids(query(vector))==std::vector<std::string>({"a","e","b"}));
    auto all=lexical;all["text"]="alpha beta";all["require_all_terms"]=true;
    assert(ids(query(all))==std::vector<std::string>({"c"}));
    all["filter"]={{"category","news"}};assert(query(all).empty());
    DatabaseEngine::setReadContext("strong",0,std::numeric_limits<long long>::max());
    assert(query(request).empty());DatabaseEngine::clearReadContext();
    const auto snapshot=LSM::exportSnapshotJson(1);
    DatabaseEngine::updateOne("alice","app","docs",{{"id","d"}},{{"$set",{{"title","zeta"}}}});
    bool stale=false;try{query(request);}catch(const std::runtime_error& e){stale=std::string(e.what()).find("unavailable")!=std::string::npos;}
    assert(stale);
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","docs","hybrid");ready();
    assert(ids(query(lexical))==std::vector<std::string>({"a","c"}));
    assert(LSM::applySnapshot(snapshot));
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","docs","hybrid");ready();
    assert(query(request)==baseline);
    DatabaseEngine::init(root.dataRoot().string());
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","docs","hybrid");ready();
    assert(query(request)==baseline);
    QueryLimiter::setMaxScanRows(1);
    bool limited=false;try{query(filtered);}catch(const std::runtime_error& e){limited=std::string(e.what()).find("scan_limit")!=std::string::npos;}
    assert(limited);QueryLimiter::setMaxScanRows(1000000);
    assert(query(request)==baseline);
    assert(DatabaseEngine::dropNativeVectorIndex("alice","app","docs","hybrid"));
    assert(DatabaseEngine::count("alice","app","docs")==5);
    std::cout<<"NATIVE_HYBRID_STORAGE_PASS "<<algorithm<<"\n";
    }
}
