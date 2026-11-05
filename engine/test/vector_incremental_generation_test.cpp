#include "vector_index_generation.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <thread>
using namespace pacificdb;
using State=VectorIndexGeneration::State;
void wait(const std::shared_ptr<VectorIndexGeneration>& manager,State wanted,uint64_t minimum=0){
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(true){const auto status=manager->status();if(status.state==wanted&&status.generation>=minimum)return;
  if(status.state==State::FAILED&&wanted!=State::FAILED)throw std::runtime_error(status.error);
  assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
}
void completeVisibility(){
 // Model a stalled maintenance worker without exhausting the development host.
 // Only global commit visibility changes; the complete vector source is stable.
 std::atomic<uint64_t> visibility{0},revision{1};
 std::atomic<bool> pauseWorker{false};
 std::mutex gate;std::condition_variable wake;bool entered=false,released=false;
 const auto stamp=[&]{return VectorFreshness::Stamp{1,revision.load(),visibility.load()};};
 const auto certificate=[&]()->std::optional<VectorFreshness::Stamp>{
  if(pauseWorker.load()){
   std::unique_lock<std::mutex> lock(gate);entered=true;wake.notify_all();
   wake.wait(lock,[&]{return released;});
  }
  return stamp();
 };
 HNSWConfig config;config.dimensions=2;config.metric=DistanceMetric::DOT_PRODUCT;
 config.maxElements=100;config.maxMemoryBytes=1024*1024;
 auto journal=std::make_shared<VectorChangeJournal>(100,65536);
 auto anchor=journal->capture();auto manager=std::make_shared<VectorIndexGeneration>(65536);
 assert(manager->enqueue(config,certificate,[](const VectorIndexGeneration::Emit& emit,const std::atomic<bool>&){emit({"one",{1,0}});emit({"two",{2,0}});}));
 wait(manager,State::READY);const auto strict=manager->acquire(stamp());assert(strict&&!strict->visibilityComplete);
 manager->enableMaintenance(journal,anchor,certificate,[&](const std::string& id)->std::optional<VectorIndexGeneration::Record>{
  // Keep unrelated commits advancing even during source resolution. A complete
  // journal and unchanged source revision must still permit a covered publish.
  ++visibility;return VectorIndexGeneration::Record{id,{500,0}};
 },128,1000,true);
 const auto initial=manager->acquire(stamp());assert(initial&&initial->visibilityComplete);
 pauseWorker=true;
 {std::unique_lock<std::mutex> lock(gate);assert(wake.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}));}
 for(uint64_t index=1;index<=1000;++index){
  visibility=index;const auto retained=manager->acquire(stamp());assert(retained&&retained->number==initial->number);
  assert(retained->accepts(stamp()));assert(!strict->accepts(stamp()));
  assert(retained->graph->search({1,0},1).front().first=="two");
 }
 auto wrong=stamp();++wrong.collection;assert(!manager->acquire(wrong));
 wrong=stamp();++wrong.root;assert(!manager->acquire(wrong));
 wrong=stamp();++wrong.configuration;assert(!manager->acquire(wrong));
 wrong=initial->certificate;wrong.visibility=0;assert(initial->accepts(wrong));
 // A real source change with a future visibility floor remains unavailable,
 // even though unrelated global commits are permitted for the complete view.
 assert(journal->append("pending",2000));assert(!manager->acquire(stamp()));++revision;
 assert(!manager->acquire(stamp()));
 {std::lock_guard<std::mutex> lock(gate);pauseWorker=false;released=true;wake.notify_all();}
 std::this_thread::sleep_for(std::chrono::milliseconds(250));assert(!manager->acquire(stamp()));
 visibility=2000;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 std::shared_ptr<const VectorIndexGeneration::Generation> updated;
 while(!(updated=manager->acquire(stamp()))){assert(manager->status().state!=State::FAILED);assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
 assert(updated->visibilityComplete&&updated->graph->search({1,0},1).front().first=="pending");
 wrong=updated->certificate;--wrong.visibility;assert(!updated->accepts(wrong));
 // A journal edit without a source revision cannot borrow an unrelated commit
 // as a version change; it is an explicit failed coverage proof.
 assert(journal->append("unversioned",0));++visibility;assert(!manager->acquire(stamp()));
 wait(manager,State::FAILED);assert(manager->status().error.find("unversioned")!=std::string::npos);
 assert(!manager->acquire(stamp()));manager->closeAndWait();
 std::cout<<"COMPLETE_VISIBILITY_STALLED_WORKER_PENDING_FLOOR_UNVERSIONED_GUARDS_PASS\n";
}
int main(){
 completeVisibility();
 std::mutex sourceMutex;std::map<std::string,std::vector<float>> records;
 uint64_t revision=1,visibility=0,root=1;bool throwCertificate=false;
 for(int i=0;i<100;++i)records[std::to_string(i)]={float(i),1};
 const auto certificate=[&]()->std::optional<VectorFreshness::Stamp>{std::lock_guard<std::mutex> lock(sourceMutex);
  if(throwCertificate)throw std::runtime_error("certificate unavailable");return VectorFreshness::Stamp{root,revision,visibility};};
 const auto source=[&](const VectorIndexGeneration::Emit& emit,const std::atomic<bool>& stop){
  std::map<std::string,std::vector<float>> snapshot;
  {std::lock_guard<std::mutex> lock(sourceMutex);snapshot=records;}
  // Emit can run certificate checks during construction. Do not retain the
  // same mock storage mutex that its certificate callback needs.
  for(const auto& [id,v]:snapshot){if(stop)return;emit({id,v});}};
 const auto resolve=[&](const std::string& id)->std::optional<VectorIndexGeneration::Record>{std::lock_guard<std::mutex> lock(sourceMutex);
  auto found=records.find(id);if(found==records.end())return {};return VectorIndexGeneration::Record{id,found->second};};
 HNSWConfig config;config.dimensions=2;config.metric=DistanceMetric::DOT_PRODUCT;config.efSearch=200;config.maxElements=1000;config.maxMemoryBytes=8*1024*1024;
 auto journal=std::make_shared<VectorChangeJournal>(1024,1024*1024);auto anchor=journal->capture();
 auto manager=std::make_shared<VectorIndexGeneration>(1024*1024);
 assert(manager->enqueue(config,certificate,source));wait(manager,State::READY);
 manager->enableMaintenance(journal,anchor,certificate,resolve,7);
 const auto mutation=[&](const std::string& id,std::optional<std::vector<float>> value,uint64_t floor=0){
  std::lock_guard<std::mutex> lock(sourceMutex);if(value)records[id]=*value;else records.erase(id);
  assert(journal->append(id,floor));++revision;};
 mutation("0",std::vector<float>{999,0});mutation("99",{});mutation("new",std::vector<float>{1000,0});
 wait(manager,State::READY,2);auto current=manager->acquire(*certificate());assert(current);
 auto hits=current->graph->search({1,0},3);assert(hits[0].first=="new"&&hits[1].first=="0");assert(current->graph->size()==100);
 // Multiple bounded batches catch up without publishing a prefix as READY.
 for(int i=1;i<=50;++i)mutation(std::to_string(i),std::vector<float>{float(i+2000),0});
 auto final=certificate();auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(!(current=manager->acquire(*final))){assert(std::chrono::steady_clock::now()<deadline);assert(manager->status().state!=State::FAILED);std::this_thread::yield();}
 assert(current->graph->search({1,0},1).front().first=="50");
 // A pending visibility requirement cannot be consumed/announced as covered.
 mutation("pending",std::vector<float>{9000,0},10);
 std::this_thread::sleep_for(std::chrono::milliseconds(250));assert(!manager->acquire(*certificate()));
 {std::lock_guard<std::mutex> lock(sourceMutex);visibility=10;}
 final=certificate();deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(!(current=manager->acquire(*final))){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
 assert(current->graph->search({1,0},1).front().first=="pending");
 // History loss is a failed generation, never a successful approximate prefix.
 journal->gap();{std::lock_guard<std::mutex> lock(sourceMutex);++revision;}
 wait(manager,State::FAILED);assert(manager->status().error.find("history_gap")!=std::string::npos);assert(!manager->acquire(*certificate()));
 // A full rebuild remains usable and requires a new explicit anchor.
 anchor=journal->capture();assert(manager->enqueue(config,certificate,source));wait(manager,State::READY);
 manager->enableMaintenance(journal,anchor,certificate,resolve);
 {std::lock_guard<std::mutex> lock(sourceMutex);++root;}
 wait(manager,State::FAILED);assert(manager->status().error.find("requires_rebuild")!=std::string::npos);
 assert(manager->enqueue(config,certificate,source));wait(manager,State::READY);
 anchor=journal->capture();manager->enableMaintenance(journal,anchor,certificate,resolve);
 {std::lock_guard<std::mutex> lock(sourceMutex);throwCertificate=true;}
 wait(manager,State::FAILED);assert(manager->status().error=="certificate unavailable");
 manager->closeAndWait();assert(manager->finished());
 std::cout<<"INCREMENTAL_GENERATION_WORKER_COVERAGE_VISIBILITY_GAPS_FAILURE_REBUILD_PASS\n";
}
