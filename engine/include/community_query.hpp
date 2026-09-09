#pragma once

#include <nlohmann/json.hpp>

namespace pacificdb::community {

using json = nlohmann::json;

json aggregateDocuments(json documents, const json& pipeline);
json explainFind(const json& filter);

}  // namespace pacificdb::community
