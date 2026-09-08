#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <regex>

/**
 * @file query.hpp
 * @brief Production-grade query engine with comprehensive operator support
 *
 * Supported operators:
 * - Comparison: $eq, $ne, $gt, $gte, $lt, $lte
 * - Array: $in, $nin, $all, $size, $elemMatch
 * - Logical: $and, $or, $not, $nor
 * - Element: $exists, $type
 * - String: $regex, $text
 * - Geospatial: $near, $geoWithin (future)
 * - Vector: $vectorSearch (semantic similarity)
 */

struct QueryNode {
    enum class Type {
        // Match types
        ALWAYS_FALSE,
        MATCH_ALL,
        INVALID,

        // Logical operators
        AND,
        OR,
        NOT,
        NOR,

        // Comparison operators
        EQ,
        NE,
        GT,
        GTE,
        LT,
        LTE,

        // Array operators
        IN,
        NIN,
        ALL,
        SIZE,
        ELEM_MATCH,

        // Element operators
        EXISTS,
        TYPE,

        // String operators
        REGEX,
        TEXT,

        // Production operators
        VECTOR_SEARCH,
        GEO_NEAR,
        GEO_WITHIN
    } type = Type::INVALID;

    std::string field;
    nlohmann::json value;
    std::vector<QueryNode> children;

    // Regex options for $regex operator
    std::string regexPattern;
    std::string regexOptions;  // i=case-insensitive, m=multiline, s=dotall

    // Vector search params
    std::vector<double> queryVector;
    double vectorThreshold = 0.8;
    std::string vectorMetric = "cosine";  // cosine, euclidean, dot_product
    int vectorTopK = 10;

    // Helper methods
    bool isMatchAll() const { return type == Type::MATCH_ALL; }
    bool isLogical() const {
        return type == Type::AND || type == Type::OR ||
               type == Type::NOT || type == Type::NOR;
    }
    bool isComparison() const {
        return type == Type::EQ || type == Type::NE ||
               type == Type::GT || type == Type::GTE ||
               type == Type::LT || type == Type::LTE;
    }
};

// Query parsing and evaluation
QueryNode parseQuery(const nlohmann::json& filter);
bool evalQuery(const QueryNode& q, const nlohmann::json& doc);

// Production query validation
struct QueryValidationResult {
    bool valid = false;
    std::string error;
    std::vector<std::string> warnings;
    int estimatedCost = 0;  // 1-100, higher = more expensive
};
QueryValidationResult validateQuery(const nlohmann::json& filter);

// Query optimization hints
struct QueryPlan {
    bool useIndex = false;
    std::string indexField;
    bool fullScan = true;
    int estimatedDocs = -1;
};
QueryPlan analyzeQuery(const QueryNode& query, const std::string& collection);
