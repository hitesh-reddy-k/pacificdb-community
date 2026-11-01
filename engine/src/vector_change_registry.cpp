#include "vector_change_registry.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
namespace pacificdb {
namespace {
struct Entry{std::string root,user,database,collection;std::weak_ptr<VectorChangeJournal> journal;};
using Entries=std::vector<Entry>;
std::shared_ptr<const Entries> entries=std::make_shared<const Entries>();
std::mutex writer;
std::atomic<bool> enabled{false};
auto budget=std::make_shared<VectorAllocationBudget>(64*1024*1024);
std::string canonicalRoot(const std::string& root){auto result=std::filesystem::path(root).lexically_normal();if(result.filename().empty()&&result!=result.root_path())result=result.parent_path();return result.string();}
bool match(const Entry& entry,const std::string& root,const std::string& user,const std::string& db,const std::string& collection){
 // LSM roots may have one trailing separator; avoid allocation on mutation paths.
 const bool sameRoot=entry.root==root||(!root.empty()&&(root.back()=='/'||root.back()=='\\')&&root.size()==entry.root.size()+1&&root.compare(0,entry.root.size(),entry.root)==0);
 return sameRoot&&entry.user==user&&entry.database==db&&entry.collection==collection;
}
}
std::shared_ptr<VectorChangeJournal> VectorChangeRegistry::subscribe(const std::string& root,const std::string& user,const std::string& db,const std::string& collection){
 std::lock_guard<std::mutex> lock(writer);auto previous=std::atomic_load(&entries);auto next=std::make_shared<Entries>();
 for(const auto& entry:*previous)if(auto journal=entry.journal.lock()){
  if(match(entry,root,user,db,collection))return journal;
  next->push_back(entry);
 }
 if(next->size()>=64)throw std::runtime_error("vector_index_history_subscription_limit");
 auto journal=std::make_shared<VectorChangeJournal>(4096,1024*1024,budget);
 next->push_back({canonicalRoot(root),user,db,collection,journal});
 std::atomic_store(&entries,std::shared_ptr<const Entries>(std::move(next)));enabled.store(true,std::memory_order_release);return journal;
}
void VectorChangeRegistry::observe(const std::string& root,const std::string& user,const std::string& db,const std::string& collection,const nlohmann::json& document) noexcept{
 if(!enabled.load(std::memory_order_acquire))return;
 const auto current=std::atomic_load(&entries);
 for(const auto& entry:*current)if(match(entry,root,user,db,collection))if(auto journal=entry.journal.lock()){
  try{
   const auto id=document.find("id");if(id==document.end()||!id->is_string()){journal->gap();continue;}
   uint64_t floor=0;
   for(const char* field:{"_raft_commit_index","commit_index","_visibility_floor"}){
    auto value=document.find(field);if(value==document.end())continue;
    if(!value->is_number_integer()||(!value->is_number_unsigned()&&value->get<int64_t>()<0)){journal->gap();return;}
    floor=std::max(floor,value->get<uint64_t>());
   }
   if((document.contains("committed")&&document.at("committed")==false)||
      document.value("_visibility_state",std::string())=="PENDING"){
    if(!floor){journal->gap();return;}
   }
   journal->append(id->get_ref<const std::string&>(),floor);
  }catch(...){journal->gap();}
 }
}
void VectorChangeRegistry::invalidate(const std::string& root,const std::string& user,const std::string& db,const std::string& collection) noexcept{
 if(!enabled.load(std::memory_order_acquire))return;
 const auto current=std::atomic_load(&entries);for(const auto& entry:*current)
  if(match(entry,root,user,db,collection))if(auto journal=entry.journal.lock())journal->reset();
}
void VectorChangeRegistry::invalidateAll() noexcept{
 if(!enabled.load(std::memory_order_acquire))return;
 const auto current=std::atomic_load(&entries);for(const auto& entry:*current)if(auto journal=entry.journal.lock())journal->reset();
}
}
