#include "btree_index.hpp"

namespace pacificdb {
namespace index {

// ============================================================================
// INDEX MANAGER IMPLEMENTATION
// ============================================================================

bool IndexManager::createIndex(const IndexDefinition& def) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = makeIndexKey(def.database, def.collection, def.name);

    if (indexes_.count(key) > 0) {
        return false;  // Index already exists
    }

    // Create appropriate index based on type
    switch (def.type) {
        case IndexType::BTREE:
        case IndexType::COMPOUND: {
            auto tree = std::make_shared<BPlusTree<std::string, std::string>>();
            indexes_[key] = tree;
            definitions_[key] = def;
            return true;
        }
        case IndexType::HASH:
        case IndexType::FULLTEXT:
        case IndexType::GEOSPATIAL:
            // TODO: Implement other index types
            return false;
    }

    return false;
}

bool IndexManager::dropIndex(const std::string& database, const std::string& collection,
                              const std::string& indexName) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = makeIndexKey(database, collection, indexName);

    indexes_.erase(key);
    definitions_.erase(key);

    return true;
}

bool IndexManager::indexExists(const std::string& database, const std::string& collection,
                                const std::string& indexName) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = makeIndexKey(database, collection, indexName);
    return indexes_.count(key) > 0;
}

bool IndexManager::insertEntry(const std::string& database, const std::string& collection,
                                const std::string& indexName, const std::string& key,
                                const std::string& documentId) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string indexKey = makeIndexKey(database, collection, indexName);

    auto it = indexes_.find(indexKey);
    if (it == indexes_.end()) {
        return false;
    }

    return it->second->insert(key, documentId);
}

bool IndexManager::removeEntry(const std::string& database, const std::string& collection,
                                const std::string& indexName, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string indexKey = makeIndexKey(database, collection, indexName);

    auto it = indexes_.find(indexKey);
    if (it == indexes_.end()) {
        return false;
    }

    return it->second->remove(key);
}

std::vector<std::string> IndexManager::lookupIndex(const std::string& database,
                                                    const std::string& collection,
                                                    const std::string& indexName,
                                                    const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string indexKey = makeIndexKey(database, collection, indexName);

    auto it = indexes_.find(indexKey);
    if (it == indexes_.end()) {
        return {};
    }

    auto result = it->second->find(key);
    if (result) {
        return {*result};
    }
    return {};
}

std::vector<std::string> IndexManager::rangeQuery(const std::string& database,
                                                   const std::string& collection,
                                                   const std::string& indexName,
                                                   const std::string& startKey,
                                                   const std::string& endKey) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string indexKey = makeIndexKey(database, collection, indexName);

    auto it = indexes_.find(indexKey);
    if (it == indexes_.end()) {
        return {};
    }

    return it->second->range(startKey, endKey);
}

std::vector<IndexDefinition> IndexManager::listIndexes(const std::string& database,
                                                        const std::string& collection) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<IndexDefinition> result;
    std::string prefix = database + "." + collection + ".";

    for (const auto& [key, def] : definitions_) {
        if (key.find(prefix) == 0) {
            result.push_back(def);
        }
    }

    return result;
}

json IndexManager::getIndexStats(const std::string& database, const std::string& collection,
                                  const std::string& indexName) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = makeIndexKey(database, collection, indexName);

    auto indexIt = indexes_.find(key);
    auto defIt = definitions_.find(key);

    if (indexIt == indexes_.end() || defIt == definitions_.end()) {
        return {{"error", "Index not found"}};
    }

    return {
        {"name", indexName},
        {"type", static_cast<int>(defIt->second.type)},
        {"size", indexIt->second->size()},
        {"height", indexIt->second->height()},
        {"unique", defIt->second.unique},
        {"fields", defIt->second.toJson()["fields"]}
    };
}

bool IndexManager::rebuildIndex(const std::string& database, const std::string& collection,
                                 const std::string& indexName) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = makeIndexKey(database, collection, indexName);

    auto it = indexes_.find(key);
    if (it == indexes_.end()) {
        return false;
    }

    // Scan all entries, clear, and re-insert
    auto entries = it->second->scan();
    it->second->clear();

    for (const auto& [k, v] : entries) {
        it->second->insert(k, v);
    }

    return true;
}

} // namespace index
} // namespace pacificdb
