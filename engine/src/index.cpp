#include "index.hpp"

void Index::add(const std::string& key, long offset) {
    map[key].push_back(offset);
}

std::vector<long> Index::find(const std::string& key) {
    if (map.count(key)) return map[key];
    return {};
}
