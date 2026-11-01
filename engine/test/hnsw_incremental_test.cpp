#include "read_cache.hpp"
#include <cassert>
#include <iostream>
#include <random>
#include <map>
#include <set>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#ifndef _WIN32
#include <sys/resource.h>
#endif
using namespace pacificdb;
int main(int argc,char** argv){
 const size_t count=argc>1?std::stoull(argv[1]):10000;
 const size_t queries=argc>2?std::stoull(argv[2]):100;
 const std::string selected=argc>3?argv[3]:"all";
 if(count<10000||count>1000000||queries<10||queries>10000||
    (selected!="all"&&selected!="cosine"&&selected!="l2"&&selected!="dot"))throw std::invalid_argument("qualification bounds invalid");
 for(const auto metric:{DistanceMetric::COSINE,DistanceMetric::EUCLIDEAN,DistanceMetric::DOT_PRODUCT}){
  if(selected!="all"&&selected!=(metric==DistanceMetric::COSINE?"cosine":metric==DistanceMetric::EUCLIDEAN?"l2":"dot"))continue;
  const auto begin=std::chrono::steady_clock::now();
  HNSWConfig config;config.dimensions=8;config.M=16;config.efConstruction=160;config.efSearch=240;
  config.metric=metric;config.maxElements=count+std::max<size_t>(4000,count/5);config.maxMemoryBytes=1024ULL*1024*1024;
  HNSWIndex graph(config);std::mt19937 random(20261031);std::uniform_real_distribution<float> value(-1,1);
  std::map<std::string,std::vector<float>> records;
  auto vector=[&]{std::vector<float> result(8);for(auto& x:result)x=value(random);return result;};
  auto score=[&](const std::vector<float>& a,const std::vector<float>& b){double dot=0,aa=0,bb=0,l2=0;
   for(size_t i=0;i<a.size();++i){dot+=double(a[i])*b[i];aa+=double(a[i])*a[i];bb+=double(b[i])*b[i];l2+=(double(a[i])-b[i])*(double(a[i])-b[i]);}
   return metric==DistanceMetric::EUCLIDEAN?-std::sqrt(l2):metric==DistanceMetric::DOT_PRODUCT?dot:aa*bb==0?0.:dot/std::sqrt(aa*bb);};
  for(size_t i=0;i<count;++i){auto id=std::to_string(i);auto v=vector();assert(graph.insert(id,v));records[id]=v;if((i+1)%100000==0)std::cerr<<"build_rows="<<i+1<<std::endl;}
  const auto built=std::chrono::steady_clock::now();
  for(int i=0;i<1000;++i){auto id=std::to_string(i);auto v=vector();assert(graph.upsert(id,v));records[id]=v;}
  for(int i=1000;i<1500;++i){auto id=std::to_string(i);assert(graph.remove(id));assert(!graph.remove(id));records.erase(id);}
  for(size_t i=count;i<count+500;++i){auto id=std::to_string(i);auto v=vector();assert(graph.upsert(id,v));records[id]=v;}
  assert(graph.search(std::vector<float>(8,.5f),10,{},40).size()==10);
  bool invalidEf=false;try{graph.search(std::vector<float>(8,.5f),10,{},config.efSearch+1);}catch(const std::invalid_argument&){invalidEf=true;}assert(invalidEf);
  assert(graph.size()==records.size());assert(graph.getStats().at("tombstones")==1500);
  const auto updated=std::chrono::steady_clock::now();
  size_t matches=0;std::vector<double> latencies;
  for(size_t i=0;i<queries;++i){auto q=vector();std::vector<std::pair<double,std::string>> ranked;
   for(const auto& [id,v]:records)ranked.push_back({score(q,v),id});
   std::partial_sort(ranked.begin(),ranked.begin()+10,ranked.end(),[](const auto& a,const auto& b){return a.first>b.first||(a.first==b.first&&a.second<b.second);});
   const auto queryStart=std::chrono::steady_clock::now();auto result=graph.search(q,10);latencies.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-queryStart).count());assert(result.size()==10);
   std::set<std::string> unique;
   for(const auto& hit:result){assert(unique.insert(hit.first).second);assert(records.count(hit.first));
    for(size_t j=0;j<10;++j)if(ranked[j].second==hit.first){++matches;break;}}
  }
  const double recall=double(matches)/(queries*10);std::sort(latencies.begin(),latencies.end());
  json evidence={{"scope","internal HNSW core after mutations; not public API/release readiness"},{"rows",count},{"dimensions",8},
    {"queries",queries},{"metric",metric==DistanceMetric::COSINE?"cosine":metric==DistanceMetric::EUCLIDEAN?"l2":"dot"},
    {"recall_at_10",recall},{"status",recall>=.95?"PASS":"FAIL"},
    {"build_seconds",std::chrono::duration<double>(built-begin).count()},
    {"mutation_seconds",std::chrono::duration<double>(updated-built).count()},
    {"p50_ms",latencies[latencies.size()/2]},{"upserts",1500},{"deletes",500},{"graph",graph.getStats()}};
#ifndef _WIN32
  rusage usage{};getrusage(RUSAGE_SELF,&usage);evidence["peak_rss_kib"]=usage.ru_maxrss;
#endif
  std::cout<<"HNSW_INCREMENTAL_QUALIFICATION "<<evidence.dump()<<std::endl;assert(recall>=.95);
  std::atomic<bool> done{false};std::thread reader([&]{while(!done){auto hits=graph.search(std::vector<float>(8,.25f),10);assert(hits.size()==10);}});
  for(int i=0;i<100;++i)assert(graph.upsert(std::to_string(i),vector()));done=true;reader.join();
  if(count==10000){for(const auto& [id,v]:records)assert(graph.remove(id));assert(graph.size()==0);assert(graph.search(vector(),10).empty());
   assert(graph.upsert("revived",vector()));assert(graph.search(vector(),10).size()==1);}
 }
 HNSWConfig small;small.dimensions=2;small.maxElements=2;HNSWIndex limited(small);
 assert(limited.insert("a",{1,0}));assert(limited.upsert("a",{0,1}));assert(!limited.upsert("a",{1,0}));
 assert(limited.search({0,1},1).front().first=="a");assert(limited.getStats().at("physicalNodes")==2);
 bool cancelled=false;try{limited.remove("a",[]{throw std::runtime_error("cancel");});}catch(...){cancelled=true;}
 assert(cancelled&&limited.size()==1);assert(limited.remove("a"));assert(limited.search({1,0},2).empty());
 std::cout<<"HNSW_INCREMENTAL_METRICS_RECALL_TOMBSTONES_CAPACITY_CONCURRENCY_PASS\n";
}
