#include "vector_change_journal.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
using namespace pacificdb;
using State=VectorChangeJournal::State;
int main(){
 auto budget=std::make_shared<VectorAllocationBudget>(1024*1024);
 {
 VectorChangeJournal journal(3,65536,budget);const auto start=journal.capture();
 assert(journal.after(start,0).state==State::Covered);
 assert(journal.append("first",1));assert(journal.append("second",2));
 auto blocked=journal.after(start,1);assert(blocked.state==State::VisibilityBlocked);assert(blocked.changes.size()==1);
 auto first=journal.after(start,2,1);assert(first.state==State::More&&first.changes.front().id=="first");
 auto second=journal.after(first.next,2);assert(second.state==State::Covered&&second.changes.front().id=="second");
 assert(journal.append("third",3));assert(journal.append("fourth",4));assert(journal.after(start,4).state==State::Gap);
 assert(journal.after(first.next,4).state==State::Covered);
 const auto beforeGap=journal.capture();journal.gap();assert(journal.after(beforeGap,99).state==State::Gap);
 const auto rebuilt=journal.capture();assert(journal.after(rebuilt,99).state==State::Covered);
 assert(journal.append("after-gap"));assert(journal.after(rebuilt,99).changes.front().id=="after-gap");
 const auto old=journal.capture();journal.reset();assert(journal.after(old,99).state==State::Gap);
 const auto current=journal.capture();assert(current.epoch!=old.epoch);assert(journal.after(current,99).state==State::Covered);
 assert(!journal.append(std::string(1025,'x')));assert(journal.after(current,99).state==State::Gap);
 VectorChangeJournal small(100,2048,budget);auto begin=small.capture();bool overflow=false;
 for(int i=0;i<100;++i)if(!small.append(std::string(1000,'a'))){overflow=true;break;}
 assert(overflow&&small.after(begin,0).state==State::Gap);assert(small.allocatedBytes()<=2048);
 const auto reanchor=small.capture();assert(small.after(reanchor,0).state==State::Covered);
 }
 assert(budget->used()==0);
 VectorChangeJournal parallel(65536,16*1024*1024);const auto first=parallel.capture();
 std::atomic<bool> done{false};std::thread reader([&]{while(!done){const auto cursor=parallel.capture();
  const auto batch=parallel.after(cursor,0);assert(batch.state==State::Covered||batch.state==State::More);
 }});
 std::vector<std::thread> writers;for(int t=0;t<4;++t)writers.emplace_back([&,t]{for(int i=0;i<1000;++i)assert(parallel.append(std::to_string(t)+"/"+std::to_string(i)));});
 for(auto& thread:writers)thread.join();done=true;reader.join();
 auto cursor=first;size_t count=0;for(;;){auto batch=parallel.after(cursor,0,113);assert(batch.state==State::More||batch.state==State::Covered);
  for(const auto& change:batch.changes){assert(change.sequence==++count);assert(change.id.size()<12);}cursor=batch.next;if(batch.state==State::Covered)break;}
 assert(count==4000);assert(parallel.allocatedBytes()<=16*1024*1024);
 std::cout<<"VECTOR_CHANGE_JOURNAL_COVERAGE_GAPS_VISIBILITY_MEMORY_CONCURRENCY_PASS\n";
}
