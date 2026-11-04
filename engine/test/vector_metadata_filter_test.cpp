#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include "query_cancellation.hpp"
#include "wal.hpp"
#include "test_failpoint.hpp"
#include <thread>
#include <fstream>
#include <cmath>
#ifndef _WIN32
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <iostream>
#include <stdexcept>
#include <vector>

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F&& f,const char* fragment){bool rejected=false;try{f();}catch(const std::exception& e){rejected=std::string(e.what()).find(fragment)!=std::string::npos;}require(rejected,"expected explicit query failure");}
int main(int argc,char** argv){
 OwnedTestRoot root("vector-metadata-filter");DatabaseEngine::init(root.dataRoot().string());
 struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();LSM::stopBackgroundTasks();WAL::shutdown();}}stop;
 try{
  DatabaseEngine::ensureUserRoot("alice");DatabaseEngine::createDatabase("alice","app","vector");DatabaseEngine::createCollection("alice","app","records");
  std::vector<json> docs;
  for(int i=0;i<120;++i)docs.push_back({{"id",std::to_string(i)},{"vector",{i==99?2000.:double(i),1.}},{"group",i<100?2:1},{"label",i<100?"other":"selected"},{"enabled",i>=100},{"nested",{{"group",i<100?2:1}}}});
  require(DatabaseEngine::insertMany("alice","app","records",docs).at("inserted")==120,"insert fixture");LSM::flush("alice","app","records");
  auto query=[](const json& filter){return json{{"vector",{1.,0.}},{"metric","dot"},{"k",3},{"search_mode","exact"},{"filter",filter}};};
  QueryLimiter::setMaxScanRows(1000);QueryLimiter::setMaxResultDocs(3);
  const auto exact=query({{"group",1}});const auto before=DatabaseEngine::queryVector("alice","app","records",exact);require(before.size()==3&&before[0].at("id")=="119","reference ranking");
  require(LSM::createSecondaryIndex("alice","app","records",{{"name","group_idx"},{"field","group"}}).value("complete",false),"index ready");
  require(LSM::waitForColumnIndexUpdates("alice","app","records",30000),"index updates drained");
  require(!LSM::vectorEqualityCandidates("alice","app","records","group",1,30,100000,[]{}),"cold vector lookup unexpectedly loaded an unbounded index");
  require(!LSM::findByField("alice","app","records","group","1",1).empty(),"warm existing metadata index");
#ifndef _WIN32
  if(argc>1&&std::string(argv[1])=="--publication-gap"){
   require(pacificdb::test::failpointsCompiled(),"failpoints unavailable");
   const auto fifo=root.dataRoot().parent_path()/"put-barrier";
   const auto evidence=root.dataRoot().parent_path()/"put-evidence.jsonl";
   require(mkfifo(fifo.c_str(),0600)==0,"FIFO creation");
   setenv("PACIFICDB_TEST_MODE","vector_index_crash",1);
   setenv("PACIFICDB_TEST_FAILPOINT_CONFIRM","I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE",1);
   setenv("PACIFICDB_TEST_FAILPOINT_INDEX","7",1);
   setenv("PACIFICDB_TEST_FAILPOINT","FP_APPLY_AFTER_BASE_BEFORE_INDEX",1);
   setenv("PACIFICDB_TEST_FAILPOINT_ACTION","pause",1);
   setenv("PACIFICDB_TEST_FAILPOINT_BARRIER_FIFO",fifo.c_str(),1);
   setenv("PACIFICDB_TEST_FAILPOINT_EVIDENCE",evidence.c_str(),1);
   std::string writerError;
   std::thread writer([&]{try{LSM::put("alice","app","records",{{"id","published"},{"group",1},{"tenant_id","alice"},{"vector",{2000.,1.}},{"version",999999}},true,7);}catch(const std::exception& e){writerError=e.what();}});
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
   while(!std::filesystem::exists(evidence)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   const bool paused=std::filesystem::exists(evidence);std::vector<json> during;
   if(paused)during=DatabaseEngine::queryVector("alice","app","records",exact);
   const int fd=open(fifo.c_str(),O_WRONLY|O_NONBLOCK);
   if(fd>=0){const char byte='x';(void)write(fd,&byte,1);close(fd);}else pacificdb::test::requestShutdown();
   writer.join();unsetenv("PACIFICDB_TEST_FAILPOINT");
   require(paused&&writerError.empty(),"publication pause failed");
   require(during.size()==3&&during.front().at("id")=="published","stale index omitted newly published matching vector");
   std::cout<<"VECTOR_METADATA_PUBLICATION_GAP_PASS\n";return 0;
  }
#endif
#ifndef _WIN32
  if(argc>1&&std::string(argv[1])=="--busy-index") {
   require(pacificdb::test::failpointsCompiled(),"failpoints unavailable");
   const auto fifo=root.dataRoot().parent_path()/"busy-index-barrier";
   const auto evidence=root.dataRoot().parent_path()/"busy-index-evidence.jsonl";
   require(mkfifo(fifo.c_str(),0600)==0,"FIFO creation");
   setenv("PACIFICDB_TEST_MODE","vector_index_crash",1);
   setenv("PACIFICDB_TEST_FAILPOINT_CONFIRM","I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE",1);
   setenv("PACIFICDB_TEST_FAILPOINT_INDEX","1",1);
   setenv("PACIFICDB_TEST_FAILPOINT","FP_IDX_REBUILD_BEFORE_PUBLISH",1);
   setenv("PACIFICDB_TEST_FAILPOINT_ACTION","pause",1);
   setenv("PACIFICDB_TEST_FAILPOINT_BARRIER_FIFO",fifo.c_str(),1);
   setenv("PACIFICDB_TEST_FAILPOINT_EVIDENCE",evidence.c_str(),1);
   json rebuilt;std::thread builder([&]{rebuilt=LSM::rebuildSecondaryIndex("alice","app","records","group_idx");});
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
   while(!std::filesystem::exists(evidence)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   const bool paused=std::filesystem::exists(evidence);bool limited=false;json execution;std::vector<json> rows;
   if(paused){
    QueryLimiter::setMaxScanRows(30);
    try{(void)DatabaseEngine::queryVector("alice","app","records",exact);}catch(const std::exception& e){limited=std::string(e.what()).find("scan_limit")!=std::string::npos;}
    QueryLimiter::setMaxScanRows(1000);rows=DatabaseEngine::queryVector("alice","app","records",exact,&execution);
   }
   const int fd=open(fifo.c_str(),O_WRONLY|O_NONBLOCK);
   if(fd>=0){const char byte='x';(void)write(fd,&byte,1);close(fd);}else pacificdb::test::requestShutdown();
   builder.join();unsetenv("PACIFICDB_TEST_FAILPOINT");
   require(paused&&limited,"busy index ignored fallback scan budget");
   require(rows==before&&execution.at("candidate_source")=="merged_scan","busy index fallback lost exact results");
   require(rebuilt.value("complete",false),"paused build did not publish after release");
   std::cout<<"VECTOR_METADATA_BUSY_INDEX_PASS\n";return 0;
  }
#endif
  // Pure plan/limit assertions require an uncontended index. Background
  // updates are exercised separately below and busy fallback above.
  // Regression: a complete metadata index must narrow the scan to 20 matches,
  // while ranking all of them despite MAX_RESULT_DOCS=3. The old full scan fails.
  QueryLimiter::setMaxScanRows(30);json execution;
  const auto indexed=DatabaseEngine::queryVector("alice","app","records",exact,&execution);
  require(indexed==before,"index changed exact ranking/payloads");
  require(execution.value("candidate_source",std::string())=="metadata_index","indexed plan missing");
  const auto check=[&](const json& filter,const std::vector<std::string>& ids){auto rows=DatabaseEngine::queryVector("alice","app","records",query(filter));require(rows.size()==ids.size(),"filter count");for(size_t i=0;i<ids.size();++i)require(rows[i].at("id")==ids[i],"filter rank");};
  check({{"group",1.0}},{"119","118","117"});
  for(const std::string metric:{"cosine","l2"}){
   auto request=exact;request["metric"]=metric;
   const auto rows=DatabaseEngine::queryVector("alice","app","records",request);
   require(rows.size()==3,"indexed metric cardinality");
   for(size_t i=0;i<3;++i){const int x=metric=="l2"?100+int(i):119-int(i);
    require(rows[i].at("id")==std::to_string(x),"indexed metric ranking");
    const double score=metric=="l2"?-std::sqrt(double((x-1)*(x-1)+1)):x/std::sqrt(double(x*x+1));
    require(std::abs(rows[i].at("score").get<double>()-score)<1e-12,"indexed metric score");}
  }
  rejects([&]{LSM::vectorEqualityCandidates("alice","app","records","group",1,30,1,[]{});},"bytes_exceeded");
  size_t checks=0;rejects([&]{LSM::vectorEqualityCandidates("alice","app","records","group",1,30,100000,[&]{if(++checks==5)throw std::runtime_error("cancelled");});},"cancelled");
  check({{"$and",json::array({json{{"group",{{"$eq",1}}}},json{{"enabled",true}},json{{"id",{{"$ne","119"}}}}})}},{"118","117","116"});
  check({{"group",999}},{});
  QueryLimiter::setMaxScanRows(1000);
  const json orFilter{{"$or",json::array({json{{"group",1}},json{{"id","99"}}})}};
  check(orFilter,{"99","119","118"});
  (void)DatabaseEngine::queryVector("alice","app","records",query(orFilter),&execution);
  require(execution.at("candidate_source")=="merged_scan","OR incorrectly narrowed to one branch");
  check({{"$not",{{"group",1}}}},{"99","98","97"});
  check({{"nested.group",1}},{"119","118","117"});
  // Numeric representations and string collisions share legacy index buckets;
  // actual predicate evaluation must remove extra types without losing equals.
  LSM::startBackgroundTasks();
  DatabaseEngine::insertMany("alice","app","records",{
   {{"id","float"},{"group",1.0},{"vector",{500.,1.}}},
   {{"id","text"},{"group","1"},{"vector",{900.,1.}}},
   {{"id","null"},{"group",nullptr},{"vector",{700.,1.}}},
   {{"id","missing"},{"vector",{800.,1.}}}});
  require(LSM::waitForColumnIndexUpdates("alice","app","records",30000),"mutation index drained");
  LSM::stopBackgroundTasks();
  QueryLimiter::setMaxScanRows(30);
  check({{"group",1}},{"float","119","118"});check({{"group",1.0}},{"float","119","118"});check({{"group","1"}},{"text"});check({{"group",nullptr}},{"null"});
  LSM::startBackgroundTasks();
  require(DatabaseEngine::updateOne("alice","app","records",{{"id","float"}},{{"$set",{{"group",2}}}}),"update");
  require(DatabaseEngine::deleteOne("alice","app","records",{{"id","119"}}),"delete");
  require(LSM::waitForColumnIndexUpdates("alice","app","records",30000),"updates drained");
  LSM::stopBackgroundTasks();
  check({{"group",1}},{"118","117","116"});
  LSM::startBackgroundTasks();
  LSM::put("alice","app","records",{{"id","foreign"},{"group",1},{"tenant_id","bob"},{"vector",{10000.,1.}}});
  require(LSM::waitForColumnIndexUpdates("alice","app","records",30000),"foreign row index drained");
  LSM::stopBackgroundTasks();
  check({{"group",1}},{"118","117","116"});
  QueryLimiter::setMaxScanRows(1);rejects([&]{DatabaseEngine::queryVector("alice","app","records",exact);},"scan_limit");QueryLimiter::setMaxScanRows(1000);
  auto token=QueryCancel::makeToken();token->cancelled.store(true);QueryCancel::setToken(token);rejects([&]{DatabaseEngine::queryVector("alice","app","records",exact);},"cancelled");QueryCancel::clearToken();
  DatabaseEngine::setReadContext("strong",0,9000000000000000000LL);require(DatabaseEngine::queryVector("alice","app","records",exact).empty(),"read floor leaked");DatabaseEngine::clearReadContext();
  DatabaseEngine::setReadContext("bounded",1000,-1);
  (void)DatabaseEngine::queryVector("alice","app","records",exact,&execution);
  require(execution.at("candidate_source")=="merged_scan","bounded context used metadata plan");DatabaseEngine::clearReadContext();
  LSM::flush("alice","app","records");LSM::compact("alice","app","records");check({{"group",1}},{"118","117","116"});
#ifndef _WIN32
  // Pause after complete ID enumeration, move a previously excluded best vector
  // into the filter, and prove that stale coverage cannot be returned as exact.
  if(pacificdb::test::failpointsCompiled()){
   const auto fifo=root.dataRoot().parent_path()/"metadata-barrier";
   const auto evidence=root.dataRoot().parent_path()/"metadata-evidence.jsonl";
   require(mkfifo(fifo.c_str(),0600)==0,"FIFO creation");
   setenv("PACIFICDB_TEST_MODE","vector_index_crash",1);
   setenv("PACIFICDB_TEST_FAILPOINT_CONFIRM","I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE",1);
   setenv("PACIFICDB_TEST_FAILPOINT_INDEX","1",1);
   setenv("PACIFICDB_TEST_FAILPOINT","FP_VECTOR_METADATA_AFTER_CANDIDATES",1);
   setenv("PACIFICDB_TEST_FAILPOINT_ACTION","pause",1);
   setenv("PACIFICDB_TEST_FAILPOINT_BARRIER_FIFO",fifo.c_str(),1);
   setenv("PACIFICDB_TEST_FAILPOINT_EVIDENCE",evidence.c_str(),1);
   std::string error;std::thread reader([&]{try{DatabaseEngine::queryVector("alice","app","records",exact);}catch(const std::exception& e){error=e.what();}});
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
   while(!std::filesystem::exists(evidence)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
   bool changed=false;
   if(std::filesystem::exists(evidence)){LSM::startBackgroundTasks();changed=DatabaseEngine::updateOne("alice","app","records",{{"id","float"}},{{"$set",{{"group",1}}}});}
   const int fd=open(fifo.c_str(),O_WRONLY|O_NONBLOCK);
   if(fd>=0){const char byte='x';(void)write(fd,&byte,1);close(fd);}else pacificdb::test::requestShutdown();
   reader.join();unsetenv("PACIFICDB_TEST_FAILPOINT");
   require(changed&&error.find("freshness_changed")!=std::string::npos,"mutation produced incomplete indexed exact result");
   require(LSM::waitForColumnIndexUpdates("alice","app","records",30000),"concurrent update drained");
   LSM::stopBackgroundTasks();
   check({{"group",1}},{"float","118","117"});
  }
#endif
  QueryLimiter::setMaxScanRows(1000);LSM::startBackgroundTasks();
  DatabaseEngine::insertMany("alice","app","records",{{{"id","async"},{"group",1},{"vector",{3000.,1.}}}});
  require(LSM::waitForColumnIndexUpdates("alice","app","records",30000),"background insert did not drain");
  const auto asyncRows=DatabaseEngine::queryVector("alice","app","records",exact);
  require(asyncRows.size()==3&&asyncRows.front().at("id")=="async","background indexing lost latest winner");
  require(DatabaseEngine::updateOne("alice","app","records",{{"id","async"}},{{"$set",{{"group",2}}}}),"background update");
  require(LSM::waitForColumnIndexUpdates("alice","app","records",30000),"background update did not drain");
  for(const auto& row:DatabaseEngine::queryVector("alice","app","records",exact))require(row.at("id")!="async","background update left stale filter entry");
  std::cout<<"VECTOR_METADATA_FILTER_PASS\n";
 }catch(const std::exception& e){std::cerr<<"VECTOR_METADATA_FILTER_FAIL: "<<e.what()<<'\n';return 1;}
}
