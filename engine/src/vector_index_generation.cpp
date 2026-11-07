#include "vector_index_generation.hpp"
#include "test_failpoint.hpp"
#include <stdexcept>
#include <unordered_set>
#include <variant>

namespace pacificdb {
VectorIndexGeneration::VectorIndexGeneration(size_t budget)
    : VectorIndexGeneration(budget, {}) {}
VectorIndexGeneration::VectorIndexGeneration(size_t budget,
    std::shared_ptr<VectorAllocationBudget> sharedGraphBudget)
    : sourceByteBudget_(budget), sharedGraphBudget_(std::move(sharedGraphBudget)) {
    if (budget == 0) throw std::invalid_argument("vector source budget must be positive");
    worker_ = std::thread([this] { run(); finished_.store(true); });
}
VectorIndexGeneration::~VectorIndexGeneration() {
    closeAndWait();
}
void VectorIndexGeneration::closeAndWait() {
    close();
    std::lock_guard<std::mutex> lock(joinMutex_);
    if (worker_.joinable()) worker_.join();
}
void VectorIndexGeneration::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++revision_;
        stopped_.store(true);
        status_.state = State::STALE;
        if (activeCancellation_) activeCancellation_->store(true);
    }
    wake_.notify_all();
}
bool VectorIndexGeneration::enqueue(HNSWConfig config, Certificate certificate, Source source,
                                    std::optional<LexicalConfig> lexical, bool exactOnly, Admission admission, Ready ready, Checkpoints checkpoints, bool sourceVisibilityComplete) {
    // Validate synchronously, without changing an existing generation on error.
    HNSWIndex validate(config);
    if (lexical) { LexicalIndex validateLexical(*lexical); }
    if (!certificate || !source) throw std::invalid_argument("missing vector source/certificate");
    if(sourceVisibilityComplete && (exactOnly||lexical||!ready))
        throw std::invalid_argument("complete visibility build requires HNSW and a coverage publication callback");
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_.load() || !queue_.empty() || building_) return false;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    queue_.push_back({config, std::move(certificate), std::move(source), ++revision_, cancelled, lexical, exactOnly, std::move(admission), {}, std::move(ready), std::move(checkpoints),sourceVisibilityComplete});
    // New acquisitions already stop while building. Retire our old ownership
    // on the worker so rebuilds do not require two full unserved generations.
    // Existing readers retain their own references and allocation charges.
    queue_.back().retired.swap(published_);
    maintenance_.reset(); // a fresh full build requires a fresh coverage anchor
    activeCancellation_ = std::move(cancelled);
    status_ = {State::QUEUED, status_.generation, {}};
    wake_.notify_one();
    return true;
}
void VectorIndexGeneration::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++revision_;
    if (activeCancellation_) activeCancellation_->store(true);
    status_.state = State::STALE;
}
VectorIndexGeneration::Status VectorIndexGeneration::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
std::shared_ptr<const VectorIndexGeneration::Generation>
VectorIndexGeneration::acquire(VectorFreshness::Stamp certificate) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != State::READY || !published_ ||
        !published_->accepts(certificate)) return {};
    if(published_->visibilityComplete){
        // A commit-only advance is eligible only while the trusted mutation
        // journal is still fully covered. This also fences a reset/gap before
        // the background worker has had a chance to report it.
        if(!maintenance_)return {};
        const auto head=maintenance_->journal->capture();
        if(head.epoch!=maintenance_->cursor.epoch||head.sequence!=maintenance_->cursor.sequence)return {};
    }
    return published_;
}
void VectorIndexGeneration::run() {
    for (;;) {
        if(maintain())continue;
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock,std::chrono::milliseconds(100),[this] {
                return stopped_.load() || !queue_.empty() || maintenanceRequested_;
            });
            maintenanceRequested_=false;
            if (stopped_.load()) {
                // Cancellation from committed collection drop must not join or
                // destroy a full graph on the Raft acknowledgement thread.
                std::shared_ptr<const Generation> retired;
                retired.swap(published_);
                std::deque<Job> abandoned;
                abandoned.swap(queue_);
                lock.unlock();
                abandoned.clear();
                retired.reset();
                return;
            }
            if(queue_.empty())continue;
            job = std::move(queue_.front()); queue_.pop_front();
            if (job.revision != revision_) continue;
            building_ = true;
            status_.state = State::BUILDING;
        }
        bool sourceChanged = false;
        try {
            job.retired.reset();
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(job.config.maxBuildMilliseconds);
            size_t admissionChecks = 0;
            std::optional<VectorFreshness::Stamp> pinned;
            const auto compatible=[&](const std::optional<VectorFreshness::Stamp>& current,const VectorFreshness::Stamp& before){
                return current && (*current==before || (job.sourceVisibilityComplete && current->sameSourceAtOrAfter(before)));
            };
            const auto check = [&] {
                if (admissionChecks++ % 1024 == 0) {
                    if (job.admission) job.admission();
                    if (pinned && !compatible(job.certificate(),*pinned)) {
                        sourceChanged = true;
                        throw std::runtime_error("vector source changed during construction");
                    }
                }
                if (job.cancelled->load()) throw std::runtime_error("vector build cancelled");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("vector_index_build_timeout");
            };
            check();
            const auto certificate = job.certificate();
            if (!certificate) throw std::runtime_error("vector source is changing");
            pinned = certificate;
            struct StoredRecord {
                using Values = std::variant<std::pmr::vector<float>,std::pmr::vector<double>>;
                explicit StoredRecord(std::pmr::memory_resource* resource,bool exact)
                    : id(resource), vector(exact
                        ? Values(std::in_place_type<std::pmr::vector<double>>,resource)
                        : Values(std::in_place_type<std::pmr::vector<float>>,resource)), text(resource) {}
                std::pmr::string id;
                Values vector;
                std::pmr::string text;
                bool hasText{false};
            };
            VectorBudgetResource sourceMemory(sourceByteBudget_, {});
            std::pmr::vector<StoredRecord> records(&sourceMemory);
            std::pmr::unordered_set<std::pmr::string> ids(&sourceMemory);
            const bool stagedGraph=!job.exactOnly&&!job.lexical;
            auto graph=job.exactOnly ? std::shared_ptr<HNSWIndex>{}
                : std::make_shared<HNSWIndex>(job.config,sharedGraphBudget_);
            size_t emitted=0;
            job.source([&](Record record) {
                check();
                if (emitted++ >= job.config.maxElements || record.id.empty() || record.id.size() > 1024 ||
                    record.id.size() > sourceByteBudget_ || record.vector.size() > sourceByteBudget_/sizeof(float) ||
                    record.precise.size() > sourceByteBudget_/sizeof(double) ||
                    (record.text && record.text->size() > sourceByteBudget_) || record.packedRecord.size()>sourceByteBudget_)
                    throw std::runtime_error("vector source budget exceeded");
                if(stagedGraph){
                    if(!record.precise.empty()||record.text||!graph->stage(record.id,record.vector,record.packedRecord,check))
                        throw std::runtime_error("invalid/duplicate staged vector build record");
                    return;
                }
                try {
                    if (!ids.insert(std::pmr::string(record.id, &sourceMemory)).second)
                        throw std::runtime_error("duplicate vector source ID");
                    StoredRecord stored(&sourceMemory,job.exactOnly);
                    stored.id.assign(record.id.data(), record.id.size());
                    if (job.exactOnly) {
                        if (!record.vector.empty()) throw std::runtime_error("flat source must preserve float64");
                        auto& values = std::get<std::pmr::vector<double>>(stored.vector);
                        values.assign(record.precise.begin(),record.precise.end());
                    } else {
                        if (!record.precise.empty()) throw std::runtime_error("unexpected precise HNSW source");
                        auto& values = std::get<std::pmr::vector<float>>(stored.vector);
                        values.assign(record.vector.begin(),record.vector.end());
                    }
                    if (record.text) {
                        stored.text.assign(record.text->data(), record.text->size()); stored.hasText = true;
                    }
                    records.push_back(std::move(stored));
                } catch (const VectorMemoryLimit&) {
                    throw std::runtime_error("vector_source_memory_budget_exceeded");
                }
            }, *job.cancelled);
            pacificdb::test::hitFailpoint("FP_VECTOR_BUILD_SOURCE_COMPLETE", job.revision);
            if (job.admission) job.admission();
            bool restored=false;std::string checkpointError;
            if(stagedGraph){
                if(job.checkpoints.restore){try{restored=job.checkpoints.restore(*graph,check);}catch(const std::exception& e){checkpointError=e.what();check();}}
                if(!restored)graph->buildStaged(check);
                if(!restored&&job.checkpoints.save){try{job.checkpoints.save(*graph,check);}catch(const std::exception& e){checkpointError=e.what();check();}}
            }
            std::shared_ptr<FlatVectorIndex> flat;
            if (job.exactOnly) {
                FlatVectorConfig config;
                config.dimensions = job.config.dimensions; config.maxElements = job.config.maxElements;
                config.maxMemoryBytes = job.config.maxMemoryBytes;
                config.metric = job.config.metric == DistanceMetric::COSINE ? "cosine" :
                    job.config.metric == DistanceMetric::EUCLIDEAN ? "l2" : "dot";
                flat = std::make_shared<FlatVectorIndex>(config,sharedGraphBudget_);
                flat->reserve(records.size());
            }
            auto lexical = job.lexical ? std::make_shared<LexicalIndex>(*job.lexical, sharedGraphBudget_) : nullptr;
            for (const auto& record : records) {
                check();
                const bool empty = std::visit([](const auto& values){return values.empty();},record.vector);
                if (empty && (!lexical || !record.hasText))
                    throw std::runtime_error("invalid empty vector build record");
                const std::string id(record.id);
                if (!empty) {
                    bool inserted = false;
                    if (flat) {
                        const auto& values = std::get<std::pmr::vector<double>>(record.vector);
                        inserted = flat->append(id,std::vector<double>(values.begin(),values.end()));
                    } else {
                        const auto& values = std::get<std::pmr::vector<float>>(record.vector);
                        inserted = graph->insert(id,std::vector<float>(values.begin(),values.end()),check);
                    }
                    if (!inserted) throw std::runtime_error("invalid/duplicate vector build record");
                }
                if (lexical && record.hasText) lexical->upsert(id, std::string(record.text), check);
            }
            if (flat) flat->freeze();
            pacificdb::test::hitFailpoint("FP_VECTOR_BUILD_GRAPH_COMPLETE", job.revision);
            const auto completedCertificate=job.certificate();
            const bool unchanged = compatible(completedCertificate,*certificate);
            pacificdb::test::hitFailpoint("FP_VECTOR_BEFORE_PUBLICATION", job.revision);
            // Releasing the final old graph reference can be O(graph size).
            // Keep that destruction outside the publication mutex.
            std::shared_ptr<const Generation> previous;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                building_ = false;
                if (!unchanged || job.revision != revision_ || stopped_.load()) {
                    status_.state = State::STALE;
                    continue;
                }
                auto generation = std::make_shared<Generation>(Generation{
                    nextGeneration_ + 1, *completedCertificate, std::move(graph), std::move(lexical), std::move(flat),restored,std::move(checkpointError)});
                // Public eligibility waits for Ready to install the covered
                // journal cursor, even if another thread observes READY here.
                generation->visibilityComplete=job.sourceVisibilityComplete;
                previous.swap(published_);
                published_ = std::move(generation);
                pacificdb::test::hitFailpoint("FP_VECTOR_DURING_PUBLICATION", job.revision);
                ++nextGeneration_;
                status_ = {State::READY, nextGeneration_, {}};
            }
            pacificdb::test::hitFailpoint("FP_VECTOR_AFTER_PUBLICATION", job.revision);
            if(job.ready)job.ready(*this);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex_);
            building_ = false;
            if (job.revision != revision_ || sourceChanged) status_.state = State::STALE;
            else status_ = {State::FAILED, status_.generation, e.what()};
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            building_ = false;
            status_ = {State::FAILED, status_.generation, "unknown vector build failure"};
        }
    }
}
}
