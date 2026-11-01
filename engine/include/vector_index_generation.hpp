#pragma once

#include "read_cache.hpp"
#include "flat_vector_index.hpp"
#include "vector_freshness.hpp"
#include "vector_change_journal.hpp"
#include "lexical_index.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <thread>

namespace pacificdb {

// Derived immutable generations with authoritative source certificates. Public
// standalone preview is gated separately from full production qualification.
class VectorIndexGeneration {
public:
    enum class State { ABSENT, QUEUED, BUILDING, READY, STALE, FAILED };
    struct Record { std::string id; std::vector<float> vector; std::optional<std::string> text; std::vector<double> precise; };
    struct Generation {
        uint64_t number;
        VectorFreshness::Stamp certificate;
        std::shared_ptr<const HNSWIndex> graph;
        std::shared_ptr<const LexicalIndex> lexical;
        std::shared_ptr<const FlatVectorIndex> flat;
    };
    struct Status { State state; uint64_t generation; std::string error; };
    using Certificate = std::function<std::optional<VectorFreshness::Stamp>()>;
    // Synchronous producer: never retain/dispatch Emit or the cancellation
    // reference after returning. Capture source dependencies with shared ownership.
    // Check cancellation and never emit more than the source budget.
    using Admission = std::function<void()>;
    using Ready = std::function<void(VectorIndexGeneration&)>;
    using Emit = std::function<void(Record)>;
    using Source = std::function<void(const Emit&, const std::atomic<bool>&)>;

    explicit VectorIndexGeneration(size_t sourceByteBudget);
    VectorIndexGeneration(size_t sourceByteBudget,
                          std::shared_ptr<VectorAllocationBudget> sharedGraphBudget);
    ~VectorIndexGeneration();
    VectorIndexGeneration(const VectorIndexGeneration&) = delete;
    VectorIndexGeneration& operator=(const VectorIndexGeneration&) = delete;
    bool enqueue(HNSWConfig config, Certificate certificate, Source source,
                 std::optional<LexicalConfig> lexical = {}, bool exactOnly = false, Admission admission = {}, Ready ready = {});
    // Trusted opt-in maintenance primitive. The caller must anchor the journal
    // before the full-build certificate and record every committed source edit.
    // Storage integration is separately gated until that coverage is qualified.
    using Resolve = std::function<std::optional<Record>(const std::string&)>;
    void enableMaintenance(std::shared_ptr<VectorChangeJournal> journal,
        VectorChangeJournal::Cursor anchor,Certificate certificate,Resolve resolve,
        size_t maximumBatch = 128,size_t maximumMilliseconds = 1000);
    void invalidate();
    void close(); // permanent nonblocking cancellation; destructor joins the worker
    void closeAndWait();
    bool finished() const noexcept { return finished_.load(); }
    Status status() const;
    // Retains an immutable graph; caller must revalidate certificate after
    // authoritative document fetch and before returning a successful response.
    std::shared_ptr<const Generation> acquire(VectorFreshness::Stamp certificate) const;

private:
    struct Job {
        HNSWConfig config;
        Certificate certificate;
        Source source;
        uint64_t revision;
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::optional<LexicalConfig> lexical;
        bool exactOnly;
        Admission admission;
        std::shared_ptr<const Generation> retired;
        Ready ready;
    };
    struct Maintenance {
        std::shared_ptr<VectorChangeJournal> journal;
        VectorChangeJournal::Cursor cursor;
        Certificate certificate;
        Resolve resolve;
        size_t maximumBatch,maximumMilliseconds;
    };
    bool maintain();
    void run();
    size_t sourceByteBudget_;
    std::shared_ptr<VectorAllocationBudget> sharedGraphBudget_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> finished_{false};
    uint64_t revision_{0}, nextGeneration_{0};
    bool building_{false};
    std::shared_ptr<std::atomic<bool>> activeCancellation_;
    Status status_{State::ABSENT, 0, {}};
    std::shared_ptr<const Generation> published_;
    std::shared_ptr<Maintenance> maintenance_;
    std::thread worker_;
    std::mutex joinMutex_;
};
}
