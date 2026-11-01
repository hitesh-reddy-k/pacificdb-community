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
int main(){
 std::mutex sourceMutex;std::map<std::string,std::vector<float>> records;
 uint64_t revision=1,visibility=0,root=1;bool throwCertificate=false;
 for(int i=0;i<100;++i)records[std::to_string(i)]={float(i),1};
 const auto certificate=[&]()->std::optional<VectorFreshness::Stamp>{std::lock_guard<std::mutex> lock(sourceMutex);
  if(throwCertificate)throw std::runtime_error("certificate unavailable");return VectorFreshness::Stamp{root,revision,visibility};};
 const auto source=[&](const VectorIndexGeneration::Emit& emit,const std::atomic<bool>& stop){
  std::lock_guard<std::mutex> lock(sourceMutex);for(const auto& [id,v]:records){if(stop)return;emit({id,v});}};
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
