#include "community_query.hpp"

#include <cassert>
#include <iostream>

using json = nlohmann::json;

int main() {
    const json docs = {{{"id", "1"}, {"team", "a"}, {"score", 2}},
                       {{"id", "2"}, {"team", "b"}, {"score", 1}}};
    const auto out = pacificdb::community::aggregateDocuments(
        docs, json::array({{{"$match", {{"team", "a"}}}},
                           {{"$project", {{"score", 1}}}},
                           {{"$sort", {{"score", -1}}}},
                           {{"$limit", 1}}}));
    assert(out.at("documents").size() == 1);
    assert((out.at("documents")[0] == json{{"score", 2}}));

    bool rejected = false;
    try {
        pacificdb::community::aggregateDocuments(
            docs, json::array({{{"$group", json::object()}}}));
    } catch (...) {
        rejected = true;
    }
    assert(rejected);

    assert(pacificdb::community::explainFind({{"name", "Ada"}})
               .at("plan") == "INDEX_LOOKUP");
    assert(pacificdb::community::explainFind({{"score", {{"$gt", 1}}}})
               .at("plan") == "FULL_SCAN");

    std::cout << "COMMUNITY_QUERY_PASS\n";
    return 0;
}
