#include "native_vector_checkpoint.hpp"
#include "native_vector_record.hpp"
#include "owned_test_root.hpp"
#include "test_failpoint.hpp"
#include "storage_format_v2.hpp"
#include "vector_value.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace pacificdb;
HNSWConfig config(){HNSWConfig c;c.dimensions=3;c.metric=DistanceMetric::DOT_PRODUCT;c.maxElements=300;c.maxMemoryBytes=4*1024*1024;c.efSearch=200;return c;}
void stage(HNSWIndex& graph,bool changed=false){
 for(int i=0;i<100;++i)assert(graph.stage(std::to_string(i),{float(i),1,3},
  packNativeVectorRecord({{"id",std::to_string(i)},{"precise",1.0000000000000009},{"metadata",changed&&i==99?1:0}}),{}));
}
void verify(HNSWIndex& graph){const auto hits=graph.searchRecords({1,0,0},10,{},200);assert(hits.size()==10);
 for(int i=0;i<10;++i){assert(hits[i].id==std::to_string(99-i));const auto raw=unpackNativeVectorRecord(hits[i].record);
  assert(json::from_msgpack(raw.begin(),raw.end()).at("precise").get<double>()==1.0000000000000009);}}
int main(){
 for(const auto& values:std::vector<std::vector<double>>{{0.,0.,0.},{1e-20,1e-20,0.},{1.,1e-14,-2.},{1e200,1e200,1e200},{1e-200,1e-200,0.}}){
  const auto packed=json::to_msgpack(json{{"vector",values}});
  const storage_v2::MsgpackRowView row(packed.data(),packed.size(),true);
  const std::vector<double> query{1.000000000000001,2.,3.};
  for(const std::string metric:{"cosine","l2","dot"}){
   const auto expected=vectors::score(vectors::decode(query),vectors::decode(values),metric);
   const auto actual=row.densePreciseScore("vector",query,metric);assert(actual&&*actual==expected);
  }
 }
 const json spec={{"dimensions",3},{"metric","dot"}};
 OwnedTestRoot root("native-graph-checkpoint");std::filesystem::create_directory(root.dataRoot());
 auto checkpoint=nativeVectorCheckpoints(root.dataRoot(),"tenant/app/collection/index",spec,4*1024*1024);
 HNSWIndex graph(config());stage(graph);assert(!checkpoint.restore(graph,{}));graph.buildStaged({});verify(graph);checkpoint.save(graph,{});
 HNSWIndex restored(config());stage(restored);assert(checkpoint.restore(restored,{}));verify(restored);
 HNSWIndex stale(config());stage(stale,true);assert(!checkpoint.restore(stale,{}));stale.buildStaged({});
 auto changedConfig=nativeVectorCheckpoints(root.dataRoot(),"tenant/app/collection/index",{{"dimensions",3},{"metric","cosine"}},4*1024*1024);
 HNSWIndex mismatched(config());stage(mismatched);assert(!changedConfig.restore(mismatched,{}));
 const auto dir=std::filesystem::path(root.dataRoot().string()+".vector-derived-v1");std::filesystem::path file;
 for(const auto& item:std::filesystem::directory_iterator(dir))if(item.path().extension()==".graph")file=item.path();assert(!file.empty());
 {std::fstream damage(file,std::ios::binary|std::ios::in|std::ios::out);damage.seekp(105);damage.put('X');}
 HNSWIndex corrupt(config());stage(corrupt);bool rejected=false;
 try{checkpoint.restore(corrupt,{});}catch(const std::runtime_error&){rejected=true;}assert(rejected);corrupt.buildStaged({});verify(corrupt);checkpoint.save(corrupt,{});
 auto tiny=nativeVectorCheckpoints(root.dataRoot(),"tenant/app/collection/index",spec,1024);rejected=false;
 try{tiny.save(corrupt,{});}catch(const std::exception&){rejected=true;}assert(rejected);
 HNSWIndex afterFailure(config());stage(afterFailure);assert(checkpoint.restore(afterFailure,{}));verify(afterFailure);
#ifndef _WIN32
 const auto outside=root.dataRoot().parent_path()/"outside-file";{std::ofstream out(outside);out<<"must remain unchanged";}
 std::filesystem::remove(file);std::filesystem::create_symlink(outside,file);rejected=false;
 try{checkpoint.restore(corrupt,{});}catch(const std::runtime_error&){rejected=true;}assert(rejected);
 {std::ifstream in(outside);std::string contents;std::getline(in,contents);assert(contents=="must remain unchanged");}
 if(test::failpointsCompiled())for(const char* phase:{"FP_VECTOR_CHECKPOINT_BEFORE_BODY","FP_VECTOR_CHECKPOINT_BEFORE_RENAME","FP_VECTOR_CHECKPOINT_AFTER_RENAME"}){
  OwnedTestRoot crash("graph-checkpoint-crash");std::filesystem::create_directory(crash.dataRoot());
  const auto child=fork();assert(child>=0);
  if(!child){
   setenv("PACIFICDB_TEST_MODE","vector_index_crash",1);
   setenv("PACIFICDB_TEST_FAILPOINT_CONFIRM","I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE",1);
   setenv("PACIFICDB_TEST_FAILPOINT_INDEX","1",1);setenv("PACIFICDB_TEST_FAILPOINT_ACTION","crash",1);setenv("PACIFICDB_TEST_FAILPOINT",phase,1);
   HNSWIndex index(config());stage(index);index.buildStaged({});
   nativeVectorCheckpoints(crash.dataRoot(),"crash",spec,4*1024*1024).save(index,{});_exit(3);
  }
  int result=0;assert(waitpid(child,&result,0)==child&&WIFEXITED(result)&&WEXITSTATUS(result)==86);
  auto recovery=nativeVectorCheckpoints(crash.dataRoot(),"crash",spec,4*1024*1024);
  HNSWIndex current(config());stage(current);const bool reused=recovery.restore(current,{});
  assert(reused==(std::string(phase)=="FP_VECTOR_CHECKPOINT_AFTER_RENAME"));
  if(!reused)current.buildStaged({});verify(current);recovery.save(current,{});
  HNSWIndex again(config());stage(again);assert(recovery.restore(again,{}));verify(again);
  std::cout<<"CHECKPOINT_CRASH_PASS "<<phase<<'\n';
 }
#endif
 std::cout<<"NATIVE_VECTOR_CHECKPOINT_PASS\n";
}
