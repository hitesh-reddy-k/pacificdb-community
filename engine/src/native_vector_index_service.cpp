#include "native_vector_index_service.hpp"
#include <stdexcept>

namespace pacificdb {
NativeVectorIndexService::NativeVectorIndexService(size_t maximum, size_t graphs, size_t sources)
    : resources_(std::make_shared<Resources>(maximum, graphs, sources)) {
    if (!maximum || maximum > 64 || !graphs || !sources)
        throw std::invalid_argument("invalid native vector service limits");
    retired_.reserve(maximum);
}
NativeVectorIndexService::~NativeVectorIndexService() { shutdown(); }
std::shared_ptr<VectorIndexGeneration> NativeVectorIndexService::queue(
    const std::string& key, const json& definition, HNSWConfig config, size_t sourceBytes,
    VectorIndexGeneration::Certificate certificate, VectorIndexGeneration::Source source,
    std::optional<LexicalConfig> lexical, bool exactOnly, VectorIndexGeneration::Admission admission, VectorIndexGeneration::Ready ready) {
    reap();
    if (key.empty() || key.size() > 4096 || !definition.is_object() || definition.dump().size() > 16384)
        throw std::invalid_argument("invalid native vector index identity/definition");
    HNSWIndex validate(config);
    if (!sourceBytes || !certificate || !source) throw std::invalid_argument("invalid vector source");
    json configuration{{"M", config.M}, {"ef_construction", config.efConstruction},
        {"ef_search", config.efSearch}, {"metric", static_cast<int>(config.metric)},
        {"dimensions", config.dimensions}, {"max_elements", config.maxElements},
        {"max_index_bytes", config.maxMemoryBytes}, {"max_build_ms", config.maxBuildMilliseconds},
        {"max_source_bytes", sourceBytes}, {"exact_only", exactOnly}};
    if (lexical) {
        LexicalIndex validateLexical(*lexical);
        configuration["lexical"] = {{"memory", lexical->maxMemoryBytes}, {"records", lexical->maxRecords},
            {"candidates", lexical->maxCandidates}, {"k1", lexical->k1}, {"b", lexical->b}};
    }
    std::shared_ptr<VectorIndexGeneration> manager;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) throw std::runtime_error("vector_index_service_stopped");
        auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (found->second.definition != definition || found->second.configuration != configuration)
                throw std::runtime_error("vector_index_reconfigure_requires_unload");
            manager = found->second.manager;
        } else {
            auto workers = resources_->workers.load();
            do {
                if (workers >= resources_->maximumIndexes)
                    throw std::runtime_error("vector_index_worker_limit_exceeded");
            } while (!resources_->workers.compare_exchange_weak(workers, workers + 1));
            if (!resources_->source.reserve(sourceBytes)) {
                resources_->workers.fetch_sub(1);
                throw VectorMemoryLimit();
            }
            const auto resources = resources_;
            VectorIndexGeneration* allocated = nullptr;
            try { allocated = new VectorIndexGeneration(sourceBytes, resources->graph); }
            catch (...) {
                resources->source.release(sourceBytes); resources->workers.fetch_sub(1); throw;
            }
            manager = std::shared_ptr<VectorIndexGeneration>(allocated,
                [resources, sourceBytes](VectorIndexGeneration* instance) {
                    delete instance; // joins before worker/source reservation is released
                    resources->source.release(sourceBytes); resources->workers.fetch_sub(1);
                });
            entries_.emplace(key, Entry{definition, configuration, manager});
        }
    }
    // Queue admission and every callback occur outside the registry mutex.
    // A concurrent unload cancels a task through the manager revision contract.
    manager->enqueue(config, std::move(certificate), std::move(source), lexical, exactOnly, std::move(admission), std::move(ready));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (stopped_ || found == entries_.end() || found->second.manager != manager)
            throw std::runtime_error("vector_index_unloaded");
    }
    return manager;
}
std::shared_ptr<VectorIndexGeneration> NativeVectorIndexService::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    return found == entries_.end() ? nullptr : found->second.manager;
}
bool NativeVectorIndexService::unload(const std::string& key) {
    std::shared_ptr<VectorIndexGeneration> manager;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) return false;
        manager = std::move(found->second.manager); entries_.erase(found);
    }
    manager->closeAndWait(); // joining also stays outside registry lock
    return true;
}
void NativeVectorIndexService::retire(const std::function<bool(const std::string&)>& matches) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it=entries_.begin();it!=entries_.end();) {
        if (!matches(it->first)) { ++it; continue; }
        it->second.manager->close();
        retired_.push_back(std::move(it->second.manager));
        it=entries_.erase(it);
    }
}
void NativeVectorIndexService::reap() {
    // At most maximumIndexes workers exist, including cancelled workers and
    // externally retained managers. A finished worker has already freed its
    // generation; final joining/destruction happens outside the registry lock.
    for (;;) {
        std::shared_ptr<VectorIndexGeneration> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it=retired_.begin();it!=retired_.end();++it) {
                if ((*it)->finished()) {
                    finished=std::move(*it);retired_.erase(it);break;
                }
            }
        }
        if (!finished) return;
        finished->closeAndWait();
    }
}
void NativeVectorIndexService::shutdown() {
    std::map<std::string, Entry> previous;
    std::vector<std::shared_ptr<VectorIndexGeneration>> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true; previous.swap(entries_); retired.swap(retired_);
    }
    for (const auto& entry : previous) entry.second.manager->close();
    for (const auto& entry : previous) entry.second.manager->closeAndWait();
    for (const auto& manager : retired) manager->closeAndWait();
}
json NativeVectorIndexService::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {{"registered_indexes", entries_.size()}, {"retired_workers", retired_.size()}, {"live_workers", resources_->workers.load()},
        {"worker_limit", resources_->maximumIndexes}, {"graph_bytes", resources_->graph->used()},
        {"graph_peak_bytes", resources_->graph->peak()}, {"graph_limit_bytes", resources_->graph->limit()},
        {"reserved_source_bytes", resources_->source.used()}, {"source_limit_bytes", resources_->source.limit()},
        {"stopped", stopped_}};
}
}
