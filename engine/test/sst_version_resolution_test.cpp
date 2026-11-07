#include "storage_format_v2.hpp"
#include "owned_test_root.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using json=nlohmann::json;
using namespace pacificdb::storage_v2;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char**argv){
 try{
  OwnedTestRoot root("sst-versions");std::string error;
  std::vector<json> rows={
   {{"id","a"},{"_mvcc_version",2},{"value",20}},{{"id","a"},{"_mvcc_version",4},{"value",40}},{{"id","a"},{"_mvcc_version",1},{"value",10}},
   {{"id","b"},{"_mvcc_version",1},{"value",10}},{{"id","b"},{"_mvcc_version",3},{"_deleted",true}},
   {{"id","c"},{"_mvcc_version",1},{"value",10}},{{"id","c"},{"_mvcc_version",2},{"value",20},{"_visibility_state","PENDING"}},
   {{"id","d"},{"_mvcc_version",5},{"value",50}},{{"id","d"},{"_mvcc_version",5},{"value",51}}};
  std::vector<std::reference_wrapper<const json>> refs;for(const auto& row:rows)refs.push_back(row);
  const auto path=root.dataRoot()/"versions.sst";
  require(writeSst(path,refs,4096,nullptr,&error),error.c_str());require(verifySst(path,&error),"valid versioned SST rejected");
  const auto visible=findInSst(path,"c",&error,[](const auto& row){const auto state=row.field("_visibility_state");return !state||*state!="PENDING";});
  require(visible&&visible->at("value")==10,"point read lost eligible older version inside SST");
  const auto tie=findInSst(path,"d",&error);require(tie&&tie->at("value")==51,"point tie did not select later row");
  if(argc>1&&std::string(argv[1])=="--point"){
   const auto found=findInSst(path,"a",&error);require(found&&found->at("value")==40,"point read did not select the newest version inside one SST");
  }else{
   std::vector<std::pair<std::string,int>> found;
   require(visitLatestSstRows({path},[&](const MsgpackRowView& row){found.push_back({row.field("id")->get<std::string>(),row.field("value")->get<int>()});return true;},&error,true),error.c_str());
   require(found==std::vector<std::pair<std::string,int>>{{"a",40},{"c",20},{"d",51}},"merge emitted duplicate/obsolete versions or resurrected a tombstone within one SST");
   found.clear();
   require(visitLatestSstRows({path},[&](const MsgpackRowView& row){found.push_back({row.field("id")->get<std::string>(),row.field("value")->get<int>()});return true;},&error,true,false,{},[](const auto& row){const auto state=row.field("_visibility_state");return !state||*state!="PENDING";}),error.c_str());
   require(found==std::vector<std::pair<std::string,int>>{{"a",40},{"c",10},{"d",51}},"eligible-version selection inside one SST failed");
  }
  std::vector<json> spans;
  for(int i=0;i<20;++i)spans.push_back({{"id","span"},{"_mvcc_version",i==9?42:i},{"value",i==9?42:i},{"payload",std::string(1024,'x')}});
  refs.clear();for(const auto& row:spans)refs.push_back(row);
  const auto across=root.dataRoot()/"blocks.sst";SstWriteStats stats;
  require(writeSst(across,refs,4096,&stats,&error)&&stats.blocks>1,"duplicate block-boundary fixture");
  const auto found=findInSst(across,"span",&error);require(found&&found->at("value")==42,"point read skipped newer versions in later SST blocks");
  size_t count=0;require(visitLatestSstRows({across},[&](const auto& row){++count;require(row.field("value")==42,"cross-block merged winner");return true;},&error,true)&&count==1,"cross-block duplicate versions were emitted");
  std::vector<json> newer{{{"id","span"},{"_mvcc_version",42},{"value",43}}};refs.clear();refs.push_back(newer.front());
  const auto later=root.dataRoot()/"later.sst";require(writeSst(later,refs,4096,nullptr,&error),error.c_str());
  count=0;require(visitLatestSstRows({across,later},[&](const auto& row){++count;require(row.field("value")==43,"later SST did not win equal-version tie");return true;},&error,true)&&count==1,"cross-file tie duplicated result");
  // File recency never outranks a higher committed record version. A pending
  // higher version must still leave the eligible committed winner visible.
  std::vector<json> overlay{{{"id","span"},{"_mvcc_version",41},{"value",41}}};refs.clear();refs.push_back(overlay.front());
  const auto lower=root.dataRoot()/"lower.sst";require(writeSst(lower,refs,4096,nullptr,&error),error.c_str());
  count=0;require(visitLatestSstRows({across,lower},[&](const auto& row){++count;require(row.field("value")==42,"file recency displaced a higher record version");return true;},&error,true)&&count==1,"higher-version cross-file result");
  overlay.front()={{"id","span"},{"_mvcc_version",99},{"value",99},{"_visibility_state","PENDING"}};refs.clear();refs.push_back(overlay.front());
  const auto pending=root.dataRoot()/"pending.sst";require(writeSst(pending,refs,4096,nullptr,&error),error.c_str());
  count=0;require(visitLatestSstRows({across,pending},[&](const auto& row){++count;require(row.field("value")==42,"newer pending file hid eligible record");return true;},&error,true,false,{},[](const auto& row){const auto state=row.field("_visibility_state");return !state||*state!="PENDING";})&&count==1,"pending overlay result");
  size_t polls=0;bool cancelled=false;
  try{visitLatestSstRows({across},[](const auto&){return true;},&error,true,false,[&]{if(++polls==10)throw std::runtime_error("cancel_duplicate_run");});}
  catch(const std::runtime_error& e){cancelled=std::string(e.what())=="cancel_duplicate_run";}
  require(cancelled&&polls==10,"duplicate-run traversal ignored cancellation");
  std::cout<<"SST_VERSION_RESOLUTION_PASS\n";
 }catch(const std::exception& e){std::cerr<<"SST_VERSION_RESOLUTION_FAIL: "<<e.what()<<'\n';return 1;}
}
