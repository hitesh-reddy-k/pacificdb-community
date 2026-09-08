#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <functional>
#include <cstring>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pacificdb {
namespace index {

// ============================================================================
// B+TREE CONSTANTS
// ============================================================================

constexpr int BTREE_ORDER = 128;           // Maximum keys per node
constexpr int BTREE_MIN_KEYS = 64;         // Minimum keys per node (ORDER/2)

// ============================================================================
// INDEX KEY TYPES
// ============================================================================

enum class IndexType {
    BTREE,           // Standard B+Tree for range queries
    HASH,            // Hash index for equality queries
    FULLTEXT,        // Full-text search index
    GEOSPATIAL,      // 2D/3D spatial index
    COMPOUND         // Multi-field composite index
};

enum class SortOrder {
    ASCENDING,
    DESCENDING
};

struct IndexField {
    std::string fieldName;
    SortOrder order;
    bool sparse;  // Skip documents without this field

    json toJson() const {
        return {
            {"fieldName", fieldName},
            {"order", order == SortOrder::ASCENDING ? "asc" : "desc"},
            {"sparse", sparse}
        };
    }
};

struct IndexDefinition {
    std::string name;
    std::string collection;
    std::string database;
    IndexType type;
    std::vector<IndexField> fields;
    bool unique;
    bool partial;  // Partial index with filter
    std::string partialFilter;  // JSON filter expression

    json toJson() const {
        json j = {
            {"name", name},
            {"collection", collection},
            {"database", database},
            {"type", static_cast<int>(type)},
            {"unique", unique},
            {"partial", partial},
            {"partialFilter", partialFilter}
        };
        j["fields"] = json::array();
        for (const auto& f : fields) {
            j["fields"].push_back(f.toJson());
        }
        return j;
    }
};

// ============================================================================
// B+TREE NODE
// ============================================================================

template<typename K, typename V>
struct BPlusTreeNode {
    bool isLeaf;
    std::vector<K> keys;
    std::vector<V> values;  // Only for leaf nodes
    std::vector<std::shared_ptr<BPlusTreeNode<K, V>>> children;  // Only for internal nodes
    std::weak_ptr<BPlusTreeNode<K, V>> parent;
    std::shared_ptr<BPlusTreeNode<K, V>> next;  // For leaf chain (range queries)
    std::shared_ptr<BPlusTreeNode<K, V>> prev;  // For reverse iteration

    BPlusTreeNode(bool leaf = true) : isLeaf(leaf) {}

    bool isFull() const { return keys.size() >= BTREE_ORDER; }
    bool isUnderflow() const { return keys.size() < BTREE_MIN_KEYS; }
};

// ============================================================================
// B+TREE INDEX
// ============================================================================

template<typename K, typename V>
class BPlusTree {
public:
    using Node = BPlusTreeNode<K, V>;
    using NodePtr = std::shared_ptr<Node>;
    using KeyComparator = std::function<int(const K&, const K&)>;

    BPlusTree(KeyComparator comparator = nullptr)
        : root_(std::make_shared<Node>(true))
        , size_(0)
        , comparator_(comparator ? comparator : defaultComparator) {}

    // Basic operations
    bool insert(const K& key, const V& value);
    bool remove(const K& key);
    std::optional<V> find(const K& key) const;
    bool contains(const K& key) const;

    // Range queries
    std::vector<V> range(const K& start, const K& end, bool inclusiveStart = true,
                         bool inclusiveEnd = true) const;
    std::vector<V> lessThan(const K& key, bool inclusive = false) const;
    std::vector<V> greaterThan(const K& key, bool inclusive = false) const;

    // Prefix search (for string keys)
    std::vector<V> prefixSearch(const K& prefix) const;

    // Stats
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    int height() const;

    // Iteration
    std::vector<std::pair<K, V>> scan(int limit = -1) const;
    std::vector<std::pair<K, V>> scanReverse(int limit = -1) const;

    // Bulk operations
    void bulkLoad(std::vector<std::pair<K, V>>& sortedData);
    void clear();

    // Serialization
    json toJson() const;
    void fromJson(const json& j);

private:
    static int defaultComparator(const K& a, const K& b) {
        if (a < b) return -1;
        if (a > b) return 1;
        return 0;
    }

    NodePtr findLeaf(const K& key) const;
    void insertInternal(NodePtr node, const K& key, NodePtr child);
    void splitLeaf(NodePtr leaf);
    void splitInternal(NodePtr node);
    void removeFromLeaf(NodePtr leaf, const K& key);
    void handleUnderflow(NodePtr node);
    NodePtr getLeftMostLeaf() const;
    NodePtr getRightMostLeaf() const;

    NodePtr root_;
    size_t size_;
    KeyComparator comparator_;
    mutable std::mutex mutex_;
};

// ============================================================================
// B+TREE IMPLEMENTATION
// ============================================================================

template<typename K, typename V>
bool BPlusTree<K, V>::insert(const K& key, const V& value) {
    std::lock_guard<std::mutex> lock(mutex_);

    NodePtr leaf = findLeaf(key);

    // Find insertion position
    size_t pos = 0;
    while (pos < leaf->keys.size() && comparator_(key, leaf->keys[pos]) > 0) {
        pos++;
    }

    // Check for duplicate
    if (pos < leaf->keys.size() && comparator_(key, leaf->keys[pos]) == 0) {
        leaf->values[pos] = value;  // Update existing
        return true;
    }

    // Insert
    leaf->keys.insert(leaf->keys.begin() + pos, key);
    leaf->values.insert(leaf->values.begin() + pos, value);
    size_++;

    // Split if necessary
    if (leaf->isFull()) {
        splitLeaf(leaf);
    }

    return true;
}

template<typename K, typename V>
bool BPlusTree<K, V>::remove(const K& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    NodePtr leaf = findLeaf(key);

    // Find key position
    size_t pos = 0;
    while (pos < leaf->keys.size() && comparator_(key, leaf->keys[pos]) != 0) {
        pos++;
    }

    if (pos >= leaf->keys.size()) {
        return false;  // Key not found
    }

    removeFromLeaf(leaf, key);
    size_--;

    // Handle underflow
    if (leaf->isUnderflow() && leaf != root_) {
        handleUnderflow(leaf);
    }

    return true;
}

template<typename K, typename V>
std::optional<V> BPlusTree<K, V>::find(const K& key) const {
    std::lock_guard<std::mutex> lock(mutex_);

    NodePtr leaf = findLeaf(key);

    for (size_t i = 0; i < leaf->keys.size(); i++) {
        if (comparator_(key, leaf->keys[i]) == 0) {
            return leaf->values[i];
        }
    }

    return std::nullopt;
}

template<typename K, typename V>
bool BPlusTree<K, V>::contains(const K& key) const {
    return find(key).has_value();
}

template<typename K, typename V>
std::vector<V> BPlusTree<K, V>::range(const K& start, const K& end,
                                       bool inclusiveStart, bool inclusiveEnd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<V> result;

    NodePtr leaf = findLeaf(start);

    while (leaf) {
        for (size_t i = 0; i < leaf->keys.size(); i++) {
            int cmpStart = comparator_(leaf->keys[i], start);
            int cmpEnd = comparator_(leaf->keys[i], end);

            bool afterStart = cmpStart > 0 || (inclusiveStart && cmpStart == 0);
            bool beforeEnd = cmpEnd < 0 || (inclusiveEnd && cmpEnd == 0);

            if (afterStart && beforeEnd) {
                result.push_back(leaf->values[i]);
            }

            if (cmpEnd > 0) {
                return result;  // Past end
            }
        }
        leaf = leaf->next;
    }

    return result;
}

template<typename K, typename V>
std::vector<V> BPlusTree<K, V>::lessThan(const K& key, bool inclusive) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<V> result;

    NodePtr leaf = getLeftMostLeaf();

    while (leaf) {
        for (size_t i = 0; i < leaf->keys.size(); i++) {
            int cmp = comparator_(leaf->keys[i], key);
            if (cmp < 0 || (inclusive && cmp == 0)) {
                result.push_back(leaf->values[i]);
            } else {
                return result;
            }
        }
        leaf = leaf->next;
    }

    return result;
}

template<typename K, typename V>
std::vector<V> BPlusTree<K, V>::greaterThan(const K& key, bool inclusive) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<V> result;

    NodePtr leaf = findLeaf(key);

    while (leaf) {
        for (size_t i = 0; i < leaf->keys.size(); i++) {
            int cmp = comparator_(leaf->keys[i], key);
            if (cmp > 0 || (inclusive && cmp == 0)) {
                result.push_back(leaf->values[i]);
            }
        }
        leaf = leaf->next;
    }

    return result;
}

template<typename K, typename V>
typename BPlusTree<K, V>::NodePtr BPlusTree<K, V>::findLeaf(const K& key) const {
    NodePtr node = root_;

    while (!node->isLeaf) {
        size_t i = 0;
        while (i < node->keys.size() && comparator_(key, node->keys[i]) >= 0) {
            i++;
        }
        node = node->children[i];
    }

    return node;
}

template<typename K, typename V>
void BPlusTree<K, V>::splitLeaf(NodePtr leaf) {
    int mid = leaf->keys.size() / 2;

    auto newLeaf = std::make_shared<Node>(true);

    // Move half to new leaf
    newLeaf->keys.assign(leaf->keys.begin() + mid, leaf->keys.end());
    newLeaf->values.assign(leaf->values.begin() + mid, leaf->values.end());
    leaf->keys.resize(mid);
    leaf->values.resize(mid);

    // Update leaf chain
    newLeaf->next = leaf->next;
    newLeaf->prev = leaf;
    if (leaf->next) leaf->next->prev = newLeaf;
    leaf->next = newLeaf;

    // Insert into parent
    K promotedKey = newLeaf->keys[0];

    if (auto parent = leaf->parent.lock()) {
        newLeaf->parent = parent;
        insertInternal(parent, promotedKey, newLeaf);
    } else {
        // Create new root
        auto newRoot = std::make_shared<Node>(false);
        newRoot->keys.push_back(promotedKey);
        newRoot->children.push_back(leaf);
        newRoot->children.push_back(newLeaf);
        leaf->parent = newRoot;
        newLeaf->parent = newRoot;
        root_ = newRoot;
    }
}

template<typename K, typename V>
void BPlusTree<K, V>::insertInternal(NodePtr node, const K& key, NodePtr child) {
    size_t pos = 0;
    while (pos < node->keys.size() && comparator_(key, node->keys[pos]) > 0) {
        pos++;
    }

    node->keys.insert(node->keys.begin() + pos, key);
    node->children.insert(node->children.begin() + pos + 1, child);

    if (node->isFull()) {
        splitInternal(node);
    }
}

template<typename K, typename V>
void BPlusTree<K, V>::splitInternal(NodePtr node) {
    int mid = node->keys.size() / 2;
    K promotedKey = node->keys[mid];

    auto newNode = std::make_shared<Node>(false);

    // Move half to new node
    newNode->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
    newNode->children.assign(node->children.begin() + mid + 1, node->children.end());

    // Update parent references
    for (auto& child : newNode->children) {
        child->parent = newNode;
    }

    node->keys.resize(mid);
    node->children.resize(mid + 1);

    if (auto parent = node->parent.lock()) {
        newNode->parent = parent;
        insertInternal(parent, promotedKey, newNode);
    } else {
        auto newRoot = std::make_shared<Node>(false);
        newRoot->keys.push_back(promotedKey);
        newRoot->children.push_back(node);
        newRoot->children.push_back(newNode);
        node->parent = newRoot;
        newNode->parent = newRoot;
        root_ = newRoot;
    }
}

template<typename K, typename V>
void BPlusTree<K, V>::removeFromLeaf(NodePtr leaf, const K& key) {
    for (size_t i = 0; i < leaf->keys.size(); i++) {
        if (comparator_(key, leaf->keys[i]) == 0) {
            leaf->keys.erase(leaf->keys.begin() + i);
            leaf->values.erase(leaf->values.begin() + i);
            return;
        }
    }
}

template<typename K, typename V>
void BPlusTree<K, V>::handleUnderflow(NodePtr node) {
    // Simplified: Just allow underflow for now
    // In production, would borrow from siblings or merge
}

template<typename K, typename V>
typename BPlusTree<K, V>::NodePtr BPlusTree<K, V>::getLeftMostLeaf() const {
    NodePtr node = root_;
    while (!node->isLeaf) {
        node = node->children[0];
    }
    return node;
}

template<typename K, typename V>
typename BPlusTree<K, V>::NodePtr BPlusTree<K, V>::getRightMostLeaf() const {
    NodePtr node = root_;
    while (!node->isLeaf) {
        node = node->children.back();
    }
    return node;
}

template<typename K, typename V>
int BPlusTree<K, V>::height() const {
    std::lock_guard<std::mutex> lock(mutex_);

    int h = 0;
    NodePtr node = root_;
    while (!node->isLeaf) {
        h++;
        node = node->children[0];
    }
    return h + 1;
}

template<typename K, typename V>
std::vector<std::pair<K, V>> BPlusTree<K, V>::scan(int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<K, V>> result;

    NodePtr leaf = getLeftMostLeaf();
    int count = 0;

    while (leaf && (limit < 0 || count < limit)) {
        for (size_t i = 0; i < leaf->keys.size(); i++) {
            if (limit >= 0 && count >= limit) break;
            result.push_back({leaf->keys[i], leaf->values[i]});
            count++;
        }
        leaf = leaf->next;
    }

    return result;
}

template<typename K, typename V>
void BPlusTree<K, V>::bulkLoad(std::vector<std::pair<K, V>>& sortedData) {
    std::lock_guard<std::mutex> lock(mutex_);

    clear();

    for (auto& [key, value] : sortedData) {
        insert(key, value);  // Will use internal locking
    }
}

template<typename K, typename V>
void BPlusTree<K, V>::clear() {
    root_ = std::make_shared<Node>(true);
    size_ = 0;
}

// ============================================================================
// INDEX MANAGER
// ============================================================================

class IndexManager {
public:
    static IndexManager& instance() {
        static IndexManager inst;
        return inst;
    }

    // Index lifecycle
    bool createIndex(const IndexDefinition& def);
    bool dropIndex(const std::string& database, const std::string& collection,
                   const std::string& indexName);
    bool indexExists(const std::string& database, const std::string& collection,
                     const std::string& indexName) const;

    // Index operations
    bool insertEntry(const std::string& database, const std::string& collection,
                     const std::string& indexName, const std::string& key,
                     const std::string& documentId);
    bool removeEntry(const std::string& database, const std::string& collection,
                     const std::string& indexName, const std::string& key);
    std::vector<std::string> lookupIndex(const std::string& database,
                                          const std::string& collection,
                                          const std::string& indexName,
                                          const std::string& key) const;
    std::vector<std::string> rangeQuery(const std::string& database,
                                         const std::string& collection,
                                         const std::string& indexName,
                                         const std::string& startKey,
                                         const std::string& endKey) const;

    // List indexes
    std::vector<IndexDefinition> listIndexes(const std::string& database,
                                              const std::string& collection) const;

    // Index stats
    json getIndexStats(const std::string& database, const std::string& collection,
                       const std::string& indexName) const;

    // Rebuild index
    bool rebuildIndex(const std::string& database, const std::string& collection,
                      const std::string& indexName);

private:
    IndexManager() = default;

    std::string makeIndexKey(const std::string& db, const std::string& coll,
                             const std::string& name) const {
        return db + "." + coll + "." + name;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, IndexDefinition> definitions_;
    std::unordered_map<std::string, std::shared_ptr<BPlusTree<std::string, std::string>>> indexes_;
};

} // namespace index
} // namespace pacificdb
