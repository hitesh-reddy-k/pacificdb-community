#include "community_query.hpp"

#include "database_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace pacificdb::community {
namespace {

void validatePipeline(const json& pipeline) {
    if (!pipeline.is_array()) throw std::invalid_argument("pipeline must be an array");
    for (std::size_t i = 0; i < pipeline.size(); ++i) {
        const auto& stage = pipeline[i];
        if (!stage.is_object() || stage.size() != 1) {
            throw std::invalid_argument("each aggregation stage must have one operator");
        }
        const auto& name = stage.begin().key();
        const auto& value = stage.begin().value();
        if (name == "$match") {
            if (!value.is_object()) throw std::invalid_argument("$match must be an object");
        } else if (name == "$project") {
            if (!value.is_object() || value.empty())
                throw std::invalid_argument("$project must be a nonempty object");
            for (const auto& [_, include] : value.items()) {
                if (!(include == 1 || include == true))
                    throw std::invalid_argument("Community $project supports inclusion only");
            }
        } else if (name == "$sort") {
            if (!value.is_object() || value.empty())
                throw std::invalid_argument("$sort must be a nonempty object");
            for (const auto& [_, direction] : value.items()) {
                if (!direction.is_number_integer() ||
                    (direction.get<int>() != 1 && direction.get<int>() != -1))
                    throw std::invalid_argument("$sort directions must be 1 or -1");
            }
        } else if (name == "$skip") {
            if (!value.is_number_integer() || value.get<long long>() < 0)
                throw std::invalid_argument("$skip must be a nonnegative integer");
        } else if (name == "$limit") {
            if (!value.is_number_integer() || value.get<long long>() <= 0)
                throw std::invalid_argument("$limit must be a positive integer");
        } else if (name == "$count") {
            if (i + 1 != pipeline.size() || !value.is_string() || value.get<std::string>().empty())
                throw std::invalid_argument("$count must be terminal with a field name");
        } else {
            throw std::invalid_argument("unsupported Community aggregation stage: " + name);
        }
    }
}

int compareJson(const json& left, const json& right) {
    if (left == right) return 0;
    if (left.is_number() && right.is_number())
        return left.get<double>() < right.get<double>() ? -1 : 1;
    if (left.is_string() && right.is_string())
        return left.get_ref<const std::string&>() < right.get_ref<const std::string&>() ? -1 : 1;
    return left.dump() < right.dump() ? -1 : 1;
}

}  // namespace

json aggregateDocuments(json documents, const json& pipeline) {
    if (!documents.is_array()) throw std::invalid_argument("documents must be an array");
    validatePipeline(pipeline);
    json stages = json::array();

    for (const auto& stage : pipeline) {
        const std::string name = stage.begin().key();
        const json& value = stage.begin().value();
        const std::size_t inputCount = documents.size();
        if (name == "$match") {
            json filtered = json::array();
            for (auto& document : documents)
                if (DatabaseEngine::match(document, value)) filtered.push_back(std::move(document));
            documents = std::move(filtered);
        } else if (name == "$project") {
            for (auto& document : documents) {
                json projected = json::object();
                for (const auto& [field, _] : value.items())
                    if (document.contains(field)) projected[field] = document[field];
                document = std::move(projected);
            }
        } else if (name == "$sort") {
            std::stable_sort(documents.begin(), documents.end(), [&](const json& a, const json& b) {
                for (const auto& [field, direction] : value.items()) {
                    const json left = a.value(field, json());
                    const json right = b.value(field, json());
                    const int order = compareJson(left, right);
                    if (order != 0) return direction.get<int>() == 1 ? order < 0 : order > 0;
                }
                return false;
            });
        } else if (name == "$skip") {
            const auto count = std::min<std::size_t>(documents.size(), value.get<std::size_t>());
            documents.erase(documents.begin(), documents.begin() + static_cast<std::ptrdiff_t>(count));
        } else if (name == "$limit") {
            const auto count = value.get<std::size_t>();
            if (documents.size() > count) documents.erase(documents.begin() + static_cast<std::ptrdiff_t>(count), documents.end());
        } else if (name == "$count") {
            documents = json::array({{{value.get<std::string>(), inputCount}}});
        }
        stages.push_back({{"stage", name}, {"input_count", inputCount},
                          {"output_count", documents.size()}});
    }
    return {{"documents", documents}, {"stages", stages}};
}

json explainFind(const json& filter) {
    if (!filter.is_object()) throw std::invalid_argument("filter must be an object");
    for (auto it = filter.begin(); it != filter.end(); ++it) {
        if (!it.value().is_object() && !it.value().is_array()) {
            return {{"plan", "INDEX_LOOKUP"}, {"field", it.key()},
                    {"reason", "simple scalar equality predicate"}};
        }
    }
    return {{"plan", "FULL_SCAN"},
            {"reason", "no simple scalar equality predicate"}};
}

}  // namespace pacificdb::community
