#pragma once

#include <string>
#include <functional>
#include <nlohmann/json.hpp>
#include <memory>
#include <unordered_map>

using json = nlohmann::json;

/**
 * @brief Plugin/Extension architecture for the database engine
 *
 * Enables:
 * - Custom index implementations
 * - Custom compression codecs
 * - Custom serialization formats
 * - Custom query planners
 * - Monitoring hooks
 */

// ==================== EXTENSION POINTS ====================

class StorageExtension {
public:
    virtual ~StorageExtension() = default;

    /**
     * @brief Serialize data for storage
     */
    virtual std::string serialize(const json& data) = 0;

    /**
     * @brief Deserialize data from storage
     */
    virtual json deserialize(const std::string& data) = 0;

    /**
     * @brief Get extension name and version
     */
    virtual std::string name() const = 0;
    virtual std::string version() const = 0;
};

class IndexExtension {
public:
    virtual ~IndexExtension() = default;

    /**
     * @brief Index a field value
     */
    virtual void index(const std::string& docId, const json& value) = 0;

    /**
     * @brief Query the index
     * @param query The query specification
     * @return List of matching document IDs
     */
    virtual std::vector<std::string> query(const json& query) = 0;

    /**
     * @brief Remove a document from the index
     */
    virtual void remove(const std::string& docId) = 0;

    /**
     * @brief Get index statistics
     */
    virtual json getStats() const = 0;

    virtual std::string name() const = 0;
};

class CompressionExtension {
public:
    virtual ~CompressionExtension() = default;

    /**
     * @brief Compress data
     */
    virtual std::string compress(const std::string& data) = 0;

    /**
     * @brief Decompress data
     */
    virtual std::string decompress(const std::string& data) = 0;

    /**
     * @brief Get compression ratio estimate
     */
    virtual float getCompressionRatio() const = 0;

    virtual std::string name() const = 0;
};

class QueryOptimizerExtension {
public:
    virtual ~QueryOptimizerExtension() = default;

    /**
     * @brief Optimize a query execution plan
     */
    virtual json optimizeQuery(const json& query) = 0;

    /**
     * @brief Estimate query cost
     */
    virtual uint64_t estimateCost(const json& query) = 0;

    virtual std::string name() const = 0;
};

// ==================== HOOKS FOR MONITORING ====================

class EngineHooks {
public:
    using BeforeQueryHook = std::function<void(const json& query)>;
    using AfterQueryHook = std::function<void(const json& query, uint64_t durationMs)>;
    using OnErrorHook = std::function<void(const std::string& error, const std::string& context)>;
    using OnCompactionHook = std::function<void(const json& stats)>;
    using OnMemtableFlushHook = std::function<void(const json& stats)>;

    /**
     * @brief Register hooks for lifecycle events
     */
    static void registerBeforeQueryHook(BeforeQueryHook hook);
    static void registerAfterQueryHook(AfterQueryHook hook);
    static void registerOnErrorHook(OnErrorHook hook);
    static void registerOnCompactionHook(OnCompactionHook hook);
    static void registerOnMemtableFlushHook(OnMemtableFlushHook hook);

    /**
     * @brief Trigger registered hooks
     */
    static void triggerBeforeQuery(const json& query);
    static void triggerAfterQuery(const json& query, uint64_t durationMs);
    static void triggerOnError(const std::string& error, const std::string& context);
    static void triggerOnCompaction(const json& stats);
    static void triggerOnMemtableFlush(const json& stats);

    /**
     * @brief Clear all hooks
     */
    static void clearHooks();

private:
    static std::vector<BeforeQueryHook> beforeQueryHooks;
    static std::vector<AfterQueryHook> afterQueryHooks;
    static std::vector<OnErrorHook> onErrorHooks;
    static std::vector<OnCompactionHook> onCompactionHooks;
    static std::vector<OnMemtableFlushHook> onMemtableFlushHooks;
};

// ==================== REGISTRY FOR EXTENSIONS ====================

class ExtensionRegistry {
public:
    /**
     * @brief Register a storage extension
     */
    static void registerStorageExtension(
        const std::string& name,
        std::shared_ptr<StorageExtension> extension
    );

    /**
     * @brief Register an index extension
     */
    static void registerIndexExtension(
        const std::string& name,
        std::shared_ptr<IndexExtension> extension
    );

    /**
     * @brief Register a compression extension
     */
    static void registerCompressionExtension(
        const std::string& name,
        std::shared_ptr<CompressionExtension> extension
    );

    /**
     * @brief Register a query optimizer extension
     */
    static void registerQueryOptimizerExtension(
        const std::string& name,
        std::shared_ptr<QueryOptimizerExtension> extension
    );

    /**
     * @brief Get registered extension
     */
    static std::shared_ptr<StorageExtension> getStorageExtension(const std::string& name);
    static std::shared_ptr<IndexExtension> getIndexExtension(const std::string& name);
    static std::shared_ptr<CompressionExtension> getCompressionExtension(const std::string& name);
    static std::shared_ptr<QueryOptimizerExtension> getQueryOptimizerExtension(const std::string& name);

    /**
     * @brief List all registered extensions
     */
    static json listExtensions();

private:
    static std::unordered_map<std::string, std::shared_ptr<StorageExtension>> storageExtensions;
    static std::unordered_map<std::string, std::shared_ptr<IndexExtension>> indexExtensions;
    static std::unordered_map<std::string, std::shared_ptr<CompressionExtension>> compressionExtensions;
    static std::unordered_map<std::string, std::shared_ptr<QueryOptimizerExtension>> queryOptimizerExtensions;
};
