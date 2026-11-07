#include "flat_vector_index.hpp"
#include <cassert>
#include <random>
#include <iostream>
#include <thread>
#include <limits>
using namespace pacificdb;
template<class F>void rejects(F fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}assert(rejected);}
int main(){
    for(const std::string metric:{"cosine","l2","dot"}){
        FlatVectorConfig config;config.dimensions=25;config.maxElements=10000;config.metric=metric;
        FlatVectorIndex index(config);index.reserve(10000);
        std::mt19937 random(1805);std::uniform_real_distribution<double> value(-1,1);
        std::vector<std::vector<double>> vectors(10000,std::vector<double>(25));
        for(size_t i=0;i<vectors.size();++i){for(auto& x:vectors[i])x=value(random);assert(index.append(std::to_string(i),vectors[i]));}
        rejects([&]{index.search(vectors[0],10);});
        index.freeze();rejects([&]{index.append("new",vectors[0]);});
        for(int request=0;request<20;++request){
            std::vector<double> query(25);for(auto& x:query)x=value(random);
            double qn=0;for(double x:query)qn+=x*x;
            std::vector<std::pair<double,std::string>> expected;
            for(size_t i=0;i<vectors.size();++i){
                double dot=0,norm=0,square=0;
                for(size_t d=0;d<query.size();++d){dot+=query[d]*vectors[i][d];norm+=vectors[i][d]*vectors[i][d];const double diff=query[d]-vectors[i][d];square+=diff*diff;}
                expected.emplace_back(metric=="dot"?dot:metric=="l2"?-std::sqrt(square):dot/(std::sqrt(qn)*std::sqrt(norm)),std::to_string(i));
            }
            std::sort(expected.begin(),expected.end(),[](const auto& a,const auto& b){return a.first>b.first||(a.first==b.first&&a.second<b.second);});
            auto hits=index.search(query,10);assert(hits.size()==10);
            for(size_t i=0;i<hits.size();++i){assert(hits[i].first==expected[i].second);assert(std::abs(hits[i].second-expected[i].first)<1e-12);}
        }
        std::vector<std::thread> readers;
        for(int worker=0;worker<4;++worker)readers.emplace_back([&]{for(int i=0;i<20;++i)assert(index.search(vectors[0],10).size()==10);});
        for(auto& reader:readers)reader.join();
        int visited=0;rejects([&]{index.search(vectors[0],10,[&](size_t count){visited=static_cast<int>(count);if(visited==50)throw std::runtime_error("cancelled");});});
        assert(index.stats().at("healthy")==true);assert(index.search(vectors[0],10).size()==10);
        size_t callbacks=0,visitedRows=0;
        auto batched=index.search(vectors[0],10,[&](size_t count){assert(count>=visitedRows);visitedRows=count;++callbacks;},128);
        assert(batched==index.search(vectors[0],10));assert(visitedRows==10000&&callbacks<100);
        rejects([&]{index.search(vectors[0],10,{},0);});
        rejects([&]{index.search({1.},10);});rejects([&]{index.search(vectors[0],0);});rejects([&]{index.search(vectors[0],10001);});
        auto invalid=vectors[0];invalid[0]=std::numeric_limits<double>::infinity();rejects([&]{index.search(invalid,10);});
    }
    FlatVectorConfig config;config.dimensions=1;config.metric="dot";config.maxElements=10;
    FlatVectorIndex precise(config);
    assert(precise.append("z",{1.+1e-14}));assert(precise.append("a",{1.}));assert(!precise.append("a",{2.}));
    assert(!precise.append("",{2.}));assert(!precise.append("invalid",{1.,2.}));
    precise.freeze();assert(precise.matches("z",{1.+1e-14}));assert(!precise.matches("z",{1.}));
    assert(!precise.matches("absent",{1.}));assert(precise.search({1.},10).front().first=="z");
    assert(precise.search({0.},10).front().first=="a");
    FlatVectorIndex overflow(config);assert(overflow.append("big",{1e308}));overflow.freeze();
    rejects([&]{overflow.search({1e308},1);});assert(overflow.stats().at("healthy")==true);
    FlatVectorConfig hugeConfig;hugeConfig.dimensions=2;hugeConfig.metric="cosine";
    FlatVectorIndex hugeCosine(hugeConfig);assert(hugeCosine.append("large",{1e300,1e300}));hugeCosine.freeze();
    assert(std::abs(hugeCosine.search({1.,0.},1).front().second-std::sqrt(.5))<1e-14);
    assert(std::abs(hugeCosine.search({1e300,0.},1).front().second-std::sqrt(.5))<1e-14);
    hugeConfig.metric="l2";FlatVectorIndex hugeL2(hugeConfig);assert(hugeL2.append("large",{1e200,0.}));hugeL2.freeze();
    assert(hugeL2.search({0.,0.},1).front().second==-1e200);
    auto budget=std::make_shared<VectorAllocationBudget>(1024*1024);
    {
        FlatVectorIndex limited(config,budget);assert(limited.append("a",{1.}));limited.freeze();
        auto available=budget->limit()-budget->used();assert(budget->reserve(available));
        rejects([&]{limited.search({1.},10);});assert(limited.stats().at("healthy")==true);
        budget->release(available);assert(limited.search({1.},10).size()==1);
    }
    assert(budget->used()==0);
    FlatVectorConfig small=config;small.dimensions=25;small.maxMemoryBytes=1024;
    FlatVectorIndex failed(small);rejects([&]{failed.reserve(10);});
    assert(failed.stats().at("healthy")==false);rejects([&]{failed.freeze();});
    std::cout<<"FLAT_VECTOR_INDEX_FLOAT64_REFERENCE_MEMORY_CONCURRENCY_PASS\n";
}
