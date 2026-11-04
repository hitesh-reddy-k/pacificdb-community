#pragma once
#include "vector_index_generation.hpp"
#include <map>

namespace pacificdb {
// Bounded engine-local derived-index service. All storage/visibility policy is
// supplied by trusted engine integration; this class never owns durable records.
class NativeVectorIndexService {
public:
    NativeVectorIndexService(size_t maximumIndexes, size_t graphBytes, size_t sourceBytes);
    ~NativeVectorIndexService();
    NativeVectorIndexService(const NativeVectorIndexService&) = delete;
    NativeVectorIndexService& operator=(const NativeVectorIndexService&) = delete;
    std::shared_ptr<VectorIndexGeneration> queue(
        const std::string& key, const json& canonicalDefinition, HNSWConfig config,
        size_t sourceByteLimit, VectorIndexGeneration::Certificate certificate,
        VectorIndexGeneration::Source source, std::optional<LexicalConfig> lexical = {}, bool exactOnly = false,
        VectorIndexGeneration::Admission admission = {}, VectorIndexGeneration::Ready ready = {},
        VectorIndexGeneration::Checkpoints checkpoints = {});
    std::shared_ptr<VectorIndexGeneration> get(const std::string& key) const;
    std::optional<json> definition(const std::string& key,const std::shared_ptr<VectorIndexGeneration>& expected) const;
    bool unload(const std::string& key);
    // Nonblocking cancellation for committed namespace drops. Callbacks only
    // inspect the bounded registry key; they must not access storage.
    void retire(const std::function<bool(const std::string&)>& matches);
    void reap();
    void shutdown();
    json metrics() const;
private:
    struct Resources {
        explicit Resources(size_t maximum, size_t graphs, size_t sources)
            : maximumIndexes(maximum), graph(std::make_shared<VectorAllocationBudget>(graphs)),
              source(sources) {}
        size_t maximumIndexes;
        std::atomic<size_t> workers{0};
        std::shared_ptr<VectorAllocationBudget> graph;
        VectorAllocationBudget source;
    };
    struct Entry { json definition; json configuration; std::shared_ptr<VectorIndexGeneration> manager; };
    std::shared_ptr<Resources> resources_;
    mutable std::mutex mutex_;
    bool stopped_{false};
    std::map<std::string, Entry> entries_;
    std::vector<std::shared_ptr<VectorIndexGeneration>> retired_;
};
}
