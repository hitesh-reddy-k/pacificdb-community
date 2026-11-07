#include "vector_index_catalog.hpp"
#include "vector_schema.hpp"
#include "database_engine.hpp"
#include "community_catalog.hpp"
#include "lsm.hpp"
#include "storage_path.hpp"
#include "raft_core.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace pacificdb {
namespace {
constexpr const char* metadata = "pacificdb_meta";
constexpr const char* catalog = "vector_index_specs_v1";
uint64_t integer(const json& input, const char* key, uint64_t fallback,
                 uint64_t minimum, uint64_t maximum) {
    if (!input.contains(key)) return fallback;
    const auto& value = input.at(key);
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::invalid_argument(std::string("invalid vector index ") + key);
    const auto result = value.get<uint64_t>();
    if (result < minimum || result > maximum)
        throw std::invalid_argument(std::string("invalid vector index ") + key);
    return result;
}
std::string incarnation(const std::string& user, const std::string& database,
                        const std::string& collection) {
    // Current CREATE_COLLECTION apply generates independent local IDs. Binding a
    // leader's ID on followers would hide definitions or resurrect a dropped
    // collection's definition. Reject until replicated identity is implemented.
    if (RaftCore::instance().isEnabled() && RaftCore::instance().peerCount() != 0)
        throw std::runtime_error("vector_index_catalog_distributed_identity_unavailable");
    validateStorageIdentifier(user, "userId");
    validateStorageIdentifier(database, "databaseName");
    validateStorageIdentifier(collection, "collectionName");
    if (community::isReservedDatabase(database))
        throw std::invalid_argument("vector index target must be a user collection");
    const auto meta = DatabaseEngine::getDatabaseMetadata(user, database);
    for (const auto& entry : meta.value("collections", json::array())) {
        if (entry.is_object() && entry.value("name", "") == collection)
            return entry.at("id").get<std::string>();
    }
    throw std::invalid_argument("vector index target collection not found");
}
std::string identity(const std::string& database, const std::string& collection,
                     const std::string& incarnation, const std::string& name) {
    // Canonical JSON array is unambiguous; this is a record ID, not a file path.
    return json::array({database, collection, incarnation, name}).dump();
}
}
json VectorIndexCatalog::validate(const json& options) {
    static const std::set<std::string> keys{"name", "field", "dimensions", "metric",
        "M", "ef_construction", "ef_search", "max_elements", "max_source_bytes", "max_index_bytes", "max_build_ms",
        "vector_name", "max_scan_rows", "text_field", "max_lexical_bytes", "algorithm", "maintenance"};
    if (!options.is_object()) throw std::invalid_argument("vector index options must be an object");
    for (auto it = options.begin(); it != options.end(); ++it)
        if (!keys.count(it.key())) throw std::invalid_argument("unknown vector index option: " + it.key());
    if (!options.contains("dimensions")) throw std::invalid_argument("vector index dimensions required");
    const auto name = options.value("name", std::string("vector"));
    const auto field = options.value("field", std::string("vector"));
    validateStorageIdentifier(name, "vectorIndexName");
    validateStorageIdentifier(field, "vectorField");
    const auto vectorName = options.value("vector_name", std::string());
    const auto textField = options.value("text_field", std::string());
    if (!textField.empty()) validateStorageIdentifier(textField, "lexicalField");
    if (!vectorName.empty() && (vectorName.size() > 128 || vectorName.find('\0') != std::string::npos))
        throw std::invalid_argument("invalid vector index vector_name");
    auto metric = options.value("metric", std::string("cosine"));
    if (metric == "euclidean") metric = "l2";
    if (metric == "dot_product") metric = "dot";
    if (metric != "cosine" && metric != "l2" && metric != "dot")
        throw std::invalid_argument("unsupported vector index metric");
    const auto algorithm = options.value("algorithm",std::string("hnsw"));
    if (algorithm != "hnsw" && algorithm != "flat") throw std::invalid_argument("unsupported vector index algorithm");
    const auto maintenance=options.value("maintenance",std::string("manual"));
    if(maintenance!="manual"&&maintenance!="incremental")throw std::invalid_argument("unsupported vector index maintenance");
    if(maintenance=="incremental"&&(algorithm!="hnsw"||!textField.empty()))
        throw std::invalid_argument("incremental vector maintenance requires HNSW without lexical fields");
    const auto m = integer(options, "M", 16, 2, 128);
    const auto ef = integer(options, "ef_construction", 200, m, 100000);
    json canonical = {{"name", name}, {"field", field}, {"metric", metric}, {"M", m}, {"vector_name", vectorName},
        {"text_field", textField},
        {"max_lexical_bytes", integer(options, "max_lexical_bytes", 67108864, 1024, 1073741824)},
        {"ef_construction", ef}, {"ef_search", integer(options, "ef_search", 100, 1, 100000)},
        {"dimensions", integer(options, "dimensions", 0, 1, 65536)},
        {"max_elements", integer(options, "max_elements", 1000000, 1, 10000000)},
        {"max_source_bytes", integer(options, "max_source_bytes", 268435456, 1024, 1073741824)},
        {"max_index_bytes", integer(options, "max_index_bytes", 268435456, 1024, 1073741824)},
        {"max_build_ms", integer(options, "max_build_ms", 600000, 1, 3600000)},
        {"max_scan_rows", integer(options, "max_scan_rows", 1000000, 1, 10000000)}};
    // Preserve legacy HNSW definitions; only the new algorithm needs a new key.
    if (algorithm == "flat") canonical["algorithm"] = "flat";
    if(maintenance=="incremental")canonical["maintenance"]=maintenance;
    return canonical;
}
HNSWConfig VectorIndexCatalog::graphConfig(const json& canonical) {
    const auto spec = validate(canonical);
    HNSWConfig config;
    config.dimensions = spec.at("dimensions").get<size_t>();
    config.M = spec.at("M").get<size_t>();
    config.efConstruction = spec.at("ef_construction").get<size_t>();
    config.efSearch = spec.at("ef_search").get<size_t>();
    config.maxElements = spec.at("max_elements").get<size_t>();
    config.maxMemoryBytes = spec.at("max_index_bytes").get<size_t>();
    config.maxBuildMilliseconds = spec.at("max_build_ms").get<size_t>();
    const auto metric = spec.at("metric").get<std::string>();
    config.metric = metric == "l2" ? DistanceMetric::EUCLIDEAN :
                    metric == "dot" ? DistanceMetric::DOT_PRODUCT : DistanceMetric::COSINE;
    return config;
}
json VectorIndexCatalog::configure(const std::string& user, const std::string& database,
                                  const std::string& collection, const json& options) {
    const auto spec = validate(options);
    const auto token = incarnation(user, database, collection);
    const auto schema=VectorSchemaRegistry::get(DatabaseEngine::getDataRoot(),user,database,collection);
    if(spec.value("maintenance",std::string())=="incremental"&&!schema)
        throw std::invalid_argument("vector_schema required for incremental maintenance");
    if(schema)VectorSchema::index(*schema,spec);
    if (!DatabaseEngine::createDatabase(user, metadata, "binary") ||
        DatabaseEngine::createCollection(user, metadata, catalog).empty())
        throw std::runtime_error("cannot initialize vector index catalog");
    DatabaseEngine::insert(user, metadata, catalog,
        {{"id", identity(database, collection, token, spec.at("name").get<std::string>())},
         {"format_version", 1}, {"database", database}, {"collection", collection},
         {"collection_id", token}, {"spec", spec}});
    return spec;
}
std::vector<json> VectorIndexCatalog::list(const std::string& user, const std::string& database,
                                         const std::string& collection) {
    const auto token = incarnation(user, database, collection);
    std::vector<json> result;
    size_t scanned = 0;
    LSM::visitLatest(user, metadata, catalog, [&](const LSM::LatestRowView& row) {
        if (++scanned > 4096) throw std::runtime_error("vector catalog scan limit exceeded");
        const auto doc = row.materialize();
        if (doc.value("tenant_id", user) != user)
            throw std::runtime_error("vector catalog tenant mismatch");
        if (doc.value("database", "") != database || doc.value("collection", "") != collection ||
            doc.value("collection_id", "") != token) return true;
        if (doc.value("format_version", 0) != 1)
            throw std::runtime_error("unsupported vector catalog format");
        result.push_back(validate(doc.at("spec")));
        return true;
    });
    std::sort(result.begin(), result.end(), [](const json& a, const json& b) {
        return a.at("name").get<std::string>() < b.at("name").get<std::string>();
    });
    return result;
}
bool VectorIndexCatalog::erase(const std::string& user, const std::string& database,
                              const std::string& collection, const std::string& name) {
    validateStorageIdentifier(name, "vectorIndexName");
    const auto token = incarnation(user, database, collection);
    return DatabaseEngine::deleteOne(user, metadata, catalog,
        {{"id", identity(database, collection, token, name)}});
}
}
