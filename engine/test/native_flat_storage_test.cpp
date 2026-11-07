#include "database_engine.hpp"
#include "lsm.hpp"
#include "memory_manager.hpp"
#include "owned_test_root.hpp"
#include "query_cancellation.hpp"
#include "query_limiter.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>

template<class F>void fails(F fn,const std::string& message){bool rejected=false;try{fn();}catch(const std::exception& e){rejected=std::string(e.what()).find(message)!=std::string::npos;}assert(rejected);}
void ready(const std::string& collection){
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    for(;;){auto status=DatabaseEngine::nativeVectorIndexStatus("alice","app",collection,"flat");
        if(status.at("state")=="READY"){assert(status.contains("flat")&&!status.contains("graph"));return;}
        if(status.at("state")=="FAILED")throw std::runtime_error(status.dump());
        assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
int main(){
    OwnedTestRoot root("native-flat-storage");DatabaseEngine::init(root.dataRoot().string());
    struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();}}stop;
    DatabaseEngine::ensureUserRoot("alice");DatabaseEngine::createDatabase("alice","app","vector");
    DatabaseEngine::createCollection("alice","app","vectors");
    std::mt19937 random(2026);std::uniform_real_distribution<double> value(-1,1);
    std::vector<json> documents;
    for(int i=0;i<1000;++i){std::vector<double> vector(8);for(auto& x:vector)x=value(random);
        documents.push_back({{"id",std::to_string(i)},{"vector",vector},{"group",i%2},{"title","shared term"}});}
    DatabaseEngine::insertMany("alice","app","vectors",std::move(documents));
    // Exercise defense in depth for misplaced tenant rows.
    LSM::put("alice","app","vectors",{{"id","foreign"},{"tenant_id","bob"},{"vector",{100.,100.,100.,100.,100.,100.,100.,100.}}});
    QueryLimiter::setMaxScanRows(10000);
    const json options={{"name","flat"},{"algorithm","flat"},{"dimensions",8},
        {"max_elements",2000},{"max_scan_rows",3000},{"text_field","title"},
        {"max_source_bytes",4*1024*1024},{"max_index_bytes",16*1024*1024}};
    for(const std::string metric:{"cosine","l2","dot"}){
        auto spec=options;spec["metric"]=metric;DatabaseEngine::buildNativeVectorIndex("alice","app","vectors",spec);ready("vectors");
        for(int q=0;q<10;++q){std::vector<double> query(8);for(auto& x:query)x=value(random);
            const json request={{"vector",query},{"metric",metric},{"k",10},{"search_mode","exact"}};
            auto expected=DatabaseEngine::queryVector("alice","app","vectors",request);
            json execution;auto actual=DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request,&execution);
            assert(execution.at("executed_mode")=="exact"&&execution.at("precision")=="float64");
            assert(actual.size()==expected.size());
            for(size_t k=0;k<actual.size();++k){assert(actual[k].at("id")==expected[k].at("id"));
                assert(actual[k].at("score")==expected[k].at("score"));assert(actual[k].at("tenant_id")=="alice");}
        }
    }
    const json request={{"vector",{1.,0.,0.,0.,0.,0.,0.,0.}},{"metric","dot"},{"k",10}};
    fails([&]{DatabaseEngine::queryNativeVectorIndex("alice","app","vectors","flat",request);},"exact-only");
    fails([&]{DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request,nullptr,
        std::chrono::steady_clock::now()-std::chrono::seconds(1));},"timeout");
    auto filtered=request;filtered["filter"]={{"group",1}};
    fails([&]{DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",filtered);},"requires_scan");
    DatabaseEngine::setReadContext("strong",0,10000);
    fails([&]{DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request);},"requires_scan");
    DatabaseEngine::clearReadContext();
    QueryLimiter::setMaxScanRows(100);
    fails([&]{DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request);},"scan_limit");
    QueryLimiter::setMaxScanRows(10000);
    {auto token=QueryCancel::makeToken();token->cancelled.store(true);QueryCancel::Scope scope(token);
        fails([&]{DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request);},"cancelled");}
    assert(DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request).size()==10);
    assert(DatabaseEngine::updateOne("alice","app","vectors",{{"id","0"}},{{"$set",{{"vector",{100.,0.,0.,0.,0.,0.,0.,0.}}}}}));
    assert(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","flat").at("state")=="STALE");
    fails([&]{DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request);},"unavailable");
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","flat");ready("vectors");
    assert(DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request).front().at("id")=="0");
    std::atomic<bool> begin{false};
    std::thread writer([&]{while(!begin.load())std::this_thread::yield();
        for(int i=0;i<20;++i)assert(DatabaseEngine::updateOne("alice","app","vectors",{{"id","0"}},
            {{"$set",{{"vector",{double(81+i),0.,0.,0.,0.,0.,0.,0.}}}}}));});
    std::vector<std::thread> readers;
    for(int worker=0;worker<4;++worker)readers.emplace_back([&]{QueryLimiter::Scope limits;begin.store(true);
        for(int i=0;i<30;++i){try{
            auto rows=DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request);
            assert(rows.size()==10);for(const auto& row:rows)assert(row.at("tenant_id")=="alice");
        }catch(const std::runtime_error& error){const std::string message=error.what();
            assert(message.find("unavailable")!=std::string::npos||message.find("freshness_changed")!=std::string::npos||
                message.find("visibility_changed")!=std::string::npos||message.find("source_changing")!=std::string::npos);}}});
    writer.join();for(auto& reader:readers)reader.join();
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","flat");ready("vectors");
    const auto snapshot=LSM::exportSnapshotJson(1);
    assert(DatabaseEngine::deleteOne("alice","app","vectors",{{"id","0"}}));
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","flat");ready("vectors");
    for(const auto& hit:DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request))assert(hit.at("id")!="0");
    assert(LSM::applySnapshot(snapshot));
    assert(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","flat").at("state")=="STALE");
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","flat");ready("vectors");
    assert(DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request).front().at("id")=="0");
    // Two definitions exceed neither the service's bounded source nor worker quota.
    DatabaseEngine::createCollection("alice","app","named");
    DatabaseEngine::insertVector("alice","app","named",{{"id","z"},{"vectors",{{"image",{1.+1e-14}},{"text",{0.,1.}}}}});
    DatabaseEngine::insertVector("alice","app","named",{{"id","a"},{"vectors",{{"image",{1.}},{"text",{1.,0.}}}}});
    DatabaseEngine::buildNativeVectorIndex("alice","app","named",{{"name","flat"},{"algorithm","flat"},{"dimensions",1},{"vector_name","image"},{"metric","dot"},{"max_source_bytes",1024*1024}});ready("named");
    assert(DatabaseEngine::queryNativeExactIndex("alice","app","named","flat",{{"vector",{1.}},{"vector_name","image"},{"metric","dot"},{"k",2}}).front().at("id")=="z");
    assert(DatabaseEngine::dropNativeVectorIndex("alice","app","named","flat"));assert(DatabaseEngine::count("alice","app","named")==2);
    LSM::flush("alice","app","vectors");DatabaseEngine::init(root.dataRoot().string());
    assert(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","flat").at("state")=="ABSENT");
    DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","flat");ready("vectors");
    assert(DatabaseEngine::queryNativeExactIndex("alice","app","vectors","flat",request).front().at("id")=="0");
    auto oversized=options;oversized["name"]="oversized";
    oversized["max_index_bytes"]=1024ULL*1024*1024;oversized["max_source_bytes"]=1024ULL*1024*1024;
    fails([&]{DatabaseEngine::buildNativeVectorIndex("alice","app","vectors",oversized);},"global_resource_budget");
    std::cout<<"NATIVE_FLAT_STORAGE_REFERENCE_FRESHNESS_RECOVERY_PASS\n";
}
