#include "community_query.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

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

    const json names = {{{"name", "Ada Lovelace"}}, {{"name", "Grace Hopper"}}};
    const auto partial = pacificdb::community::aggregateDocuments(
        names, json::array({{{"$match", {{"name", {{"$regex", "Lovelace"}}}}}}}));
    assert(partial.at("documents").size() == 1);
    const auto insensitive = pacificdb::community::aggregateDocuments(
        names, json::array({{{"$match", {{"name", {{"$regex", "ada lovelace"},
                                                     {"$options", "i"}}}}}}}));
    assert(insensitive.at("documents").size() == 1);

    const auto started = std::chrono::steady_clock::now();
    const json adversarial = {{{"value", std::string(4096, 'a') + "!"}}};
    const auto bounded = pacificdb::community::aggregateDocuments(
        adversarial,
        json::array({{{"$match", {{"value", {{"$regex", "(a+)+$"}}}}}}}));
    assert(bounded.at("documents").empty());
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));

    for (const json& expression : {
             json{{"$regex", R"((a)\1)"}},
             json{{"$regex", "a(?=b)"}},
             json{{"$regex", "a"}, {"$options", "m"}},
             json{{"$regex", std::string(4097, 'a')}}}) {
        bool invalidRegexRejected = false;
        try {
            (void)pacificdb::community::aggregateDocuments(
                names, json::array({{{"$match", {{"name", expression}}}}}));
        } catch (const std::invalid_argument&) {
            invalidRegexRejected = true;
        }
        assert(invalidRegexRejected);
    }

    std::cout << "COMMUNITY_QUERY_PASS\n";
    return 0;
}
