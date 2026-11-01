#pragma once
#include "read_cache.hpp"

namespace pacificdb {
// Trusted engine-internal catalog. Router authorization is required before use.
// Definitions are ordinary tenant-owned WAL/LSM/Raft documents, not CREATE_INDEX
// commands; existing B-tree apply semantics are unchanged.
class VectorIndexCatalog {
public:
    static json validate(const json& options);
    static HNSWConfig graphConfig(const json& canonical);
    static json configure(const std::string& user, const std::string& database,
                          const std::string& collection, const json& options);
    static std::vector<json> list(const std::string& user, const std::string& database,
                                  const std::string& collection);
    static bool erase(const std::string& user, const std::string& database,
                      const std::string& collection, const std::string& name);
};
}
