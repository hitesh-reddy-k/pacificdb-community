#include "database_engine.hpp"
#include "lsm.hpp"
#include "owned_test_root.hpp"
#include "query_limiter.hpp"
#include "wal.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <thread>
#ifdef __linux__
#include <sys/resource.h>
#endif

using Clock=std::chrono::steady_clock;
int main(int argc,char** argv) try {
    const size_t count=argc>1?std::stoull(argv[1]):10000;
    const size_t queries=argc>2?std::stoull(argv[2]):100;
    constexpr size_t dimensions=25;
    if(count<10||count>1000000||!queries||queries>10000)throw std::invalid_argument("qualification bounds invalid");
#ifndef _WIN32
    setenv("WAL_ENABLED","true",1);setenv("WAL_FSYNC_ENABLED","true",1);
    setenv("WAL_FLUSH_ON_COMMIT","true",1);setenv("WAL_GROUP_COMMIT","false",1);
#endif
    OwnedTestRoot root("native-vector-qualification");DatabaseEngine::init(root.dataRoot().string());
    LSM::startBackgroundTasks();
    struct Stop{~Stop(){DatabaseEngine::stopNativeVectorIndexes();LSM::stopBackgroundTasks();}} stop;
    DatabaseEngine::ensureUserRoot("bench");DatabaseEngine::createDatabase("bench","app","vector");
    DatabaseEngine::createCollection("bench","app","vectors");
    QueryLimiter::setMaxScanRows(count+1000);
    std::mt19937 rng(1701);std::uniform_real_distribution<double> random(-1,1);
    std::vector<std::vector<double>> records(count,std::vector<double>(dimensions));
    const auto loadStart=Clock::now();
    for(size_t first=0;first<count;first+=500){
        std::vector<json> batch;
        for(size_t i=first;i<std::min(first+500,count);++i){
            double norm=0;for(auto& number:records[i]){number=random(rng);norm+=number*number;}
            for(auto& number:records[i])number=static_cast<float>(number/std::sqrt(norm));
            batch.push_back({{"id",std::to_string(i)},{"vector",records[i]},{"payload","authoritative"}});
        }
        DatabaseEngine::insertMany("bench","app","vectors",std::move(batch));
    }
    const double loadSeconds=std::chrono::duration<double>(Clock::now()-loadStart).count();
    LSM::flush("bench","app","vectors");
    if(DatabaseEngine::count("bench","app","vectors")!=count)throw std::runtime_error("load count mismatch");
    const auto buildStart=Clock::now();
    DatabaseEngine::buildNativeVectorIndex("bench","app","vectors",{{"name","native"},{"dimensions",dimensions},
        {"metric","cosine"},{"M",16},{"ef_construction",200},{"ef_search",300},{"max_elements",count},
        {"max_scan_rows",count+1000},{"max_source_bytes",512ULL*1024*1024},
        {"max_index_bytes",1024ULL*1024*1024},{"max_build_ms",3600000}});
    json status;
    for(;;){
        status=DatabaseEngine::nativeVectorIndexStatus("bench","app","vectors","native");
        if(status.at("state")=="READY")break;
        if(status.at("state")=="FAILED")throw std::runtime_error(status.dump());
        if(Clock::now()-buildStart>std::chrono::hours(1))throw std::runtime_error("qualification build deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const double buildSeconds=std::chrono::duration<double>(Clock::now()-buildStart).count();
    std::cerr<<"native_ready rows="<<count<<" build_seconds="<<buildSeconds<<'\n';
    size_t matches=0;std::vector<double> latency;
    for(size_t q=0;q<queries;++q){
        std::vector<double> query(dimensions);double qq=0;
        for(auto& number:query){number=random(rng);qq+=number*number;}
        std::vector<std::pair<double,size_t>> truth;truth.reserve(count);
        for(size_t i=0;i<count;++i){
            double dot=0,length=0;for(size_t d=0;d<dimensions;++d){dot+=query[d]*records[i][d];length+=records[i][d]*records[i][d];}
            truth.emplace_back(-dot/std::sqrt(qq*length),i);
        }
        std::partial_sort(truth.begin(),truth.begin()+10,truth.end());
        const auto start=Clock::now();json execution;
        const auto hits=DatabaseEngine::queryNativeVectorIndex("bench","app","vectors","native",
            {{"vector",query},{"metric","cosine"},{"k",10}},&execution);
        latency.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
        if(hits.size()!=10||execution.at("executed_mode")!="ann")throw std::runtime_error("ANN response incomplete");
        std::set<size_t> ids;
        for(const auto& hit:hits){
            const auto id=std::stoull(hit.at("id").get<std::string>());
            if(id>=count||!ids.insert(id).second||hit.at("payload")!="authoritative"||hit.at("tenant_id")!="bench")
                throw std::runtime_error("candidate payload/tenant/ID invalid");
            for(size_t k=0;k<10;++k)if(truth[k].second==id){
                ++matches;
                if(std::abs(hit.at("score").get<double>()+truth[k].first)>1e-12)
                    throw std::runtime_error("authoritative score mismatch");
            }
        }
    }
    std::sort(latency.begin(),latency.end());const auto recall=double(matches)/(10*queries);
    uint64_t disk=0;for(const auto& entry:std::filesystem::recursive_directory_iterator(root.dataRoot()))
        if(entry.is_regular_file())disk+=entry.file_size();
    json result{{"status",recall>=.95?"PASS":"FAIL"},{"scope","trusted native C++ API; not public/distributed readiness"},
        {"rows",count},{"dimensions",dimensions},{"metric","cosine"},{"queries",queries},
        {"recall_at_10",recall},{"p50_ms",latency[queries/2]},
        {"p95_ms",queries>=1000?json(latency[size_t(std::ceil(.95*queries))-1]):json()},
        {"p99_ms",queries>=1000?json(latency[size_t(std::ceil(.99*queries))-1]):json()},
        {"load_seconds",loadSeconds},{"source_and_build_seconds",buildSeconds},
        {"storage_bytes",disk},{"wal_physical_syncs",WAL::getStats().physicalSyncs.load()},
        {"wal_bytes",WAL::getStats().bytesWritten.load()}, {"index",status},
        {"durability",{{"wal",true},{"fsync",true},{"flush_on_commit",true},{"group_commit",false}}}};
#ifdef __linux__
    rusage usage{};if(getrusage(RUSAGE_SELF,&usage)==0)result["process_peak_rss_kib"]=usage.ru_maxrss;
#endif
    std::cout<<"NATIVE_QUALIFICATION_JSON "<<result.dump()<<'\n';return recall>=.95?0:1;
}catch(const std::exception& e){std::cerr<<"NATIVE_QUALIFICATION_FAIL "<<e.what()<<'\n';return 1;}
