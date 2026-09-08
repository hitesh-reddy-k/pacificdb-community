#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <future>
#include <map>
#include <random>
#include <unordered_map>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Production-grade Raft replication modes
enum class ReplicationMode {
    SYNCHRONOUS,      // Wait for majority quorum
    ASYNCHRONOUS,     // Fire-and-forget background replication
    HYBRID_ADAPTIVE   // Dynamic switching based on load/health
};

// Operation priority levels
enum class OperationPriority {
    CRITICAL,    // Financial transactions, user auth
    HIGH,        // Business logic, orders
    NORMAL,      // General data operations
    LOW,         // Analytics, logging, bulk operations
    BULK         // Large batch operations
};

// Circuit breaker states
enum class CircuitState {
    CLOSED,      // Normal operation
    OPEN,        // Failing, reject requests
    HALF_OPEN    // Testing recovery
};

// System health metrics
struct SystemHealth {
    double cpuUsage = 0.0;
    double memoryUsage = 0.0;
    size_t queueDepth = 0;
    size_t activeConnections = 0;
    std::chrono::milliseconds avgResponseTime{0};
    bool networkHealthy = true;
    size_t failedRequests = 0;
    std::chrono::steady_clock::time_point lastHealthCheck;

    // Queue depths by priority
    size_t criticalQueueDepth = 0;
    size_t highQueueDepth = 0;
    size_t normalQueueDepth = 0;
    size_t lowQueueDepth = 0;
    size_t bulkQueueDepth = 0;

    // Performance metrics
    double requestsPerSecond = 0.0;
    double errorsPerSecond = 0.0;

    // System state
    CircuitState circuitBreakerState = CircuitState::CLOSED;
    ReplicationMode currentReplicationMode = ReplicationMode::HYBRID_ADAPTIVE;
    double healthScore = 100.0;
    bool isHealthy = true;
    // active worker count
    size_t activeWorkers = 0;
};

class RaftCore {
public:
    static RaftCore& instance();

    // Initialize raft with peers in host:port format, node id, listen port
    void init(const std::vector<std::string>& peers, const std::string& nodeId, int listenPort, bool startAsLeader=false);
    void start();
    void stop();

    bool isLeader() const { return leader_.load(); }
    bool isLeaderEligible() const { return leaderEligible_; }
    bool isEnabled() const { return initialized_.load(); }
    bool isRaftListenerRunning() const { return raftListenerRunning_.load(); }
    void setCleanShutdownRecovery(bool clean) {
        cleanShutdownRecovery_.store(clean, std::memory_order_release);
    }

    // Production-grade replication with priority and adaptive behavior
    bool replicateAndApply(const json& entry,
                          OperationPriority priority = OperationPriority::NORMAL,
                          int timeoutMs = 30000);

    // Linearizable read fence: verifies current leadership with a quorum before serving STRONG reads.
    bool strongReadBarrier(int timeoutMs = 1200,
                           uint64_t* observedTerm = nullptr,
                           uint64_t* observedCommitIndex = nullptr);

    // Lightweight Raft state accessors for trace metadata.
    uint64_t getCurrentTerm();
    uint64_t getCommitIndex();
    uint64_t getLastIndex();
    uint64_t getLastLogTerm();
    uint64_t getLastApplied();
    // Capture a stable state-machine image for backup without compacting the
    // live Raft log. Returns the included index, or zero on failure.
    uint64_t createBackupSnapshot();
    static json decodePersistedLogPayload(const std::string& payload);
    static bool validatePersistedSnapshot(const json& snapshot, std::string& reason);
    static bool readPersistedSnapshotMetadata(
        const std::string& snapshotPath,
        uint64_t& lastIncludedIndex,
        uint64_t& lastIncludedTerm,
        std::string& reason);
    long long getAmbiguousLateCommitCount();
    std::string getNodeId();
    std::string getVotedFor();
    std::string getLeaderId();
    std::string getRole();
    // v3.8: monotonically observe a higher leader term from an external/control source
    // (control plane pushes an elected term in for in-core stale-leader fencing).
    void observeHigherTerm(uint64_t newTerm);
    uint64_t getLastHeartbeatMs() const;
    uint64_t millisSinceLastHeartbeat() const;
    bool applyCommittedUpTo(uint64_t upToIndex);
    // V11.4-DIV-001: a node holding an unapplied committed entry is not recovered, no
    // matter what the replay loop concluded. Readiness and every strong-read fence
    // already flow through this predicate, so blocking it here refuses strong reads and
    // readiness from one place rather than at each call site.
    bool isRecoveryComplete() const {
        return recoveryComplete_.load() && !applyBlocked_.load(std::memory_order_acquire);
    }

    // ═══ V11.4-DIV-001: fail-closed apply ═══
    // A committed entry that fails to apply must never be stepped over. Callers that
    // previously discarded the applyReplicatedEntry() result advanced progress past a
    // failed entry, which left replicas permanently divergent while every progress
    // marker still agreed. When apply is blocked the node must not advance any progress
    // marker, must not report recovery complete, must not become ready, and must not
    // serve strong reads until an operator resolves it.
    bool isApplyBlocked() const { return applyBlocked_.load(std::memory_order_acquire); }
    void blockApply(uint64_t index, uint64_t term, const std::string& commandType,
                    const std::string& logicalOperationId, const std::string& error,
                    bool mutationMayHaveOccurred);
    nlohmann::json applyBlockedStatus() const;

    // ═══ V11.4-DIV-001 P0.6: divergence fencing ═══
    // A node that was asked to truncate committed history holds a log that
    // irreconcilably conflicts with the leader's committed history. Until an
    // operator resolves it the node must refuse strong reads and must not be
    // used as a backup or repair source.
    bool isDivergent() const { return divergenceDetected_.load(std::memory_order_acquire); }
    // Single predicate for every consumer that needs "this node's state is
    // complete and trustworthy": strong reads, readiness, backup sourcing.
    bool strongReadsAllowed() const {
        return isRecoveryComplete() && !isDivergent();
    }
    // Durable safety audit trail: one fsynced JSON line per event under
    // <dataRoot>/raft/safety_audit.jsonl. Never throws.
    void recordSafetyAudit(const std::string& event, const nlohmann::json& detail) noexcept;

    // ═══ RAFT HARDENING: Quorum health and leader lease ═══
    // hasQuorum(): true if leader heard from N/2 peers within electionTimeout
    bool hasQuorum();
    // isLeaseValid(): true if leader's lease hasn't expired (based on heartbeat acks)
    bool isLeaseValid() const;
    // getQuorumHealth(): detailed quorum diagnostics for monitoring
    json getQuorumHealth();
    // Per-peer heartbeat tracking
    uint64_t getLastPeerHeartbeatMs(const std::string& peer) const;
    // Access peer list for cluster topology checks
    const std::vector<std::string>& peers() const { return peers_; }
    size_t peerCount() const { return peers_.size(); }

    // Dynamic configuration
    void setReplicationMode(ReplicationMode mode);
    void setAdaptiveThresholds(double highLoadThreshold = 0.8,
                              double criticalLoadThreshold = 0.95,
                              size_t maxQueueDepth = 10000);

    // Health monitoring
    SystemHealth getSystemHealth();
    bool isSystemHealthy();

    // Circuit breaker control
    void resetCircuitBreaker();
    CircuitState getCircuitState() const;
    json getWriteReplicationMetrics() const;

    // Load management
    size_t getPendingOperations() const;
    bool isUnderHighLoad() const;

private:
    enum class ApplySource {
        WORKER,
        APPEND_ENTRIES,
        LEADER_BYPASS,
        LEADER_ASYNC,
        RECOVERY,
        STARTUP,
        STRONG_READ,
        SNAPSHOT
    };

    RaftCore();
    ~RaftCore();

    // Core configuration
    std::vector<std::string> peers_;
    std::string nodeId_;
    int listenPort_ = 0;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::mutex shutdownMutex_;
    std::condition_variable shutdownCv_;
    std::atomic<bool> leader_{false};
    bool leaderEligible_ = true;
    std::atomic<bool> raftListenerRunning_{false};
    std::atomic<std::intptr_t> raftListenerSocket_{-1};
    std::atomic<bool> leadershipReady_{true};
    // Background storage maintenance starts before Raft initialization. Keep
    // recovery closed until replay has restored the durable commit watermark.
    std::atomic<bool> recoveryComplete_{false};
    // Set by startup only when the durable clean marker was consumed. In that
    // case checksummed lastApplied proves the already-flushed state-machine
    // prefix and recovery validates, but need not mutate, that prefix again.
    std::atomic<bool> cleanShutdownRecovery_{false};
    // V11.4-DIV-001 P0.6: sticky divergence fence. Set when this node is asked to
    // truncate committed history (leader/committed-log conflict). Cleared only by
    // operator-driven repair (node replacement / snapshot reinstall).
    std::atomic<bool> divergenceDetected_{false};
    mutable std::mutex safetyAuditMutex_;
    // V11.4-DIV-001 fail-closed apply state. Sticky by design: once a committed entry
    // fails to apply, the node stays blocked rather than silently continuing.
    std::atomic<bool> applyBlocked_{false};
    mutable std::mutex applyBlockedMutex_;
    uint64_t applyBlockedIndex_{0};
    uint64_t applyBlockedTerm_{0};
    std::string applyBlockedCommandType_;
    std::string applyBlockedLogicalOperationId_;
    std::string applyBlockedError_;
    bool applyBlockedMutationMayHaveOccurred_{false};
    // v5.5P-R2B FD/connection counters for leak observability.
    std::atomic<long> raftOpenConnections_{0};
    std::atomic<long> raftTotalAccepted_{0};
    std::atomic<long> raftTotalClosed_{0};
    std::atomic<long> raftAcceptErrors_{0};
    std::atomic<long> raftEmfileErrors_{0};
    // v5.5P-R2B bounded handler-thread pool (caps concurrent fds/threads).
    std::mutex raftWorkerMutex_;
    std::condition_variable raftWorkerCv_;
    int raftActiveWorkers_{0};
    int raftMaxWorkers_{16};
public:
    long getRaftOpenConnections() const { return raftOpenConnections_.load(); }
    long getRaftTotalAccepted() const { return raftTotalAccepted_.load(); }
    long getRaftTotalClosed() const { return raftTotalClosed_.load(); }
    long getRaftAcceptErrors() const { return raftAcceptErrors_.load(); }
    long getRaftEmfileErrors() const { return raftEmfileErrors_.load(); }
private:

    // Core features
    std::atomic<ReplicationMode> replicationMode_{ReplicationMode::HYBRID_ADAPTIVE};
    std::atomic<CircuitState> circuitState_{CircuitState::CLOSED};

    // Adaptive thresholds
    std::atomic<double> highLoadThreshold_{0.8};
    std::atomic<double> criticalLoadThreshold_{0.95};
    std::atomic<size_t> maxQueueDepth_{10000};

    // Health monitoring
    mutable std::mutex healthMutex_;
    SystemHealth systemHealth_;
    std::chrono::seconds healthCheckInterval_{5};

    // Priority queues for different operation types
    struct PriorityEntry {
        json entry;
        OperationPriority priority;
        std::chrono::steady_clock::time_point submitted;
        std::promise<bool> result;
        int timeoutMs;
    };

    mutable std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::queue<std::shared_ptr<PriorityEntry>> criticalQueue_;
    std::queue<std::shared_ptr<PriorityEntry>> highQueue_;
    std::queue<std::shared_ptr<PriorityEntry>> normalQueue_;
    std::queue<std::shared_ptr<PriorityEntry>> lowQueue_;
    std::queue<std::shared_ptr<PriorityEntry>> bulkQueue_;

    // Worker threads
    std::vector<std::thread> workerThreads_;
    std::atomic<size_t> activeWorkers_{0};
    std::atomic<size_t> minWorkers_{1};

    // Circuit breaker
    std::atomic<size_t> consecutiveFailures_{0};
    std::atomic<size_t> circuitBreakerThreshold_{10};
    std::chrono::steady_clock::time_point circuitOpenTime_;
    std::chrono::seconds circuitRecoveryTimeout_{30};
    std::atomic<size_t> successThreshold_{5}; // Success threshold for half-open recovery

    // Adaptive thresholds
    std::atomic<double> errorRateThreshold_{5.0}; // Errors per second threshold
    std::atomic<size_t> maxWorkers_{1};
    ReplicationMode adaptiveMode_{ReplicationMode::ASYNCHRONOUS};

    // Health monitoring
    SystemHealth health_;
    std::chrono::steady_clock::time_point lastMetricsUpdate_;
    std::chrono::steady_clock::time_point lastFailureTime_;

    // Performance metrics
    std::atomic<size_t> totalRequests_{0};
    std::atomic<size_t> successfulRequests_{0};
    std::atomic<size_t> failedRequests_{0};
    std::atomic<size_t> healthRejectedRequests_{0};
    std::atomic<size_t> timeoutRequests_{0};
    std::chrono::steady_clock::time_point lastRequestTime_;

    // Threads
    std::thread listenerThread_;
    std::thread monitorThread_;
    std::thread snapshotThread_;
    std::thread healthMonitorThread_;
    std::thread adaptiveControllerThread_;

    // Legacy fields (kept for compatibility)
    std::atomic<bool> replicationAsync_{false};
    std::mutex logMutex_;

    // Core Raft state
    uint64_t lastIndex_ = 0;
    // Term of the last appended log entry (used for election log-up-to-date checks)
    uint64_t lastEntryTerm_ = 0;
    uint64_t currentTerm_ = 0;
    // Last committed index (leader advances when majority replicate).
    // v5.5P-R6.10: atomic so getCommitIndex() (called by EVERY read for the visibility check)
    // can read it lock-free. It previously locked electionMutex_, serializing all reads behind
    // the commit/apply/replication path -> read P99 ballooned to ~2.8s at 500 users/shard
    // (idle reads are ~2ms). Writers still mutate it under electionMutex_; the atomic just lets
    // readers load it without that lock.
    std::atomic<uint64_t> commitIndex_{0};
    // Last applied index (state machine)
    std::atomic<uint64_t> lastApplied_{0};
    // Byte offset in raft log for last applied entry (best-effort)
    std::atomic<uint64_t> lastAppliedOffset_{0};
    // Strong-read lease cache (optional, controlled by env)
    std::atomic<uint64_t> lastBarrierAtMs_{0};
    std::atomic<uint64_t> lastBarrierTerm_{0};
    std::atomic<uint64_t> lastBarrierCommit_{0};
    // Last snapshot index written (used for compaction/install)
    uint64_t lastSnapshotIndex_ = 0;
    // v5.5P-R2B: term of lastSnapshotIndex_ so getTermForIndex() can answer
    // prevLogTerm queries at the compaction boundary (entries <= lastSnapshotIndex_
    // are removed from log.bin; without this, getTermForIndex() returns 0 and every
    // append_entries with prevLogIndex==lastSnapshotIndex_ is rejected as a phantom
    // prev_term_conflict, which over a real network stalls replication for the full
    // op timeout).
    uint64_t lastSnapshotTerm_ = 0;
    std::chrono::steady_clock::time_point lastSnapshotTime_;
    // Random generator for election jitter
    std::mt19937 rng_;
    std::atomic<uint64_t> lastHeartbeat_{0};
    std::atomic<uint64_t> firstSuccessfulPeerContact_{0};  // Track when we first contacted a peer
    std::mutex electionMutex_;
    // Serializes the checksummed consensus-state replacement. Term/vote and
    // commit/apply progress must survive a power cut as one coherent record.
    std::mutex consensusStatePersistMutex_;
    bool consensusStateLoaded_ = false;
    uint64_t persistedConsensusTerm_ = 0;
    uint64_t persistedCommitIndex_ = 0;
    uint64_t persistedLastApplied_ = 0;
    std::string persistedVotedFor_;
    std::string votedFor_;
    std::string leaderId_;
    // Per-peer replication tracking
    std::unordered_map<std::string, uint64_t> nextIndex_;
    std::unordered_map<std::string, uint64_t> matchIndex_;
    std::mutex replicaIndexMutex_;
    // ═══ RAFT HARDENING: Per-peer heartbeat tracking for lease/quorum ═══
    // Timestamp (steady_clock ms) of last successful heartbeat ack from each peer
    std::unordered_map<std::string, uint64_t> peerLastHeartbeatMs_;
    mutable std::mutex peerHeartbeatMutex_;
    // Leader lease duration (default: 2x election timeout)
    int leaseTimeoutMs_ = 6400; // 2 * 3200ms default election timeout
    // Snapshot configuration
    std::chrono::seconds snapshotIntervalSec_{300};
    uint64_t snapshotThresholdEntries_ = 100000;
    std::mutex snapshotMutex_;
    std::mutex replicationMutex_;
    std::mutex applyMutex_;
    std::thread applyWorkerThread_;
    std::mutex applyWorkerMutex_;
    std::condition_variable applyWorkerCv_;
    std::atomic<uint64_t> applyWorkerTarget_{0};
    std::atomic<bool> applyWorkerActive_{false};

    // Batched read-barrier ("ReadIndex") rounds. Concurrent strong reads share one
    // quorum-confirmation round instead of each issuing its own heartbeat fan-out, so
    // barrier RPC volume tracks round count rather than read throughput.
    std::thread barrierRoundThread_;
    std::mutex barrierRoundMutex_;
    std::condition_variable barrierRoundCv_;
    uint64_t barrierRoundCompleted_ = 0;
    bool barrierRoundActive_ = false;
    int barrierRoundWaiters_ = 0;
    bool barrierRoundLastOk_ = false;
    uint64_t barrierRoundLastTerm_ = 0;
    uint64_t barrierRoundLastCommit_ = 0;
    void barrierRoundLoop();
    bool runBarrierRound(uint64_t& termOut, uint64_t& commitOut);

    // index -> term cache backing getTermForIndex(). Without it every AppendEntries
    // (leader prevLogTerm, follower conflict check) linearly scans raft/log.bin, so write
    // latency grows with log size. Entries are immutable once appended; the cache is
    // therefore only invalidated where log.bin is rewritten (compaction, trim, follower
    // conflict truncation).
    mutable std::mutex termCacheMutex_;
    std::unordered_map<uint64_t, uint64_t> termCache_;
    std::unordered_map<uint64_t, uint64_t> logOffsetCache_;
    void cacheIndexTerm(uint64_t index, uint64_t term);
    void cacheIndexTermAndOffset(uint64_t index, uint64_t term, uint64_t offset);
    void cacheRaftRecordBlock(const std::string& bytes, uint64_t startOffset);
    void invalidateTermCache();
    struct QuorumWaiter {
        uint64_t targetIndex = 0;
        std::promise<bool> promise;
        std::atomic<bool> completed{false};
    };
    struct ApplyWaiter {
        uint64_t targetIndex = 0;
        std::promise<bool> promise;
        std::atomic<bool> completed{false};
    };
    mutable std::mutex quorumWaiterMutex_;
    std::map<uint64_t, std::vector<std::shared_ptr<QuorumWaiter>>> quorumWaitersByTarget_;
    std::atomic<uint64_t> quorumReachedIndex_{0};
    std::atomic<long long> quorumWaiterNotifyCount_{0};
    std::atomic<long long> quorumWaiterWakeups_{0};
    std::atomic<long long> quorumWaiterSatisfied_{0};
    std::atomic<long long> quorumWaiterTimeouts_{0};
    std::atomic<long long> quorumWaitNotifyAllCount_{0};
    mutable std::mutex applyWaiterMutex_;
    std::map<uint64_t, std::vector<std::shared_ptr<ApplyWaiter>>> applyWaitersByTarget_;
    std::atomic<long long> applyWaiterNotifyCount_{0};
    std::atomic<long long> applyWaiterWakeups_{0};
    std::atomic<long long> applyWaiterSatisfied_{0};
    std::atomic<long long> applyWaiterTimeouts_{0};
    std::atomic<long long> applyWaitNotifyAllCount_{0};

    // Private methods
    void loadPersistedState();
    void persistConsensusState(uint64_t term,
                               const std::string& votedFor,
                               uint64_t commitIndex,
                               uint64_t lastApplied,
                               bool updateElectionState);
    void persistProgress();
    void persistCurrentTerm();
    // Caller holds electionMutex_; only a new term may clear the durable vote.
    void adoptLeaderTermLocked(uint64_t term);
    void persistVotedFor();

    void monitorLoop();
    void snapshotLoop();
    void healthMonitorLoop();
    void adaptiveControllerLoop();
    bool waitForShutdown(std::chrono::milliseconds delay);
    void workerLoop();
    void resizeWorkerPool(size_t newSize);
    void restartWorkers();
    void adaptReplicationMode();
    void adaptWorkerPool();
    void adaptCircuitBreaker();
    int calculateHealthScore();

    bool attemptElection();
    void runListener();
    void handleFollowerConn(int clientSock, const std::atomic<size_t>& pendingConnections);
    // Check if a peer address is the current node itself
    bool isSelfPeer(const std::string& peer) const;
    int clusterNodeCount() const;
    int quorumSize() const;
    // Send payload to peer, set acked=true if peer acknowledged. If respStr provided, fills with raw response.
    // v5.5P-R5.6: optional persistentSock reuses one long-lived connection across calls. When
    // non-null: *persistentSock>=0 is reused (no connect); on success it stays open; on any
    // failure it is closed and set to -1 so the caller reconnects on the next round.
    bool sendToPeer(const std::string& peer, const json& payload, int timeoutMs, bool& acked,
                    std::string* respStr = nullptr, int* persistentSock = nullptr);
    // Read term for a given log index from binary raft log (returns 0 if not found)
    uint64_t getTermForIndex(uint64_t index);
    // Read raw Raft log payloads from the binary indexed log, inclusive.
    std::vector<nlohmann::json> readRaftEntriesRange(
        uint64_t fromIndex,
        uint64_t toIndex,
        std::vector<uint64_t>* entryTerms = nullptr);
    // Leader-side catch-up for a peer that reports matchIndex behind commitIndex.
    bool backfillCommittedEntriesToPeer(const std::string& peer,
                                       uint64_t peerMatchIndex,
                                       uint64_t leaderCommitIndex,
                                       uint64_t term,
                                       int timeoutMs);
    bool sendSnapshotToPeer(const std::string& peer,
                            uint64_t term,
                            int timeoutMs,
                            std::string& response);
    // Create a snapshot at or including the given index. Writes .snapshot to dataRoot
    bool createSnapshot(uint64_t lastIncludedIndex,
                        const std::filesystem::path& pinnedRoot);
    // Compact the binary raft log and jsonl raft log up to lastIncludedIndex
    bool compactRaftLog(uint64_t lastIncludedIndex);
    // Trim the binary raft log to at most the given index
    bool trimRaftLog(uint64_t maxIndex);
    std::string dataRoot();

    // Persistence and recovery
    std::vector<nlohmann::json> loadRaftLog();
    void replayRaftLog();
    bool applyCommittedEntries(uint64_t upToIndex, ApplySource source);
    bool advanceLastApplied(uint64_t newIndex,
                            ApplySource source,
                            bool allowSnapshotJump = false);
    void persistAppliedProgress(uint64_t appliedIndex);
    static const char* applySourceName(ApplySource source);
    void applyWorkerLoop();
    void publishApplyTarget(uint64_t upToIndex);
    void publishAppliedIndex();
    bool waitForAppliedIndex(uint64_t targetIndex, int timeoutMs);
    bool applyCommittedOrWait(uint64_t upToIndex,
                              uint64_t waitForIndex,
                              int timeoutMs,
                              ApplySource source);

    // Non-blocking apply scheduling for follower RPC acknowledgement paths.
    void scheduleFollowerApply(uint64_t upToIndex);

    // Production methods
    ReplicationMode determineReplicationMode(const json& entry, OperationPriority priority);
    bool shouldUseSynchronous(const json& entry, OperationPriority priority);
    void updateHealthMetrics();
    void handleCircuitBreaker();
    bool checkSystemHealth();
    size_t getQueueDepth() const;
    void enqueueOperation(std::shared_ptr<PriorityEntry> entry);
    std::shared_ptr<PriorityEntry> dequeueOperation();
    bool processOperation(std::shared_ptr<PriorityEntry> entry);
    bool processBatch(const std::vector<std::shared_ptr<PriorityEntry>>& batch);
    bool replicateSynchronousBatch(const std::vector<std::shared_ptr<PriorityEntry>>& batch, int timeoutMs);
    bool batchEligible(const std::shared_ptr<PriorityEntry>& entry) const;
    void collectBatch(std::vector<std::shared_ptr<PriorityEntry>>& batch);
    bool replicateSynchronous(const json& entry, int timeoutMs);
    // v5.5P-R4.2.2: ensure one long-lived coalescing replicator per peer. Writers publish
    // a target index and wait on the shared match-advanced CV; the replicator streams the
    // contiguous log suffix to the follower in bounded chunks. Used by both the single-write
    // and the group-commit batch paths so neither spawns per-write/per-batch peer workers.
    void ensurePeerReplicator(const std::string& peer, uint64_t term, uint64_t targetIndex);
    uint64_t computeQuorumIndex(long long* replicaIndexMutexWaitUs = nullptr);
    bool waitForQuorumIndex(uint64_t targetIndex, int needed, int timeoutMs, int* observedAcks);
    int reachedCountForIndex(uint64_t targetIndex, long long* replicaIndexMutexWaitUs = nullptr);
    // v5.5P-R4.5: publish the highest quorum-replicated index and complete only
    // waiters whose target is now satisfied.
    void publishQuorumIndex();
    bool replicateAsynchronous(const json& entry);
    void logOperationMetrics(const json& entry, bool success, std::chrono::milliseconds duration);
};
