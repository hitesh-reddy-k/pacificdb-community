#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "wal.hpp"
#include "query_limiter.hpp"
#include "raft_core.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <set>

void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
 OwnedTestRoot root("vector-cold-filter");DatabaseEngine::init(root.dataRoot().string());
 struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();LSM::stopBackgroundTasks();WAL::shutdown();}}stop;
 try{
  DatabaseEngine::ensureUserRoot("alice");DatabaseEngine::createDatabase("alice","app","vector");DatabaseEngine::createCollection("alice","app","records");
  const auto put=[](const char* id,int version,int group,bool deleted=false){json row{{"id",id},{"_mvcc_version",version},{"version",version},{"group",group},{"tenant_id","alice"},{"vector",{double(version),1.}}};if(deleted)row["_deleted"]=true;LSM::put("alice","app","records",row);};
  put("moved",10,1);put("gone",10,1);put("sst_newer",10,1);put("memory_newer",10,2);put("kept",10,1);LSM::flush("alice","app","records");
  put("moved",20,2);put("gone",20,1,true);put("sst_newer",20,2);LSM::flush("alice","app","records");
  put("sst_newer",5,1);put("memory_newer",30,1);
  std::vector<json> rows;size_t rejected=0;
  LSM::visitLatest("alice","app","records",[&](const LSM::LatestRowView& row){rows.push_back(row.materialize());return true;},false,false,[&](const LSM::LatestRowView& row){const auto group=row.field("group");const bool accept=group&&*group==1;if(!accept)++rejected;return accept;});
  std::set<std::string> ids;for(const auto& row:rows)ids.insert(row.at("id").get<std::string>());
  require(ids==std::set<std::string>{"kept","memory_newer"},"prefilter resurrected an older matching version or tombstone");require(rejected>=2,"filter did not run on resolved nonmatching winners");
  const std::vector<std::string> selected{"moved","gone","sst_newer","memory_newer","kept","absent"};
  const auto capture=[&](const std::vector<std::string>& requested){std::vector<json> found;const auto stats=LSM::visitVectorCandidates("alice","app","records",requested,[&](const LSM::LatestRowView& row){found.push_back(row.materialize());return true;},[]{});return std::make_pair(found,stats);};
  const auto first=capture(selected),second=capture(selected);
  const auto sorted=[](std::vector<json> values){std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.at("id")<b.at("id");});return values;};
  require(sorted(first.first)==sorted(second.first),"candidate batch/cache paths disagree");
  require(first.first.size()==4,"missing/deleted candidate returned");
  for(const auto& row:first.first){if(row.at("id")=="moved"||row.at("id")=="sst_newer")require(row.at("version")==20&&row.at("group")==2,"candidate version resolution failed");}
  require(first.second.storageRows>0,"cold candidate read skipped storage");require(second.second.cacheHits==4,"resolved candidate records were not safely reused");
  // A document read that revalidates the same payload must preserve the
  // independently certified vector cache proof (and vice versa).
  const auto revalidated=LSM::findStrongSnapshotBound("alice","app","records","kept",11);
  require(revalidated.value("found",false)&&!revalidated.value("floor_satisfied",true),"point revalidation fixture");
  const auto afterDocumentRead=capture(selected);
  require(afterDocumentRead.second.cacheHits==4&&afterDocumentRead.second.storageRows==0,"document revalidation discarded unchanged merged cache proof");
  bool cancelled=false;try{LSM::visitVectorCandidates("alice","app","records",selected,[](const auto&){return true;},[]{throw std::runtime_error("cancelled");});}catch(const std::runtime_error& e){cancelled=std::string(e.what())=="cancelled";}require(cancelled,"candidate traversal ignored cancellation");
  DatabaseEngine::createCollection("alice","app","pending");
  LSM::put("alice","app","pending",json{{"id","p"},{"version",1},{"_visibility_state","COMMITTED_VISIBLE"}});LSM::flush("alice","app","pending");
  LSM::put("alice","app","pending",json{{"id","p"},{"version",2},{"_visibility_state","PENDING"}});LSM::flush("alice","app","pending");
  const auto pendingCapture=[](){std::vector<json> found;LSM::visitVectorCandidates("alice","app","pending",{"p"},[&](const auto& row){found.push_back(row.materialize());return true;},[]{});return found;};
  const auto pendingCold=pendingCapture();require(pendingCold.size()==1&&pendingCold.front().at("version")==1,"merged scan lost the newest committed version");
  const auto point=LSM::findStrongSnapshotBound("alice","app","pending","p",0);
  require(point.value("found",false),"pending parity fixture did not expose older point-read version");
  require(pendingCapture()==pendingCold,"point cache changed merged winner visibility without a mutation");
  DatabaseEngine::createCollection("alice","app","flush_parity");
  LSM::put("alice","app","flush_parity",json{{"id","p"},{"version",1},{"_visibility_state","COMMITTED_VISIBLE"}});LSM::flush("alice","app","flush_parity");
  LSM::put("alice","app","flush_parity",json{{"id","p"},{"version",2},{"_visibility_state","PENDING"}});
  const auto flushCapture=[](){std::vector<json> found;LSM::visitVectorCandidates("alice","app","flush_parity",{"p"},[&](const auto& row){found.push_back(row.materialize());return true;},[]{});return found;};
  require(flushCapture().size()==1,"pre-flush merged fixture did not retain visible SST row");
  LSM::flush("alice","app","flush_parity");
  require(flushCapture().size()==1&&flushCapture().front().at("version")==1,"flush changed committed-version visibility");
  DatabaseEngine::createCollection("alice","app","tie_parity");
  LSM::put("alice","app","tie_parity",json{{"id","p"},{"_mvcc_version",7},{"value",40}});LSM::flush("alice","app","tie_parity");
  LSM::put("alice","app","tie_parity",json{{"id","p"},{"_mvcc_version",7},{"value",41}});
  // A floor above the row version forces a storage comparison, excluding a
  // writer-fed point-cache shortcut from this layer-priority regression.
  const auto tiePoint=LSM::findStrongSnapshotBound("alice","app","tie_parity","p",8);
  require(tiePoint.value("found",false)&&tiePoint.at("documents").at(0).at("value")==41,"point read let SST overwrite equal-version active memtable");
  std::vector<json> tieRows;LSM::visitLatest("alice","app","tie_parity",[&](const auto& row){tieRows.push_back(row.materialize());return true;});
  require(tieRows.size()==1&&tieRows.front().at("value")==41,"merged tie priority disagrees with point read");
  DatabaseEngine::createCollection("alice","app","tombs");
  for(int i=0;i<50;++i)LSM::put("alice","app","tombs",json{{"id",std::to_string(i)},{"version",1},{"_deleted",true}});
  LSM::flush("alice","app","tombs");
  size_t polls=0;cancelled=false;
  try{LSM::visitVectorCandidates("alice","app","tombs",{"absent"},[](const auto&){return true;},[&]{if(++polls==10)throw std::runtime_error("cancelled_mid_scan");});}catch(const std::runtime_error& e){cancelled=std::string(e.what())=="cancelled_mid_scan";}
  require(cancelled&&polls==10,"tombstone traversal did not poll cancellation after entry");
  DatabaseEngine::createCollection("alice","app","memory_tombs");
  for(int i=0;i<50;++i)LSM::put("alice","app","memory_tombs",json{{"id",std::to_string(i)},{"version",1},{"_deleted",true}});
  polls=0;cancelled=false;
  try{LSM::visitVectorCandidates("alice","app","memory_tombs",{"absent"},[](const auto&){return true;},[&]{if(++polls==10)throw std::runtime_error("cancelled_in_memory");});}catch(const std::runtime_error& e){cancelled=std::string(e.what())=="cancelled_in_memory";}
  require(cancelled&&polls==10,"memory preparation did not poll cancellation");
  const auto savedBytes=QueryLimiter::getMaxResultBytes();QueryLimiter::setMaxResultBytes(1);bool bounded=false;
  try{capture(selected);}catch(const std::runtime_error& e){bounded=std::string(e.what())=="vector_candidate_bytes_exceeded";}
  QueryLimiter::setMaxResultBytes(savedBytes);require(bounded,"candidate IDs exceeded byte budget silently");
  // Independent visibility matrix: exercise selective encoded and in-memory
  // accessors under legacy standalone policy and real enabled-Raft fencing.
  DatabaseEngine::createCollection("alice","app","visibility_sst");
  DatabaseEngine::createCollection("alice","app","visibility_memory");
  std::vector<json> visibilityRows{
   {{"id","good"},{"_visibility_state","COMMITTED_VISIBLE"},{"_raft_commit_index",0}},
   {{"id","pending"},{"_visibility_state","PENDING"},{"_raft_commit_index",0}},
   {{"id","alias_good"},{"_visibility_state",42},{"visibility_state","COMMITTED_VISIBLE"},{"commit_index",0}},
   {{"id","alias_pending"},{"_visibility_state",nullptr},{"visibility_state","PENDING"},{"_raft_commit_index",0}},
   {{"id","false_commit"},{"_visibility_state",nullptr},{"committed",false},{"_raft_commit_index",0}},
   {{"id","true_commit"},{"_visibility_state",nullptr},{"committed",true},{"_raft_commit_index",0}},
   {{"id","future"},{"_visibility_state","COMMITTED_VISIBLE"},{"_raft_commit_index",1}},
   {{"id","future_floor"},{"_visibility_state","COMMITTED_VISIBLE"},{"_raft_commit_index",0},{"_visibility_floor",1}},
   {{"id","missing_index"},{"_visibility_state",nullptr},{"committed",true}},
   {{"id","fallback_index"},{"_visibility_state",nullptr},{"committed",true},{"_raft_commit_index","bad"},{"commit_index",0}},
   {{"id","nonnumeric_floor"},{"_visibility_state",nullptr},{"committed",true},{"commit_index",0},{"_visibility_floor","bad"}},
   {{"id","negative_index"},{"_visibility_state",nullptr},{"committed",true},{"_raft_commit_index",-1}},
   {{"id","null_state"},{"_visibility_state",nullptr},{"_raft_commit_index",0}},
   {{"id","unknown_state"},{"_visibility_state","OTHER"},{"_raft_commit_index",0}},
   {{"id","primary_precedence"},{"_visibility_state","PENDING"},{"visibility_state","COMMITTED_VISIBLE"},{"committed",true},{"_raft_commit_index",0}},
   {{"id","alias_precedence"},{"_visibility_state",nullptr},{"visibility_state","PENDING"},{"committed",true},{"_raft_commit_index",0}},
   {{"id","state_precedence"},{"_visibility_state","COMMITTED_VISIBLE"},{"committed",false},{"_raft_commit_index",0}},
   {{"id","visible_tombstone"},{"_visibility_state","TOMBSTONED"},{"_deleted",true},{"_raft_commit_index",0}}
  };
  std::set<std::string> legacyExpected;
  for(const auto& row:visibilityRows){LSM::put("alice","app","visibility_sst",row);LSM::put("alice","app","visibility_memory",row);const auto id=row.at("id").get<std::string>();if(id!="pending"&&id!="primary_precedence")legacyExpected.insert(id);}
  LSM::flush("alice","app","visibility_sst");
  const auto visibleIds=[](const char* collection){std::set<std::string> ids;LSM::visitLatest("alice","app",collection,[&](const auto& row){ids.insert(row.field("id")->template get<std::string>());return true;},false,true);return ids;};
  require(visibleIds("visibility_sst")==legacyExpected&&visibleIds("visibility_memory")==legacyExpected,"legacy visibility field precedence/type semantics changed");
#ifdef _WIN32
  _putenv_s("RAFT_STANDALONE_BYPASS","0");
  _putenv_s("DATA_ROOT",root.dataRoot().string().c_str());
  _putenv_s("DATA_DIR",root.dataRoot().string().c_str());
#else
  setenv("RAFT_STANDALONE_BYPASS","0",1);
  setenv("DATA_ROOT",root.dataRoot().string().c_str(),1);
  setenv("DATA_DIR",root.dataRoot().string().c_str(),1);
#endif
  std::filesystem::create_directories(root.dataRoot()/"raft");
  auto& raft=RaftCore::instance();raft.init({},"cold-visibility",0,true);
  require(raft.getCommitIndex()==0,"visibility matrix requires empty Raft log");
  const std::set<std::string> raftExpected{"good","alias_good","true_commit","fallback_index","nonnumeric_floor","null_state","state_precedence","visible_tombstone"};
  require(visibleIds("visibility_sst")==raftExpected&&visibleIds("visibility_memory")==raftExpected,"Raft visibility state/commit/floor semantics changed");
  raft.stop();
  std::cout<<"VECTOR_COLD_FILTER_PASS\n";
 }catch(const std::exception& e){std::cerr<<"VECTOR_COLD_FILTER_FAIL: "<<e.what()<<'\n';return 1;}
}
