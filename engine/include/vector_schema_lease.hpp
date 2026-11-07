#pragma once
#include "vector_schema.hpp"
#include <array>
#include <shared_mutex>
#include <mutex>
#include <optional>

namespace pacificdb {
namespace schema_leases {
inline std::array<std::shared_mutex,4096> locks;
inline thread_local std::array<unsigned char,4096> held{};
inline thread_local unsigned applying=0;
}
// A committed apply must not wait on a lease held by its submitting client.
// Only trusted apply enters this scope; client validation remains outside it.
class VectorSchemaApplyScope {
public:
    VectorSchemaApplyScope(){++schema_leases::applying;}
    ~VectorSchemaApplyScope(){--schema_leases::applying;}
    VectorSchemaApplyScope(const VectorSchemaApplyScope&)=delete;
};
// Existing non-preview roots without schemas take the disabled fast path.
// Concurrent mutations share a database lease through consensus acknowledgement.
// DDL takes it exclusively before submission; apply never recursively takes it.
class VectorSchemaLease {
public:
    VectorSchemaLease(const std::string& user,const std::string& database,bool exclusive){
        if(schema_leases::applying||!VectorSchemaRegistry::leasesEnabled())return;
        index_=std::hash<std::string>{}(user+"/"+database)%schema_leases::locks.size();
        if(schema_leases::held[index_]){
            if(exclusive&&schema_leases::held[index_]!=2)
                throw std::runtime_error("vector_schema DDL cannot upgrade an active mutation lease");
            return;
        }
        if(exclusive)unique_.emplace(schema_leases::locks[index_]);
        else shared_.emplace(schema_leases::locks[index_]);
        schema_leases::held[index_]=exclusive?2:1;owned_=true;
    }
    ~VectorSchemaLease(){if(owned_)schema_leases::held[index_]=0;}
    VectorSchemaLease(const VectorSchemaLease&)=delete;
private:
    size_t index_{0};bool owned_{false};
    std::optional<std::unique_lock<std::shared_mutex>> unique_;
    std::optional<std::shared_lock<std::shared_mutex>> shared_;
};
}
