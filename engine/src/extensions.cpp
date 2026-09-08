#include "extensions.hpp"
#include <iostream>

// Static member initialization for EngineHooks
std::vector<EngineHooks::BeforeQueryHook> EngineHooks::beforeQueryHooks;
std::vector<EngineHooks::AfterQueryHook> EngineHooks::afterQueryHooks;
std::vector<EngineHooks::OnErrorHook> EngineHooks::onErrorHooks;
std::vector<EngineHooks::OnCompactionHook> EngineHooks::onCompactionHooks;
std::vector<EngineHooks::OnMemtableFlushHook> EngineHooks::onMemtableFlushHooks;

void EngineHooks::registerBeforeQueryHook(BeforeQueryHook hook) {
    beforeQueryHooks.push_back(hook);
}

void EngineHooks::registerAfterQueryHook(AfterQueryHook hook) {
    afterQueryHooks.push_back(hook);
}

void EngineHooks::registerOnErrorHook(OnErrorHook hook) {
    onErrorHooks.push_back(hook);
}

void EngineHooks::registerOnCompactionHook(OnCompactionHook hook) {
    onCompactionHooks.push_back(hook);
}

void EngineHooks::registerOnMemtableFlushHook(OnMemtableFlushHook hook) {
    onMemtableFlushHooks.push_back(hook);
}

void EngineHooks::triggerBeforeQuery(const json& query) {
    for (auto& hook : beforeQueryHooks) {
        try {
            hook(query);
        } catch (const std::exception& e) {
            std::cerr << "[HOOK] Error in beforeQuery hook: " << e.what() << "\n";
        }
    }
}

void EngineHooks::triggerAfterQuery(const json& query, uint64_t durationMs) {
    for (auto& hook : afterQueryHooks) {
        try {
            hook(query, durationMs);
        } catch (const std::exception& e) {
            std::cerr << "[HOOK] Error in afterQuery hook: " << e.what() << "\n";
        }
    }
}

void EngineHooks::triggerOnError(const std::string& error, const std::string& context) {
    for (auto& hook : onErrorHooks) {
        try {
            hook(error, context);
        } catch (const std::exception& e) {
            std::cerr << "[HOOK] Error in onError hook: " << e.what() << "\n";
        }
    }
}

void EngineHooks::triggerOnCompaction(const json& stats) {
    for (auto& hook : onCompactionHooks) {
        try {
            hook(stats);
        } catch (const std::exception& e) {
            std::cerr << "[HOOK] Error in onCompaction hook: " << e.what() << "\n";
        }
    }
}

void EngineHooks::triggerOnMemtableFlush(const json& stats) {
    for (auto& hook : onMemtableFlushHooks) {
        try {
            hook(stats);
        } catch (const std::exception& e) {
            std::cerr << "[HOOK] Error in onMemtableFlush hook: " << e.what() << "\n";
        }
    }
}

void EngineHooks::clearHooks() {
    beforeQueryHooks.clear();
    afterQueryHooks.clear();
    onErrorHooks.clear();
    onCompactionHooks.clear();
    onMemtableFlushHooks.clear();
    std::cout << "[HOOKS] All hooks cleared\n";
}

// ==================== ExtensionRegistry Implementation ====================

std::unordered_map<std::string, std::shared_ptr<StorageExtension>> ExtensionRegistry::storageExtensions;
std::unordered_map<std::string, std::shared_ptr<IndexExtension>> ExtensionRegistry::indexExtensions;
std::unordered_map<std::string, std::shared_ptr<CompressionExtension>> ExtensionRegistry::compressionExtensions;
std::unordered_map<std::string, std::shared_ptr<QueryOptimizerExtension>> ExtensionRegistry::queryOptimizerExtensions;

void ExtensionRegistry::registerStorageExtension(
    const std::string& name,
    std::shared_ptr<StorageExtension> extension) {
    storageExtensions[name] = extension;
    std::cout << "[REGISTRY] Registered StorageExtension: " << name << "\n";
}

void ExtensionRegistry::registerIndexExtension(
    const std::string& name,
    std::shared_ptr<IndexExtension> extension) {
    indexExtensions[name] = extension;
    std::cout << "[REGISTRY] Registered IndexExtension: " << name << "\n";
}

void ExtensionRegistry::registerCompressionExtension(
    const std::string& name,
    std::shared_ptr<CompressionExtension> extension) {
    compressionExtensions[name] = extension;
    std::cout << "[REGISTRY] Registered CompressionExtension: " << name << "\n";
}

void ExtensionRegistry::registerQueryOptimizerExtension(
    const std::string& name,
    std::shared_ptr<QueryOptimizerExtension> extension) {
    queryOptimizerExtensions[name] = extension;
    std::cout << "[REGISTRY] Registered QueryOptimizerExtension: " << name << "\n";
}

std::shared_ptr<StorageExtension> ExtensionRegistry::getStorageExtension(const std::string& name) {
    auto it = storageExtensions.find(name);
    return (it != storageExtensions.end()) ? it->second : nullptr;
}

std::shared_ptr<IndexExtension> ExtensionRegistry::getIndexExtension(const std::string& name) {
    auto it = indexExtensions.find(name);
    return (it != indexExtensions.end()) ? it->second : nullptr;
}

std::shared_ptr<CompressionExtension> ExtensionRegistry::getCompressionExtension(const std::string& name) {
    auto it = compressionExtensions.find(name);
    return (it != compressionExtensions.end()) ? it->second : nullptr;
}

std::shared_ptr<QueryOptimizerExtension> ExtensionRegistry::getQueryOptimizerExtension(const std::string& name) {
    auto it = queryOptimizerExtensions.find(name);
    return (it != queryOptimizerExtensions.end()) ? it->second : nullptr;
}

json ExtensionRegistry::listExtensions() {
    json result = json::object();

    json storageList = json::array();
    for (const auto& [name, ext] : storageExtensions) {
        storageList.push_back(name);
    }

    json indexList = json::array();
    for (const auto& [name, ext] : indexExtensions) {
        indexList.push_back(name);
    }

    json compressionList = json::array();
    for (const auto& [name, ext] : compressionExtensions) {
        compressionList.push_back(name);
    }

    json optimizerList = json::array();
    for (const auto& [name, ext] : queryOptimizerExtensions) {
        optimizerList.push_back(name);
    }

    result["storageExtensions"] = storageList;
    result["indexExtensions"] = indexList;
    result["compressionExtensions"] = compressionList;
    result["queryOptimizerExtensions"] = optimizerList;

    return result;
}
