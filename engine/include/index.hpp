#pragma once
#include <unordered_map>
#include <vector>
#include <string>

class Index {
    std::unordered_map<std::string, std::vector<long>> map;

public:
    void add(const std::string& key, long offset);
    std::vector<long> find(const std::string& key);
};
