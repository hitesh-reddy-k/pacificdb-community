#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

class Storage {
public:
    static void appendDocument(const std::string& file, const json& doc);
    static std::vector<json> readAll(const std::string& file);
    static void writeAll(const std::string& file, const std::vector<json>& docs);
};
