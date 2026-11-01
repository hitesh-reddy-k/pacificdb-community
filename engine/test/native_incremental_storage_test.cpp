#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include <cassert>
#include <thread>
#include <chrono>
#include <iostream>
void ready(const char* collection="vectors",const char* name="ann"){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
 while(true){auto status=DatabaseEngine::nativeVectorIndexStatus("alice","app",collection,name);
  if(status.at("state")=="READY")return;if(status.at("state")=="FAILED")throw std::runtime_error(status.dump());
  assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
int main(){
#ifndef _WIN32
 setenv("PACIFICDB_VECTOR_PREVIEW_ENABLED","1",1);
#else
 _putenv_s("PACIFICDB_VECTOR_PREVIEW_ENABLED","1");
#endif
 OwnedTestRoot root("native-incremental-storage");DatabaseEngine::init(root.dataRoot().string());
 struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();}}stop;
 DatabaseEngine::ensureUserRoot("alice");DatabaseEngine::createDatabase("alice","app","vector");
 DatabaseEngine::createCollection("alice","app","vectors",{{"default",{{"dimensions",2},{"metric","dot"},{"required",true}}}});
 std::vector<json> records;for(int i=0;i<200;++i)records.push_back({{"id",std::to_string(i)},{"vector",{i,1}},{"metadata",0}});
 DatabaseEngine::insertMany("alice","app","vectors",records);QueryLimiter::setMaxScanRows(10000);
 const json options={{"name","ann"},{"dimensions",2},{"metric","dot"},{"maintenance","incremental"},
  {"max_elements",2000},{"ef_search",300},{"max_source_bytes",1024*1024},{"max_index_bytes",16*1024*1024}};
 DatabaseEngine::buildNativeVectorIndex("alice","app","vectors",options);ready();
 const auto request=json{{"vector",{1.,0.}},{"metric","dot"},{"k",10},{"search_mode","ann"}};
 auto compare=[&]{ready();const auto expected=DatabaseEngine::queryVector("alice","app","vectors",{{"vector",{1.,0.}},{"metric","dot"},{"k",10},{"search_mode","exact"}});
  const auto actual=DatabaseEngine::queryNativeVectorIndex("alice","app","vectors","ann",request);
  assert(actual.size()==expected.size());for(size_t i=0;i<actual.size();++i){assert(actual[i].at("id")==expected[i].at("id"));assert(actual[i].at("score")==expected[i].at("score"));}};
 compare();assert(DatabaseEngine::updateOne("alice","app","vectors",{{"id","0"}},{{"$set",{{"vector",{9999.,0.}}}}}));compare();
 DatabaseEngine::insertVector("alice","app","vectors",{{"id","new"},{"vector",{10000.,0.}}});compare();
 assert(DatabaseEngine::deleteOne("alice","app","vectors",{{"id","new"}}));compare();
 // Metadata edits advance the certificate but must not accumulate duplicate graph nodes.
 const auto physical=DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann").at("graph").at("physicalNodes");
 for(int i=0;i<50;++i)assert(DatabaseEngine::updateOne("alice","app","vectors",{{"id","0"}},{{"$set",{{"metadata",i}}}}));
 compare();assert(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann").at("graph").at("physicalNodes")==physical);
 records.clear();for(int i=200;i<500;++i)records.push_back({{"id",std::to_string(i)},{"vector",{i+20000,1}}});
 DatabaseEngine::insertMany("alice","app","vectors",records);compare();
 // IDs beyond the declared ANN limit produce an explicit history gap.
 const std::string oversizedId(1025,'x');
 LSM::put("alice","app","vectors",{{"id",oversizedId},{"vector",{1.,0.}}});
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann").at("state")!="FAILED"){
  assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
 bool rejected=false;try{DatabaseEngine::queryNativeVectorIndex("alice","app","vectors","ann",request);}catch(...){rejected=true;}assert(rejected);
 LSM::del("alice","app","vectors",oversizedId);DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","ann");compare();
 LSM::flush("alice","app","vectors");auto snapshot=LSM::exportSnapshotJson(1);
 assert(DatabaseEngine::deleteOne("alice","app","vectors",{{"id","499"}}));compare();
 assert(LSM::applySnapshot(snapshot));assert(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann").at("state")!="READY");
 DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","ann");compare();
 DatabaseEngine::init(root.dataRoot().string());DatabaseEngine::rebuildNativeVectorIndex("alice","app","vectors","ann");compare();
 assert(DatabaseEngine::updateOne("alice","app","vectors",{{"id","0"}},{{"$set",{{"vector",{99999.,0.}}}}}));compare();
 // Pending source records cannot silently disappear from an initial eligible graph.
 DatabaseEngine::dropNativeVectorIndex("alice","app","vectors","ann");
 LSM::put("alice","app","vectors",{{"id","pending"},{"vector",{999999.,0.}},{"_visibility_state","PENDING"},{"committed",false}});
 DatabaseEngine::buildNativeVectorIndex("alice","app","vectors",options);
 const auto pendingDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann").at("state")!="FAILED"){
  assert(std::chrono::steady_clock::now()<pendingDeadline);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
 assert(DatabaseEngine::nativeVectorIndexStatus("alice","app","vectors","ann").at("error").get<std::string>().find("pending_source")!=std::string::npos);
 std::cout<<"NATIVE_INCREMENTAL_STORAGE_CRUD_BATCH_HISTORY_GAPS_SNAPSHOT_RESTART_PENDING_PASS\n";
}
