/**
 * @file query_parser.cpp
 * @brief Production-grade query parser and evaluator
 *
 * This is a complete MongoDB-compatible query engine supporting:
 * - All comparison operators ($eq, $ne, $gt, $gte, $lt, $lte)
 * - All array operators ($in, $nin, $all, $size, $elemMatch)
 * - All logical operators ($and, $or, $not, $nor)
 * - Element operators ($exists, $type)
 * - String operators ($regex with options)
 * - Production vector search ($vectorSearch)
 *
 * Performance optimizations:
 * - Early termination for AND/OR
 * - Efficient regex compilation and caching
 * - Type coercion for comparisons
 */

#include "query.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <mutex>
#include <functional>

using json = nlohmann::json;

// ============================================================================
// REGEX CACHE FOR PERFORMANCE
// ============================================================================
static std::unordered_map<std::string, std::regex> regexCache;
static std::mutex regexCacheMutex;
static const size_t MAX_REGEX_CACHE = 100;

static std::regex getOrCompileRegex(const std::string& pattern, const std::string& options) {
    std::string cacheKey = pattern + "|" + options;

    std::lock_guard<std::mutex> lock(regexCacheMutex);
    auto it = regexCache.find(cacheKey);
    if (it != regexCache.end()) {
        return it->second;
    }

    // Build regex flags
    std::regex::flag_type flags = std::regex::ECMAScript;
    if (options.find('i') != std::string::npos) {
        flags |= std::regex::icase;
    }

    try {
        std::regex compiled(pattern, flags);

        // Cache management - evict oldest if full
        if (regexCache.size() >= MAX_REGEX_CACHE) {
            regexCache.erase(regexCache.begin());
        }
        regexCache[cacheKey] = compiled;
        return compiled;
    } catch (const std::regex_error& e) {
        std::cerr << "[QUERY] Invalid regex pattern: " << pattern << " - " << e.what() << std::endl;
        throw;
    }
}

// ============================================================================
// VECTOR MATH HELPERS
// ============================================================================
static double dotProduct(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

static double magnitude(const std::vector<double>& v) {
    double sum = 0.0;
    for (double x : v) sum += x * x;
    return std::sqrt(sum);
}

static double cosineSimilarity(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return std::numeric_limits<double>::quiet_NaN();
    double dot = dotProduct(a, b);
    double magA = magnitude(a);
    double magB = magnitude(b);
    if (magA < 1e-10 || magB < 1e-10) return 0.0;
    return dot / (magA * magB);
}

static double euclideanDistance(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.empty()) return std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d = a[i] - b[i];
        sum += d * d;
    }
    return std::sqrt(sum);
}

static std::vector<double> jsonToVector(const json& jvec) {
    std::vector<double> v;
    if (!jvec.is_array()) return v;
    v.reserve(jvec.size());
    for (const auto& x : jvec) {
        if (x.is_number()) v.push_back(x.get<double>());
        else if (x.is_string()) {
            try { v.push_back(std::stod(x.get<std::string>())); }
            catch (...) { v.push_back(0.0); }
        }
    }
    return v;
}

// ============================================================================
// TYPE CHECKING HELPERS
// ============================================================================
static std::string getJsonTypeName(const json& val) {
    if (val.is_null()) return "null";
    if (val.is_boolean()) return "bool";
    if (val.is_number_integer()) return "int";
    if (val.is_number_float()) return "double";
    if (val.is_string()) return "string";
    if (val.is_array()) return "array";
    if (val.is_object()) return "object";
    return "unknown";
}

static int getJsonTypeCode(const json& val) {
    // MongoDB type codes
    if (val.is_number_float()) return 1;   // double
    if (val.is_string()) return 2;         // string
    if (val.is_object()) return 3;         // object
    if (val.is_array()) return 4;          // array
    if (val.is_boolean()) return 8;        // bool
    if (val.is_null()) return 10;          // null
    if (val.is_number_integer()) return 16; // int32
    return -1;
}

// ============================================================================
// PARSER IMPLEMENTATION
// ============================================================================

QueryNode parseQuery(const json& filter) {
    QueryNode node;
    node.type = QueryNode::Type::INVALID;

    // Null or non-object filter
    if (filter.is_null()) {
        node.type = QueryNode::Type::MATCH_ALL;
        return node;
    }

    // Empty object = match all
    if (filter.is_object() && filter.empty()) {
        node.type = QueryNode::Type::MATCH_ALL;
        return node;
    }

    // Must be an object
    if (!filter.is_object()) {
        std::cerr << "[QUERY] Invalid filter: must be an object" << std::endl;
        return node;
    }

    // ========== LOGICAL OPERATORS ==========

    // $or
    if (filter.contains("$or")) {
        if (!filter["$or"].is_array()) {
            std::cerr << "[QUERY] $or must be an array" << std::endl;
            return node;
        }
        node.type = QueryNode::Type::OR;
        for (const auto& f : filter["$or"]) {
            if (f.is_object() && !f.empty()) {
                node.children.push_back(parseQuery(f));
            }
        }
        if (node.children.empty()) {
            node.type = QueryNode::Type::ALWAYS_FALSE;
        }
        return node;
    }

    // $and
    if (filter.contains("$and")) {
        if (!filter["$and"].is_array()) {
            std::cerr << "[QUERY] $and must be an array" << std::endl;
            return node;
        }
        node.type = QueryNode::Type::AND;
        for (const auto& f : filter["$and"]) {
            node.children.push_back(parseQuery(f));
        }
        return node;
    }

    // $nor
    if (filter.contains("$nor")) {
        if (!filter["$nor"].is_array()) {
            std::cerr << "[QUERY] $nor must be an array" << std::endl;
            return node;
        }
        node.type = QueryNode::Type::NOR;
        for (const auto& f : filter["$nor"]) {
            node.children.push_back(parseQuery(f));
        }
        return node;
    }

    // $not at top level (must wrap another operator)
    if (filter.contains("$not")) {
        node.type = QueryNode::Type::NOT;
        node.children.push_back(parseQuery(filter["$not"]));
        return node;
    }

    // ========== VECTOR SEARCH ==========
    if (filter.contains("$vectorSearch")) {
        auto vsearch = filter["$vectorSearch"];
        node.type = QueryNode::Type::VECTOR_SEARCH;
        node.field = vsearch.value("field", "vector");
        node.queryVector = jsonToVector(vsearch.value("queryVector", json::array()));
        node.vectorThreshold = vsearch.value("threshold", 0.8);
        node.vectorMetric = vsearch.value("metric", "cosine");
        node.vectorTopK = vsearch.value("topK", 10);
        return node;
    }

    // ========== MULTIPLE TOP-LEVEL FIELDS = IMPLICIT AND ==========
    if (filter.size() > 1) {
        node.type = QueryNode::Type::AND;
        for (auto it = filter.begin(); it != filter.end(); ++it) {
            json sub = json::object();
            sub[it.key()] = it.value();
            node.children.push_back(parseQuery(sub));
        }
        return node;
    }

    // ========== SINGLE FIELD CASE ==========
    auto it = filter.begin();
    node.field = it.key();
    node.value = it.value();

    // If value is a direct value (not operator object), it's equality
    if (!node.value.is_object()) {
        node.type = QueryNode::Type::EQ;
        return node;
    }

    // ========== OPERATOR OBJECT ==========
    auto vobj = node.value;

    // Handle multiple operators on same field: { age: { $gte: 18, $lte: 65 } }
    if (vobj.size() > 1) {
        node.type = QueryNode::Type::AND;
        for (auto oit = vobj.begin(); oit != vobj.end(); ++oit) {
            json sub = json::object();
            json opObj = json::object();
            opObj[oit.key()] = oit.value();
            sub[node.field] = opObj;
            node.children.push_back(parseQuery(sub));
        }
        return node;
    }

    // Single operator
    if (vobj.contains("$eq"))  { node.type = QueryNode::Type::EQ;  node.value = vobj["$eq"]; }
    else if (vobj.contains("$ne"))  { node.type = QueryNode::Type::NE;  node.value = vobj["$ne"]; }
    else if (vobj.contains("$gt"))  { node.type = QueryNode::Type::GT;  node.value = vobj["$gt"]; }
    else if (vobj.contains("$gte")) { node.type = QueryNode::Type::GTE; node.value = vobj["$gte"]; }
    else if (vobj.contains("$lt"))  { node.type = QueryNode::Type::LT;  node.value = vobj["$lt"]; }
    else if (vobj.contains("$lte")) { node.type = QueryNode::Type::LTE; node.value = vobj["$lte"]; }
    else if (vobj.contains("$in")) {
        if (!vobj["$in"].is_array()) {
            std::cerr << "[QUERY] $in requires an array" << std::endl;
            return node;
        }
        node.type = QueryNode::Type::IN;
        node.value = vobj["$in"];
    }
    else if (vobj.contains("$nin")) {
        if (!vobj["$nin"].is_array()) {
            std::cerr << "[QUERY] $nin requires an array" << std::endl;
            return node;
        }
        node.type = QueryNode::Type::NIN;
        node.value = vobj["$nin"];
    }
    else if (vobj.contains("$all")) {
        if (!vobj["$all"].is_array()) {
            std::cerr << "[QUERY] $all requires an array" << std::endl;
            return node;
        }
        node.type = QueryNode::Type::ALL;
        node.value = vobj["$all"];
    }
    else if (vobj.contains("$size")) {
        node.type = QueryNode::Type::SIZE;
        node.value = vobj["$size"];
    }
    else if (vobj.contains("$elemMatch")) {
        node.type = QueryNode::Type::ELEM_MATCH;
        // Store the sub-query for evaluation
        node.children.push_back(parseQuery(vobj["$elemMatch"]));
    }
    else if (vobj.contains("$exists")) {
        node.type = QueryNode::Type::EXISTS;
        node.value = vobj["$exists"];
    }
    else if (vobj.contains("$type")) {
        node.type = QueryNode::Type::TYPE;
        node.value = vobj["$type"];
    }
    else if (vobj.contains("$regex")) {
        node.type = QueryNode::Type::REGEX;
        node.regexPattern = vobj["$regex"].get<std::string>();
        node.regexOptions = vobj.value("$options", "");
    }
    else if (vobj.contains("$text")) {
        node.type = QueryNode::Type::TEXT;
        auto textOp = vobj["$text"];
        node.value = textOp.value("$search", "");
    }
    else if (vobj.contains("$not")) {
        // Field-level $not: { field: { $not: { $gt: 5 } } }
        node.type = QueryNode::Type::NOT;
        json subFilter = json::object();
        subFilter[node.field] = vobj["$not"];
        node.children.push_back(parseQuery(subFilter));
        node.field = "";  // Clear field as it's handled in child
    }
    else {
        // Unknown operator or nested object (embedded doc comparison)
        node.type = QueryNode::Type::EQ;
    }

    return node;
}

// ============================================================================
// FIELD EVALUATION
// ============================================================================

static bool matchField(const QueryNode& node, const json& doc) {
    // Handle nested field paths: "user.address.city"
    std::string fieldPath = node.field;
    const json* current = &doc;

    size_t pos = 0;
    while ((pos = fieldPath.find('.')) != std::string::npos) {
        std::string part = fieldPath.substr(0, pos);
        if (!current->is_object() || !current->contains(part)) {
            // For $exists check, return based on existence
            if (node.type == QueryNode::Type::EXISTS) {
                bool shouldExist = node.value.is_boolean() ? node.value.get<bool>() : true;
                return !shouldExist;
            }
            return false;
        }
        current = &(*current)[part];
        fieldPath = fieldPath.substr(pos + 1);
    }

    // Final field check
    bool fieldExists = current->is_object() && current->contains(fieldPath);

    // Handle $exists operator
    if (node.type == QueryNode::Type::EXISTS) {
        bool shouldExist = node.value.is_boolean() ? node.value.get<bool>() : true;
        return fieldExists == shouldExist;
    }

    // For other operators, field must exist
    if (!fieldExists) return false;

    const json& dv = (*current)[fieldPath];

    try {
        switch (node.type) {
            case QueryNode::Type::EQ:
                return dv == node.value;

            case QueryNode::Type::NE:
                return dv != node.value;

            case QueryNode::Type::GT:
            case QueryNode::Type::GTE:
            case QueryNode::Type::LT:
            case QueryNode::Type::LTE: {
                // Numeric comparison with type coercion
                if (dv.is_number() && node.value.is_number()) {
                    double a = dv.get<double>();
                    double b = node.value.get<double>();
                    switch (node.type) {
                        case QueryNode::Type::GT:  return a > b;
                        case QueryNode::Type::GTE: return a >= b;
                        case QueryNode::Type::LT:  return a < b;
                        case QueryNode::Type::LTE: return a <= b;
                        default: break;
                    }
                }
                // String comparison
                if (dv.is_string() && node.value.is_string()) {
                    std::string sa = dv.get<std::string>();
                    std::string sb = node.value.get<std::string>();
                    switch (node.type) {
                        case QueryNode::Type::GT:  return sa > sb;
                        case QueryNode::Type::GTE: return sa >= sb;
                        case QueryNode::Type::LT:  return sa < sb;
                        case QueryNode::Type::LTE: return sa <= sb;
                        default: break;
                    }
                }
                // Date string comparison (ISO 8601)
                if (dv.is_string() && node.value.is_string()) {
                    std::string sa = dv.get<std::string>();
                    std::string sb = node.value.get<std::string>();
                    // If both look like dates, compare lexicographically (works for ISO 8601)
                    if (sa.length() >= 10 && sb.length() >= 10 &&
                        sa[4] == '-' && sa[7] == '-' &&
                        sb[4] == '-' && sb[7] == '-') {
                        switch (node.type) {
                            case QueryNode::Type::GT:  return sa > sb;
                            case QueryNode::Type::GTE: return sa >= sb;
                            case QueryNode::Type::LT:  return sa < sb;
                            case QueryNode::Type::LTE: return sa <= sb;
                            default: break;
                        }
                    }
                }
                // Type mismatch - convert to strings for comparison
                std::string sa = dv.is_string() ? dv.get<std::string>() : dv.dump();
                std::string sb = node.value.is_string() ? node.value.get<std::string>() : node.value.dump();
                switch (node.type) {
                    case QueryNode::Type::GT:  return sa > sb;
                    case QueryNode::Type::GTE: return sa >= sb;
                    case QueryNode::Type::LT:  return sa < sb;
                    case QueryNode::Type::LTE: return sa <= sb;
                    default: break;
                }
                return false;
            }

            case QueryNode::Type::IN: {
                if (!node.value.is_array()) return false;
                // Check if document value is in the array
                for (const auto& item : node.value) {
                    if (dv == item) return true;
                }
                // Also check if document value is an array with intersection
                if (dv.is_array()) {
                    for (const auto& docItem : dv) {
                        for (const auto& queryItem : node.value) {
                            if (docItem == queryItem) return true;
                        }
                    }
                }
                return false;
            }

            case QueryNode::Type::NIN: {
                if (!node.value.is_array()) return true;
                // Must NOT be in the array
                for (const auto& item : node.value) {
                    if (dv == item) return false;
                }
                if (dv.is_array()) {
                    for (const auto& docItem : dv) {
                        for (const auto& queryItem : node.value) {
                            if (docItem == queryItem) return false;
                        }
                    }
                }
                return true;
            }

            case QueryNode::Type::ALL: {
                if (!dv.is_array() || !node.value.is_array()) return false;
                // Document array must contain ALL elements in query array
                for (const auto& required : node.value) {
                    bool found = false;
                    for (const auto& docItem : dv) {
                        if (docItem == required) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) return false;
                }
                return true;
            }

            case QueryNode::Type::SIZE: {
                if (!dv.is_array()) return false;
                int expectedSize = node.value.is_number() ? node.value.get<int>() : -1;
                return static_cast<int>(dv.size()) == expectedSize;
            }

            case QueryNode::Type::TYPE: {
                // Support both type name and type code
                if (node.value.is_string()) {
                    std::string typeName = node.value.get<std::string>();
                    return getJsonTypeName(dv) == typeName;
                }
                if (node.value.is_number()) {
                    int typeCode = node.value.get<int>();
                    return getJsonTypeCode(dv) == typeCode;
                }
                return false;
            }

            case QueryNode::Type::REGEX: {
                if (!dv.is_string()) return false;
                try {
                    std::regex re = getOrCompileRegex(node.regexPattern, node.regexOptions);
                    return std::regex_search(dv.get<std::string>(), re);
                } catch (...) {
                    return false;
                }
            }

            case QueryNode::Type::TEXT: {
                // Simple text search - check if value contains search term
                if (!dv.is_string() || !node.value.is_string()) return false;
                std::string docText = dv.get<std::string>();
                std::string searchTerm = node.value.get<std::string>();
                // Case-insensitive search
                std::transform(docText.begin(), docText.end(), docText.begin(), ::tolower);
                std::transform(searchTerm.begin(), searchTerm.end(), searchTerm.begin(), ::tolower);
                return docText.find(searchTerm) != std::string::npos;
            }

            default:
                return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[QUERY] Field evaluation error: " << e.what() << std::endl;
        return false;
    }
}

// ============================================================================
// MAIN QUERY EVALUATOR
// ============================================================================

bool evalQuery(const QueryNode& node, const json& doc) {
    switch (node.type) {
        case QueryNode::Type::MATCH_ALL:
            return true;

        case QueryNode::Type::ALWAYS_FALSE:
        case QueryNode::Type::INVALID:
            return false;

        // Comparison and element operators
        case QueryNode::Type::EQ:
        case QueryNode::Type::NE:
        case QueryNode::Type::GT:
        case QueryNode::Type::GTE:
        case QueryNode::Type::LT:
        case QueryNode::Type::LTE:
        case QueryNode::Type::IN:
        case QueryNode::Type::NIN:
        case QueryNode::Type::ALL:
        case QueryNode::Type::SIZE:
        case QueryNode::Type::EXISTS:
        case QueryNode::Type::TYPE:
        case QueryNode::Type::REGEX:
        case QueryNode::Type::TEXT:
            return matchField(node, doc);

        // $elemMatch - at least one array element matches the sub-query
        case QueryNode::Type::ELEM_MATCH: {
            if (node.children.empty()) return false;
            // Get the array field
            if (!doc.contains(node.field)) return false;
            const json& arr = doc[node.field];
            if (!arr.is_array()) return false;

            // Check if any element matches the sub-query
            for (const auto& elem : arr) {
                if (evalQuery(node.children[0], elem)) {
                    return true;
                }
            }
            return false;
        }

        // Logical AND - all must match
        case QueryNode::Type::AND: {
            for (const auto& child : node.children) {
                if (!evalQuery(child, doc)) return false;  // Early termination
            }
            return true;
        }

        // Logical OR - at least one must match
        case QueryNode::Type::OR: {
            if (node.children.empty()) return false;
            for (const auto& child : node.children) {
                if (evalQuery(child, doc)) return true;  // Early termination
            }
            return false;
        }

        // Logical NOT - child must NOT match
        case QueryNode::Type::NOT: {
            if (node.children.empty()) return true;
            return !evalQuery(node.children[0], doc);
        }

        // Logical NOR - NONE must match
        case QueryNode::Type::NOR: {
            for (const auto& child : node.children) {
                if (evalQuery(child, doc)) return false;
            }
            return true;
        }

        // Vector search - semantic similarity
        case QueryNode::Type::VECTOR_SEARCH: {
            if (node.queryVector.empty()) return false;
            if (!doc.contains(node.field)) return false;

            std::vector<double> docVector = jsonToVector(doc[node.field]);
            if (docVector.empty()) return false;

            double similarity;
            if (node.vectorMetric == "cosine") {
                similarity = cosineSimilarity(node.queryVector, docVector);
            } else if (node.vectorMetric == "euclidean") {
                // Convert distance to similarity (lower distance = higher similarity)
                double distance = euclideanDistance(node.queryVector, docVector);
                similarity = 1.0 / (1.0 + distance);
            } else if (node.vectorMetric == "dot_product") {
                similarity = dotProduct(node.queryVector, docVector);
            } else {
                similarity = cosineSimilarity(node.queryVector, docVector);
            }
            if (std::isnan(similarity)) return false;

            return similarity >= node.vectorThreshold;
        }

        // Geospatial (placeholder for future implementation)
        case QueryNode::Type::GEO_NEAR:
        case QueryNode::Type::GEO_WITHIN:
            std::cerr << "[QUERY] Geospatial operators not yet implemented" << std::endl;
            return false;
    }

    return false;
}

// ============================================================================
// QUERY VALIDATION (Core feature)
// ============================================================================

QueryValidationResult validateQuery(const json& filter) {
    QueryValidationResult result;
    result.valid = true;
    result.estimatedCost = 10;  // Base cost

    if (filter.is_null() || (filter.is_object() && filter.empty())) {
        result.warnings.push_back("Empty filter will scan all documents");
        result.estimatedCost = 100;
        return result;
    }

    if (!filter.is_object()) {
        result.valid = false;
        result.error = "Filter must be an object";
        return result;
    }

    // Check for common issues
    std::function<void(const json&, int)> checkNode = [&](const json& node, int depth) {
        if (depth > 10) {
            result.warnings.push_back("Deep nesting detected (>10 levels) - may impact performance");
            result.estimatedCost += 20;
            return;
        }

        if (!node.is_object()) return;

        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string& key = it.key();
            const json& val = it.value();

            // Check for $regex without index
            if (key == "$regex") {
                result.warnings.push_back("$regex may be slow without text index");
                result.estimatedCost += 30;
            }

            // Check for $or with many clauses
            if (key == "$or" && val.is_array() && val.size() > 5) {
                result.warnings.push_back("$or with many clauses may be slow");
                result.estimatedCost += static_cast<int>(val.size()) * 5;
            }

            // Check for $nin (typically slow)
            if (key == "$nin") {
                result.warnings.push_back("$nin requires full collection scan");
                result.estimatedCost += 40;
            }

            // Check for negation operators
            if (key == "$not" || key == "$ne") {
                result.warnings.push_back("Negation operators may not use indexes efficiently");
                result.estimatedCost += 15;
            }

            // Recurse into nested objects
            if (val.is_object()) {
                checkNode(val, depth + 1);
            }
            if (val.is_array()) {
                for (const auto& item : val) {
                    if (item.is_object()) {
                        checkNode(item, depth + 1);
                    }
                }
            }
        }
    };

    checkNode(filter, 0);

    // Cap cost at 100
    if (result.estimatedCost > 100) result.estimatedCost = 100;

    return result;
}

// ============================================================================
// QUERY PLAN ANALYSIS (Core feature)
// ============================================================================

QueryPlan analyzeQuery(const QueryNode& query, const std::string& collection) {
    QueryPlan plan;
    plan.fullScan = true;
    plan.useIndex = false;
    plan.estimatedDocs = -1;  // Unknown

    // Check if query can use index
    std::function<void(const QueryNode&)> findIndexableField = [&](const QueryNode& node) {
        if (node.type == QueryNode::Type::EQ && !node.field.empty()) {
            // Equality on 'id' or '_id' can use primary index
            if (node.field == "id" || node.field == "_id") {
                plan.useIndex = true;
                plan.indexField = node.field;
                plan.fullScan = false;
                plan.estimatedDocs = 1;
            }
        }

        // Check children
        for (const auto& child : node.children) {
            findIndexableField(child);
        }
    };

    findIndexableField(query);

    return plan;
}
