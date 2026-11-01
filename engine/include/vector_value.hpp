#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace pacificdb::vectors {
using json = nlohmann::json;
struct Value {
    uint32_t dimensions{0};
    std::vector<uint32_t> indices; // Empty for dense; sparse flag disambiguates zero sparse vectors.
    std::vector<double> values;
    bool sparse{false};
};
inline uint32_t unsignedInteger(const json& value, uint32_t maximum) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > maximum)
        throw std::invalid_argument("invalid sparse vector dimension/index");
    return value.get<uint32_t>();
}
inline Value decode(const json& input) {
    constexpr uint32_t maxDimensions = 1000000000;
    constexpr size_t maxValues = 1000000;
    Value out;
    if (input.is_array()) {
        if (input.empty() || input.size() > maxValues)
            throw std::invalid_argument("vector must be a non-empty numeric array within vector limits");
        out.dimensions = static_cast<uint32_t>(input.size());
        out.values.reserve(input.size());
        for (const auto& value : input) {
            if (!value.is_number() || !std::isfinite(value.get<double>()))
                throw std::invalid_argument("vector must be a non-empty numeric array");
            out.values.push_back(value.get<double>());
        }
        return out;
    }
    if (!input.is_object() || input.size() != 5 || input.value("format", std::string()) != "sparse-v1" ||
        !input.contains("dimensions") || !input.contains("indices") || !input.contains("values") ||
        !input.contains("precision") || input.at("precision") != "float64")
        throw std::invalid_argument("invalid sparse-v1 vector representation");
    out.sparse = true;
    out.dimensions = unsignedInteger(input.at("dimensions"), maxDimensions);
    const auto& indices = input.at("indices"); const auto& values = input.at("values");
    if (!out.dimensions || !indices.is_array() || !values.is_array() ||
        indices.size() != values.size() || indices.size() > maxValues)
        throw std::invalid_argument("invalid sparse vector shape");
    out.indices.reserve(indices.size()); out.values.reserve(values.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        const auto index = unsignedInteger(indices[i], out.dimensions - 1);
        if ((i && index <= out.indices.back()) || !values[i].is_number() ||
            !std::isfinite(values[i].get<double>()))
            throw std::invalid_argument("sparse indices must be sorted unique and values finite");
        out.indices.push_back(index); out.values.push_back(values[i].get<double>());
    }
    return out;
}
inline void validateName(const std::string& name) {
    if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos)
        throw std::invalid_argument("vector_name must be 1-128 bytes without NUL");
}
inline void validateRecord(const json& record) {
    if (!record.is_object() || (!record.contains("vector") && !record.contains("vectors")))
        throw std::invalid_argument("vector record requires vector or named vectors");
    size_t values = 0;
    if (record.contains("vector")) values += decode(record.at("vector")).values.size();
    if (record.contains("vectors")) {
        const auto& named = record.at("vectors");
        if (!named.is_object() || named.empty() || named.size() > 64)
            throw std::invalid_argument("vectors must contain 1-64 named vector spaces");
        for (auto it = named.begin(); it != named.end(); ++it) {
            validateName(it.key()); values += decode(it.value()).values.size();
            if (values > 1000000) throw std::invalid_argument("vector record numeric limit exceeded");
        }
    }
}
// Linear in nonzeros for sparse/sparse, no allocation proportional to dimension.
inline double score(const Value& a, const Value& b, const std::string& metric) {
    if (a.dimensions != b.dimensions) return -std::numeric_limits<double>::infinity();
    long double dot = 0, aa = 0, bb = 0, square = 0;
    size_t i = 0, j = 0;
    while (i < a.values.size() || j < b.values.size()) {
        const auto ai = i == a.values.size() ? uint64_t{UINT32_MAX} : a.sparse ? a.indices[i] : i;
        const auto bj = j == b.values.size() ? uint64_t{UINT32_MAX} : b.sparse ? b.indices[j] : j;
        long double x = 0, y = 0;
        if (ai <= bj) x = a.values[i++];
        if (bj <= ai) y = b.values[j++];
        dot += x * y; aa += x * x; bb += y * y;
        const auto delta = x - y; square += delta * delta;
    }
    const auto result = metric == "dot" || metric == "dot_product" ? dot :
        metric == "l2" || metric == "euclidean" ? -std::sqrt(square) :
        aa == 0 || bb == 0 ? 0.L : std::clamp(dot / std::sqrt(aa * bb), -1.L, 1.L);
    if (!std::isfinite(result) || std::abs(result) > std::numeric_limits<double>::max())
        throw std::runtime_error("vector_score_out_of_range");
    return static_cast<double>(result);
}
}
