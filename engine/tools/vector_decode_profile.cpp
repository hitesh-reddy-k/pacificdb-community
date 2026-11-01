#include "storage_format_v2.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
using json=nlohmann::json;
using pacificdb::storage_v2::MsgpackRowView;
int main(){
    json doc={{"id","123456"},{"kind","vector"},{"version",42},{"_mvcc_version",42},
              {"_deleted",false},{"committed",true},{"_raft_commit_index",42},
              {"vector",std::vector<double>(25,.2)}};
    auto bytes=json::to_msgpack(doc);
    constexpr size_t repeats=300000;double checksum=0;
    const std::vector<double> query(25, 1.0);
    auto run=[&](bool projection){
      auto start=std::chrono::steady_clock::now();
      for(size_t i=0;i<repeats;++i){
#ifdef PACIFICDB_PROFILE_INDEXED
        MsgpackRowView row(bytes.data(),bytes.size(),true);
#else
        MsgpackRowView row(bytes.data(),bytes.size());
#endif
        if(projection){
#ifdef PACIFICDB_PROFILE_RAW
          auto selected=row.project({"id","tenant_id","_deleted","_visibility_state","committed",
              "_raft_commit_index","_visibility_floor","version","_mvcc_commit_ms"});
          checksum+=row.denseScore("vector",query,"dot",1.0).value();
#else
          auto selected=row.project({"id","tenant_id","_deleted","_visibility_state","committed",
              "_raft_commit_index","_visibility_floor","version","_mvcc_commit_ms","vector"});
          for(const auto& number:selected.at("vector"))checksum+=number.get<double>();
#endif
        }else{for(const char* name:{"id","_mvcc_version","_mvcc_version","_deleted","_visibility_state",
             "visibility_state","committed","_raft_commit_index","commit_index","_visibility_floor"})
           if(auto value=row.field(name))checksum+=value->is_number()?value->get<double>():1.;}
      }
      return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    };
    double lookups=run(false),projection=run(true);
    std::cout<<json{{"rows",repeats},{"dimensions",25},{"lookups_seconds",lookups},
      {"projection_seconds",projection},{"checksum",checksum},
      {"scope","decoder microprofile; not full storage benchmark"}}.dump()<<'\n';
}
