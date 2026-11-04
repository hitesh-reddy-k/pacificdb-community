#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "wal.hpp"
#include "query_limiter.hpp"
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
  std::cout<<"VECTOR_COLD_FILTER_PASS\n";
 }catch(const std::exception& e){std::cerr<<"VECTOR_COLD_FILTER_FAIL: "<<e.what()<<'\n';return 1;}
}
