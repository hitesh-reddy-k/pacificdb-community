#include "vector_index_generation.hpp"
#include "test_failpoint.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>
namespace pacificdb {
void VectorIndexGeneration::enableMaintenance(std::shared_ptr<VectorChangeJournal> journal,
        VectorChangeJournal::Cursor anchor,Certificate certificate,Resolve resolve,
        size_t maximumBatch,size_t maximumMilliseconds,bool sourceVisibilityComplete){
    if(!journal||!anchor.epoch||!certificate||!resolve||!maximumBatch||maximumBatch>1024||
       !maximumMilliseconds||maximumMilliseconds>60000)
        throw std::invalid_argument("invalid incremental vector maintenance configuration");
    auto config=std::make_shared<Maintenance>(Maintenance{std::move(journal),anchor,
        std::move(certificate),std::move(resolve),maximumBatch,maximumMilliseconds,sourceVisibilityComplete});
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopped_.load()||building_||!queue_.empty()||status_.state!=State::READY||!published_||
       !published_->graph||published_->lexical||published_->flat||maintenance_)
        throw std::runtime_error("vector incremental maintenance requires an idle READY HNSW-only generation");
    // Publish policy with the retained generation; never mutate a metadata
    // object already held by a reader. Graph and payload ownership are shared.
    if(sourceVisibilityComplete){
        auto complete=std::make_shared<Generation>(*published_);
        complete->visibilityComplete=true;
        published_=std::move(complete);
    }
    maintenance_=std::move(config);wake_.notify_one();
}
bool VectorIndexGeneration::maintain(){
    std::shared_ptr<Maintenance> maintenance;
    std::shared_ptr<const Generation> previous;
    std::shared_ptr<std::atomic<bool>> cancelled;
    uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopped_.load()||building_||!queue_.empty()||!maintenance_||!published_||
           status_.state==State::FAILED)return false;
        maintenance=maintenance_;previous=published_;revision=revision_;
    }
    // Callbacks and journal reads never execute under the publication mutex.
    std::optional<VectorFreshness::Stamp> target;
    try{target=maintenance->certificate();}
    catch(const std::exception& error){
        std::lock_guard<std::mutex> lock(mutex_);
        if(revision_==revision&&maintenance_==maintenance)status_={State::FAILED,status_.generation,error.what()};
        return false;
    }catch(...){
        std::lock_guard<std::mutex> lock(mutex_);
        if(revision_==revision&&maintenance_==maintenance)status_={State::FAILED,status_.generation,"incremental certificate callback failed"};
        return false;
    }
    if(!target)return false;
    if(previous->accepts(*target)){
        const auto head=maintenance->journal->capture();
        if(head.epoch==maintenance->cursor.epoch&&head.sequence==maintenance->cursor.sequence)return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopped_.load()||revision_!=revision||maintenance_!=maintenance||building_||!queue_.empty())return false;
        // Allocation failure is surfaced on the worker rather than terminating
        // the process or leaving building_ latched forever.
        try{cancelled=std::make_shared<std::atomic<bool>>(false);}
        catch(const std::exception& error){status_={State::FAILED,status_.generation,error.what()};return false;}
        building_=true;status_.state=State::STALE;activeCancellation_=cancelled;
    }
    std::string failure;
    bool advanced=false;
    try{
        if(target->root!=previous->certificate.root||target->configuration!=previous->certificate.configuration||
           target->collection<previous->certificate.collection||target->visibility<previous->certificate.visibility)
            throw std::runtime_error("vector_index_incremental_root_or_configuration_requires_rebuild");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(maintenance->maximumMilliseconds);
        const auto check=[&]{
            if(cancelled->load())throw std::runtime_error("vector incremental maintenance cancelled");
            if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("vector_index_incremental_timeout_requires_rebuild");
        };
        check();const auto batch=maintenance->journal->after(maintenance->cursor,target->visibility,maintenance->maximumBatch);
        if(batch.state==VectorChangeJournal::State::Gap)
            throw std::runtime_error("vector_index_incremental_history_gap_requires_rebuild");
        if(!batch.changes.empty()&&(*target==previous->certificate ||
           (previous->visibilityComplete && target->sameSourceAtOrAfter(previous->certificate))))
            throw std::runtime_error("vector_index_incremental_unversioned_changes_require_rebuild");
        // Resolve repeated IDs once per batch against the same authoritative
        // certificate. Stage complete records within the existing source bound.
        std::map<std::string,std::optional<Record>> resolved;size_t bytes=0;
        for(const auto& change:batch.changes){
            check();if(resolved.count(change.id))continue;
            auto record=maintenance->resolve(change.id);check();
            if(record&&(record->id!=change.id||record->text||!record->precise.empty()||record->vector.empty()))
                throw std::runtime_error("invalid incremental HNSW source record");
            const size_t values=record?record->vector.size():0;
            if(values>sourceByteBudget_/sizeof(float))throw std::runtime_error("vector incremental source memory budget exceeded");
            if(record&&record->packedRecord.size()>sourceByteBudget_)throw std::runtime_error("vector incremental source memory budget exceeded");
            const size_t charge=sizeof(Record)+128+change.id.size()+values*sizeof(float)+(record?record->packedRecord.size():0);
            if(charge>sourceByteBudget_||bytes>sourceByteBudget_-charge)
                throw std::runtime_error("vector incremental source memory budget exceeded");
            bytes+=charge;resolved.emplace(change.id,std::move(record));
        }
        const auto stillCurrent=maintenance->certificate();
        const auto compatible=[&](const std::optional<VectorFreshness::Stamp>& current){
            return current && (*current==*target || (maintenance->sourceVisibilityComplete &&
                current->sameSourceAtOrAfter(*target)));
        };
        if(!compatible(stillCurrent)){
            std::lock_guard<std::mutex> lock(mutex_);building_=false;return false;
        }
        // The original graph was constructed mutable. Search/insert/delete are
        // protected by its per-index shared mutex. Existing queries must still
        // revalidate authoritative certificates after fetching their payloads.
        test::hitFailpoint("FP_VECTOR_INCREMENTAL_BEFORE_GRAPH",batch.next.sequence);
        auto graph=std::const_pointer_cast<HNSWIndex>(previous->graph);
        for(const auto& item:resolved){
            check();
            if(item.second){
                if(!graph->upsert(item.first,item.second->vector,check,item.second->packedRecord))
                    throw std::runtime_error("vector_index_incremental_capacity_or_shape_requires_rebuild");
            }else graph->remove(item.first,check);
        }
        test::hitFailpoint("FP_VECTOR_INCREMENTAL_AFTER_GRAPH",batch.next.sequence);
        const auto final=maintenance->certificate();check();
        const auto head=maintenance->journal->capture();
        test::hitFailpoint("FP_VECTOR_INCREMENTAL_BEFORE_PUBLICATION",batch.next.sequence);
        std::shared_ptr<const Generation> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);building_=false;
            if(stopped_.load()||revision_!=revision||maintenance_!=maintenance)return false;
            maintenance->cursor=batch.next;
            const bool covered=batch.state==VectorChangeJournal::State::Covered&&compatible(final)&&
                head.epoch==batch.next.epoch&&head.sequence==batch.next.sequence;
            if(covered){
                auto next=std::make_shared<Generation>(Generation{nextGeneration_+1,*target,graph,{},{}});
                next->visibilityComplete=maintenance->sourceVisibilityComplete;
                retired.swap(published_);published_=std::move(next);++nextGeneration_;
                status_={State::READY,nextGeneration_,{}};
            }else status_.state=State::STALE;
            advanced=!batch.changes.empty();
        }
        test::hitFailpoint("FP_VECTOR_INCREMENTAL_AFTER_PUBLICATION",batch.next.sequence);
    }catch(const std::exception& error){failure=error.what();}
    catch(...){failure="unknown incremental vector maintenance failure";}
    if(!failure.empty()){
        std::lock_guard<std::mutex> lock(mutex_);building_=false;
        if(stopped_.load()||revision_!=revision)status_.state=State::STALE;
        else status_={State::FAILED,status_.generation,failure};
    }
    return advanced; // immediate next bounded batch; empty/blocked work sleeps
}
}
