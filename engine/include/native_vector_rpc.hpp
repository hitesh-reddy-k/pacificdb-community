#pragma once
#include "vector_query_deadline.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace pacificdb::native_vector_rpc {
bool enabled();
bool annEnabled();
vector_query::Deadline deadline(const nlohmann::json& query, vector_query::Deadline parent = {});
std::vector<nlohmann::json> search(const std::string& user,const std::string& database,
    const std::string& collection,const nlohmann::json& query,nlohmann::json* execution = nullptr,
    vector_query::Deadline deadline = {});
}
