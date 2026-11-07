#include "vector_change_journal.hpp"
#include <limits>
#include <stdexcept>
namespace pacificdb {
std::atomic<uint64_t> VectorChangeJournal::epochs_{1};
uint64_t VectorChangeJournal::epoch() noexcept {
    auto next=epochs_.load();
    while(next!=std::numeric_limits<uint64_t>::max())
        if(epochs_.compare_exchange_weak(next,next+1))return next;
    return 0; // permanent exhaustion, never wrap into a previously valid epoch
}
VectorChangeJournal::VectorChangeJournal(size_t records,size_t bytes,std::shared_ptr<VectorAllocationBudget> shared)
    :memory_(bytes,std::move(shared)),entries_(&memory_),capacity_(records),epoch_(epoch()){
    if(!records||records>65536||bytes<1024||bytes>64*1024*1024)throw std::invalid_argument("vector journal invalid capacity");
    if(!epoch_||epoch_==std::numeric_limits<uint64_t>::max())throw std::runtime_error("vector journal epoch exhausted");
}
VectorChangeJournal::Cursor VectorChangeJournal::capture() const{
    std::lock_guard<std::mutex> lock(mutex_);return poisoned_?Cursor{}:Cursor{epoch_,sequence_};
}
void VectorChangeJournal::gapLocked() noexcept{
    floor_=sequence_;
    // Bounded metadata destruction; never frees a graph or authoritative data.
    entries_.clear();
}
bool VectorChangeJournal::append(const std::string& id,uint64_t visibility) noexcept{
    std::lock_guard<std::mutex> lock(mutex_);
    if(poisoned_)return false;
    if(sequence_==std::numeric_limits<uint64_t>::max()){poisoned_=true;gapLocked();return false;}
    ++sequence_;
    if(id.empty()||id.size()>1024){gapLocked();return false;}
    if(entries_.size()==capacity_){floor_=entries_.front().sequence;entries_.pop_front();}
    try{entries_.emplace_back(sequence_,id,visibility,&memory_);return true;}
    catch(...){gapLocked();return false;}
}
void VectorChangeJournal::gap() noexcept{
    std::lock_guard<std::mutex> lock(mutex_);
    if(sequence_==std::numeric_limits<uint64_t>::max())poisoned_=true;else ++sequence_;
    gapLocked();
}
void VectorChangeJournal::reset() noexcept{
    std::lock_guard<std::mutex> lock(mutex_);epoch_=epoch();
    poisoned_=!epoch_||epoch_==std::numeric_limits<uint64_t>::max();sequence_=0;gapLocked();
}
VectorChangeJournal::Batch VectorChangeJournal::after(Cursor cursor,uint64_t visibility,size_t maximum) const{
    if(!maximum||maximum>4096)throw std::invalid_argument("vector journal invalid batch bound");
    std::lock_guard<std::mutex> lock(mutex_);
    if(poisoned_||cursor.epoch!=epoch_||cursor.sequence<floor_||cursor.sequence>sequence_)
        return {State::Gap,cursor,{}};
    Batch result{State::Covered,cursor,{}};
    result.changes.reserve(std::min(maximum,entries_.size()));
    for(const auto& entry:entries_){
        if(entry.sequence<=cursor.sequence)continue;
        if(entry.minimumVisibility>visibility){result.state=State::VisibilityBlocked;return result;}
        if(result.changes.size()==maximum){result.state=State::More;return result;}
        result.changes.push_back({entry.sequence,std::string(entry.id),entry.minimumVisibility});
        result.next={epoch_,entry.sequence};
    }
    // Gaps cannot be hidden by an empty deque or a successful partial prefix.
    if(result.next.sequence!=sequence_)result.state=State::Gap;
    return result;
}
}
