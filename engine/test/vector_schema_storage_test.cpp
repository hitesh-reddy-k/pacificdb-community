#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include "vector_schema.hpp"
#include "vector_schema_lease.hpp"
#include <cassert>
#include <fstream>
#include <cstdint>
#include <iostream>
#include <thread>
#include <atomic>
using namespace pacificdb;
template<class F>void rejectsAt(int line,F fn){bool caught=false;std::string observed;try{fn();}catch(const std::exception& e){observed=e.what();caught=observed.find("vector_schema")!=std::string::npos;}if(!caught)std::cerr<<"schema rejection failed at line "<<line<<" observed: "<<observed<<std::endl;assert(caught);}
#define rejects(...) rejectsAt(__LINE__,__VA_ARGS__)
int main(){
    std::cout.setf(std::ios::unitbuf);
#ifdef _WIN32
    _putenv_s("PACIFICDB_VECTOR_PREVIEW_ENABLED","1");
#else
    setenv("PACIFICDB_VECTOR_PREVIEW_ENABLED","1",1);
#endif
    const json schema={{"default",{{"dimensions",2},{"metric","dot"},{"required",true}}},
        {"named",{{"terms",{{"kind","sparse"},{"dimensions",1000000},{"metric","dot"},{"required",true}}},
                  {"image",{{"dimensions",3},{"metric","cosine"}}}}}};
    const auto canonical=VectorSchema::canonical(schema);assert(VectorSchema::canonical(canonical)==canonical);
    const json sparse={{"format","sparse-v1"},{"precision","float64"},{"dimensions",1000000},
        {"indices",{3,999999}},{"values",{.125,-2.}}};
    const json valid={{"id","valid"},{"vector",{1.,0.}},{"vectors",{{"terms",sparse}}},{"payload","document"}};
    VectorSchema::record(canonical,valid);
    for(const auto& mutation:std::vector<json>{json{{"vector",{1.}}},json{{"vector",nullptr}},json{{"vectors",json::array()}},
            json{{"vectors",{{"terms",{1.,0.}}}}},json{{"vectors",{{"terms",sparse},{"unknown",{1.,2.}}}}}}){
        auto candidate=valid;candidate.update(mutation);rejects([&]{VectorSchema::record(canonical,candidate);});}
    auto missing=valid;missing.erase("vector");rejects([&]{VectorSchema::record(canonical,missing);});
    missing=valid;missing.erase("vectors");rejects([&]{VectorSchema::record(canonical,missing);});
    for(const auto& config:std::vector<json>{json::object(),json{{"default",{{"dimensions",0}}}},json{{"default",{{"dimensions",2.5}}}},
        json{{"default",{{"dimensions",2},{"metric","bad"}}}},json{{"format_version",1.5},{"default",{{"dimensions",2}}}},
        json{{"default",{{"dimensions",2},{"required",1}}}},json{{"default",{{"dimensions",2},{"precision","float32"}}}}})
        rejects([&]{VectorSchema::canonical(config);});
    const auto optional=VectorSchema::canonical({{"named",{{"embedding",{{"dimensions",2}}}}}});
    VectorSchema::record(optional,{{"id","plain"}});
    rejects([&]{VectorSchema::query(canonical,"",vectors::decode(json{1.,0.}),"cosine");});
    VectorSchema::query(canonical,"terms",vectors::decode(sparse),"dot_product");
    rejects([&]{VectorSchema::index(canonical,{{"dimensions",2},{"metric","cosine"}});});

    OwnedTestRoot root("vector-schema-storage");DatabaseEngine::init(root.dataRoot().string());
    struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();}}stop;
    DatabaseEngine::ensureUserRoot("alice");DatabaseEngine::ensureUserRoot("bob");
    DatabaseEngine::createDatabase("alice","app","vector");DatabaseEngine::createDatabase("bob","app","document");
    const auto id=DatabaseEngine::createCollection("alice","app","configured",schema);assert(!id.empty());
    DatabaseEngine::createCollection("bob","app","configured");DatabaseEngine::createCollection("alice","app","legacy");
    assert(VectorSchemaRegistry::get(root.dataRoot().string()+"/","alice","app","configured"));
    DatabaseEngine::insert("alice","app","configured",valid);assert(DatabaseEngine::count("alice","app","configured")==1);
    DatabaseEngine::insert("bob","app","configured",{{"id","plain"},{"vector","legacy unvalidated document"}});
    DatabaseEngine::insert("alice","app","legacy",{{"id","plain"},{"vector","legacy document"}});
    rejects([&]{DatabaseEngine::insert("alice","app","configured",{{"id","bad"},{"vector",{1.}}});});
    rejects([&]{LSM::put("alice","app","configured",{{"id","bad-low-level"},{"vector",{1.}}});});
    auto other=valid;other["id"]="batch-good";
    rejects([&]{DatabaseEngine::insertMany("alice","app","configured",{other,{{"id","bad"},{"vector",{1.}}}});});
    rejects([&]{LSM::putMany("alice","app","configured",{other,{{"id","bad"},{"vector",{1.}}}});});
    assert(DatabaseEngine::count("alice","app","configured")==1);
    assert(DatabaseEngine::updateOne("alice","app","configured",{{"id","valid"}},{{"$set",{{"payload","changed"}}}}));
    rejects([&]{DatabaseEngine::updateOne("alice","app","configured",{{"id","valid"}},{{"$set",{{"vector",{1.}}}}});});
    rejects([&]{DatabaseEngine::updateOne("alice","app","configured",{{"id","valid"}},{{"$unset",{"vectors"}}});});
    assert(DatabaseEngine::queryVector("alice","app","configured",{{"vector",{1.,0.}},{"metric","dot"},{"k",1}}).front().at("id")=="valid");
    rejects([&]{DatabaseEngine::queryVector("alice","app","configured",{{"vector",{1.,0.}},{"metric","cosine"},{"k",1}});});
    rejects([&]{DatabaseEngine::buildNativeVectorIndex("alice","app","configured",{{"name","wrong"},{"dimensions",2},{"metric","cosine"}});});
    auto changed=schema;changed["default"]["dimensions"]=3;
    rejects([&]{DatabaseEngine::createCollection("alice","app","configured",changed);});

    // Later ordinary DDL and ACL edits in the same database must preserve the
    // durable schema instead of falling back to truncating db.meta in place.
    DatabaseEngine::createCollection("alice","app","ordinary-after-schema");
    assert(DatabaseEngine::setDatabaseSecurity("alice","app",{{"version",1},{"owners",json::array({"alice"})},{"grants",json::object()}}));
    assert(DatabaseEngine::dropCollection("alice","app","ordinary-after-schema"));
    // Registry reload from durable db.meta precedes both collection WAL recovery
    // and snapshot activation; no schema memory is the authoritative source.
    DatabaseEngine::init(root.dataRoot().string());LSM::restoreFromWal();
    assert(DatabaseEngine::count("alice","app","configured")==1);
    rejects([&]{DatabaseEngine::insert("alice","app","configured",{{"id","after-restart"}});});
    // Explicit trusted tombstone intent must retain DELETE WAL semantics.
    auto doomed=valid;doomed["id"]="trusted-delete";DatabaseEngine::insert("alice","app","configured",doomed);
    LSM::put("alice","app","configured",{{"id","trusted-delete"},{"_deleted",true},{"version",INT64_MAX}},false,0,true,LSM::PutIntent::Tombstone);
    DatabaseEngine::init(root.dataRoot().string());LSM::restoreFromWal();
    assert(DatabaseEngine::count("alice","app","configured")==1);
    LSM::flush("alice","app","configured");const auto snapshot=LSM::exportSnapshotJson(1);
    assert(DatabaseEngine::deleteOne("alice","app","configured",{{"id","valid"}}));
    assert(LSM::applySnapshot(snapshot));assert(DatabaseEngine::count("alice","app","configured")==1);
    rejects([&]{LSM::put("alice","app","configured",{{"id","after-restore"}});});
    QueryLimiter::setMaxScanRows(10000);
    std::atomic<int> failures{0};std::vector<std::thread> workers;
    for(int t=0;t<4;++t)workers.emplace_back([&,t]{for(int i=0;i<25;++i){
        auto document=valid;document["id"]=std::to_string(t)+"-"+std::to_string(i);
        DatabaseEngine::insert("alice","app","configured",document);
        try{DatabaseEngine::insert("alice","app","configured",{{"id","rejected"}});++failures;}catch(const std::exception&){}
    }});
    for(auto& worker:workers)worker.join();assert(failures==0);assert(DatabaseEngine::count("alice","app","configured")==101);
    // DDL and post-image validation have one per-database ordering, including
    // direct C++ callers. Readers share the lease; unrelated databases proceed.
    std::atomic<bool> holding{false},release{false},entered{false},ddlWaiting{false};
    std::thread heldMutation([&]{VectorSchemaLease lease("alice","app",false);holding=true;while(!release.load())std::this_thread::yield();});
    while(!holding.load())std::this_thread::yield();
    std::thread ddl([&]{ddlWaiting=true;VectorSchemaLease lease("alice","app",true);entered=true;});
    while(!ddlWaiting.load())std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(!entered.load());
    DatabaseEngine::insert("bob","app","configured",{{"id","unrelated-db-progress"}});
    release=true;heldMutation.join();ddl.join();assert(entered.load());
    DatabaseEngine::createCollection("alice","app","racing",{{"default",{{"dimensions",2},{"metric","dot"},{"required",true}}}});
    std::atomic<bool> racingDone{false};std::atomic<int> successful{0};
    std::thread writes([&]{int i=0;while(!racingDone.load()){
        try{DatabaseEngine::insert("alice","app","racing",{{"id",std::to_string(i++)},{"vector",{1.,0.}}});++successful;}
        catch(const std::exception& error){assert(std::string(error.what()).find("vector_schema")!=std::string::npos);}
    }});
    for(int n=0;n<12;++n){
        VectorSchemaLease transition("alice","app",true);
        assert(DatabaseEngine::dropCollection("alice","app","racing"));
        DatabaseEngine::createCollection("alice","app","racing",{{"default",{{"dimensions",n%2?2:3},{"metric","dot"},{"required",true}}}});
    }
    racingDone=true;writes.join();
    assert(DatabaseEngine::dropCollection("alice","app","configured"));
    DatabaseEngine::createCollection("alice","app","configured");DatabaseEngine::insert("alice","app","configured",{{"id","new-incarnation"}});
    assert(!VectorSchemaRegistry::get(root.dataRoot().string(),"alice","app","configured"));
    OwnedTestRoot otherRoot("vector-schema-other-root");DatabaseEngine::init(otherRoot.dataRoot().string());
    assert(!VectorSchemaRegistry::get(otherRoot.dataRoot().string(),"alice","app","configured"));
    std::cout<<"VECTOR_SCHEMA_VALIDATION_CRUD_WAL_SNAPSHOT_TENANT_CONCURRENCY_PASS\n";
}
