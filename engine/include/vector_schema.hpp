#pragma once
#include "vector_value.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace pacificdb {
using json = nlohmann::json;
// Immutable creation-time collection constraints. No independent record store:
// canonical schemas are persisted in the existing replicated db.meta lifecycle.
class VectorSchema {
public:
    static json canonical(const json& input);
    static bool metadataUsesSchemas(const json& metadata);
    static void record(const json& schema, const json& document);
    static void query(const json& schema, const std::string& name,
                      const vectors::Value& value, const std::string& metric);
    static void index(const json& schema, const json& definition);
};
class VectorSchemaRegistry {
public:
    static void load(const std::string& root);
    static bool leasesEnabled();
    static void install(const std::string& root,const std::string& user,const std::string& database,
                        const std::string& collection,const json& schema);
    static void erase(const std::string& root,const std::string& user,const std::string& database,
                      const std::string& collection);
    static std::shared_ptr<const json> get(const std::string& root,const std::string& user,
                                        const std::string& database,const std::string& collection);
    static void validateRecord(const std::string& root,const std::string& user,const std::string& database,
                               const std::string& collection,const json& document);
    static void persistMetadata(const std::string& root,const std::filesystem::path& path,const json& metadata,
                                const std::filesystem::path& marker = {});
};
}
