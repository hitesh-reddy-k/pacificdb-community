#include "database_engine.hpp"
#include "community_catalog.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>

int main() {
    OwnedTestRoot root("native-vector-mixed");DatabaseEngine::init(root.dataRoot().string());
    LSM::startBackgroundTasks();
    struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();LSM::stopBackgroundTasks();}} stop;
    DatabaseEngine::ensureUserRoot("alice");DatabaseEngine::createDatabase("alice","app","binary");
    for(const std::string name:{"docs","vectors","assets"})DatabaseEngine::createCollection("alice","app",name);
    DatabaseEngine::insert("alice","app","docs",{{"id","seed"},{"counter",-1}});
    assert(LSM::createSecondaryIndex("alice","app","docs",{{"name","by_counter"},{"type","btree"},
        {"field","counter"},{"order",1},{"unique",false}}).value("status",std::string())!="error");
    auto& media=pacificdb::community::CommunityCatalog::instance();media.initialize("alice");
    const auto upload=[&](const std::string& filename){
        const std::string digest="63c1dd951ffedf6f7fd968ad4efa39b8ed584f162f46e715114ee184f8de9201";
        const auto started=media.beginMedia("alice","app","assets",filename,"application/octet-stream",4,1,digest);
        const auto id=started.at("id").get<std::string>();
        media.putMediaChunk("alice",id,0,"QUFBQQ==",4,digest);
        assert(media.finalizeMedia("alice",id).at("status")=="ready");
        assert(!media.getMediaChunk("alice",id,0).is_null());
        assert(media.deleteMedia("alice",id));assert(media.getMedia("alice",id).is_null());
    };
    upload("warmup.bin");
    std::mt19937 random(1701);std::uniform_real_distribution<double> value(-1,1);
    std::vector<json> vectors;
    for(int i=0;i<3000;++i){std::vector<double> vector(8);for(auto& number:vector)number=value(random);
        vectors.push_back({{"id",std::to_string(i)},{"vector",vector}});}
    DatabaseEngine::insertMany("alice","app","vectors",std::move(vectors));
    LSM::flush("alice","app","vectors");
    DatabaseEngine::buildNativeVectorIndex("alice","app","vectors",{{"name","native"},{"dimensions",8},
        {"metric","cosine"},{"ef_search",200}});
    std::thread documents([&]{
        QueryLimiter::Scope request;
        for(int i=0;i<50;++i){
            const auto id="doc-"+std::to_string(i);
            DatabaseEngine::insert("alice","app","docs",{{"id",id},{"counter",i}});
            assert(DatabaseEngine::updateOne("alice","app","docs",{{"id",id}},{{"$set",{{"counter",i+1000}}}}));
            const auto rows=DatabaseEngine::find("alice","app","docs",{{"id",id}});
            assert(rows.size()==1&&rows.front().at("counter")==i+1000);
            if(i%2==0)assert(DatabaseEngine::deleteOne("alice","app","docs",{{"id",id}}));
        }
    });
    std::thread files([&]{QueryLimiter::Scope request;for(int i=0;i<5;++i)upload("concurrent-"+std::to_string(i)+".bin");});
    std::thread exact([&]{
        QueryLimiter::Scope request;
        for(int i=0;i<8;++i)assert(DatabaseEngine::queryVector("alice","app","vectors",
            {{"vector",{1.,0.,0.,0.,0.,0.,0.,0.}},{"metric","cosine"},{"k",10},{"search_mode","exact"}}).size()==10);
    });
    std::thread rebuilding([&]{
        for(int i=0;i<10;++i)assert(LSM::rebuildSecondaryIndex("alice","app","docs","by_counter").at("status")=="ok");
    });
    documents.join();files.join();exact.join();rebuilding.join();
    assert(LSM::waitForColumnIndexUpdates("alice","app","docs",5000));
    assert(DatabaseEngine::count("alice","app","docs")==26);
    assert(DatabaseEngine::find("alice","app","docs",{{"counter",{{"$gte",1000}}}}).size()==25);
    const auto validation=LSM::validateColumnIndexes("alice","app","docs");
    if(validation.at("status")!="ok")throw std::runtime_error(validation.dump());
    assert(validation.at("validationComplete")==true);
    // Deterministically replay an obsolete queued payload after a flushed
    // update, then after deletion. Neither may resurrect stale index values.
    const json oldPayload={{"id","delayed"},{"counter",-100}};
    DatabaseEngine::insert("alice","app","docs",oldPayload);
    assert(DatabaseEngine::updateOne("alice","app","docs",{{"id","delayed"}},
        {{"$set",{{"counter",2000}}}}));
    assert(LSM::waitForColumnIndexUpdates("alice","app","docs",5000));
    LSM::flush("alice","app","docs");
    LSM::updateColumnIndexesBatch("alice","app","docs",{oldPayload});
    assert(LSM::validateColumnIndexes("alice","app","docs").at("status")=="ok");
    assert(DatabaseEngine::deleteOne("alice","app","docs",{{"id","delayed"}}));
    LSM::updateColumnIndexesBatch("alice","app","docs",{oldPayload});
    assert(LSM::validateColumnIndexes("alice","app","docs").at("status")=="ok");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    for(;;){const auto status=DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","native");
        if(status.at("state")=="READY")break;
        if(status.at("state")=="STALE")DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","native");
        if(status.at("state")=="FAILED")throw std::runtime_error(status.dump());
        assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
    assert(DatabaseEngine::queryNativeVectorIndex("alice","app","vectors","native",
        {{"vector",{1.,0.,0.,0.,0.,0.,0.,0.}},{"metric","cosine"},{"k",10}}).size()==10);
    std::cout<<"NATIVE_VECTOR_MIXED_DOCUMENT_MEDIA_INDEX_PASS\n";
}
