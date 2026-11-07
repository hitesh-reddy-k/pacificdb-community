#pragma once
#include "vector_memory_budget.hpp"
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <mutex>
#include <string>
#include <vector>

namespace pacificdb {
// Derived mutation history for one collection incarnation. No vectors or WAL
// ownership. A missed/overwritten entry is an explicit coverage gap; callers must
// rebuild from committed storage. Only full coverage may certify an index READY.
class VectorChangeJournal {
public:
    struct Cursor {uint64_t epoch{0},sequence{0};};
    struct Change {uint64_t sequence;std::string id;uint64_t minimumVisibility;};
    enum class State { Covered, More, Gap, VisibilityBlocked };
    struct Batch {State state;Cursor next;std::vector<Change> changes;};
    VectorChangeJournal(size_t records,size_t bytes,
                        std::shared_ptr<VectorAllocationBudget> shared = {});
    Cursor capture() const;
    // Called inside the authoritative collection's existing mutation exclusion.
    // Allocation failure cannot fail a committed write: it records a coverage gap.
    bool append(const std::string& id,uint64_t minimumVisibility = 0) noexcept;
    void gap() noexcept;
    void reset() noexcept;
    Batch after(Cursor cursor,uint64_t visibility,size_t maximum = 256) const;
    size_t allocatedBytes() const noexcept {return memory_.accounting().used();}
private:
    struct Entry {
        Entry(uint64_t sequence,const std::string& id,uint64_t visibility,std::pmr::memory_resource* resource)
            :sequence(sequence),id(id,resource),minimumVisibility(visibility){}
        uint64_t sequence;std::pmr::string id;uint64_t minimumVisibility;
    };
    void gapLocked() noexcept;
    mutable std::mutex mutex_;
    VectorBudgetResource memory_;
    std::pmr::deque<Entry> entries_;
    size_t capacity_;
    uint64_t epoch_,sequence_{0},floor_{0};
    bool poisoned_{false};
    static uint64_t epoch() noexcept;
    static std::atomic<uint64_t> epochs_;
};
}
