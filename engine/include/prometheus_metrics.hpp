#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace pacificdb {
namespace observability {

// ============================================================================
// METRIC TYPES
// ============================================================================

enum class MetricType {
    COUNTER,      // Monotonically increasing
    GAUGE,        // Can go up/down
    HISTOGRAM,    // Distribution of values
    SUMMARY       // Quantile summaries
};

struct MetricLabel {
    std::string name;
    std::string value;
};

// ============================================================================
// COUNTER METRIC
// ============================================================================

class Counter {
public:
    Counter(const std::string& name, const std::string& help,
            const std::vector<std::string>& labelNames = {})
        : name_(name), help_(help), labelNames_(labelNames) {}

    void inc(double value = 1.0, const std::vector<std::string>& labelValues = {}) {
        std::string key = makeLabelKey(labelValues);
        std::lock_guard<std::mutex> lock(mutex_);
        values_[key] += value;
    }

    double get(const std::vector<std::string>& labelValues = {}) const {
        std::string key = makeLabelKey(labelValues);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(key);
        return it != values_.end() ? it->second : 0.0;
    }

    std::string toPrometheus() const {
        std::stringstream ss;
        ss << "# HELP " << name_ << " " << help_ << "\n";
        ss << "# TYPE " << name_ << " counter\n";

        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, value] : values_) {
            ss << name_;
            if (!key.empty()) ss << "{" << key << "}";
            ss << " " << std::fixed << std::setprecision(2) << value << "\n";
        }
        return ss.str();
    }

private:
    std::string makeLabelKey(const std::vector<std::string>& values) const {
        if (values.empty() || labelNames_.empty()) return "";
        std::stringstream ss;
        for (size_t i = 0; i < std::min(labelNames_.size(), values.size()); ++i) {
            if (i > 0) ss << ",";
            ss << labelNames_[i] << "=\"" << values[i] << "\"";
        }
        return ss.str();
    }

    std::string name_;
    std::string help_;
    std::vector<std::string> labelNames_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> values_;
};

// ============================================================================
// GAUGE METRIC
// ============================================================================

class Gauge {
public:
    Gauge(const std::string& name, const std::string& help,
          const std::vector<std::string>& labelNames = {})
        : name_(name), help_(help), labelNames_(labelNames) {}

    void set(double value, const std::vector<std::string>& labelValues = {}) {
        std::string key = makeLabelKey(labelValues);
        std::lock_guard<std::mutex> lock(mutex_);
        values_[key] = value;
    }

    void inc(double value = 1.0, const std::vector<std::string>& labelValues = {}) {
        std::string key = makeLabelKey(labelValues);
        std::lock_guard<std::mutex> lock(mutex_);
        values_[key] += value;
    }

    void dec(double value = 1.0, const std::vector<std::string>& labelValues = {}) {
        std::string key = makeLabelKey(labelValues);
        std::lock_guard<std::mutex> lock(mutex_);
        values_[key] -= value;
    }

    double get(const std::vector<std::string>& labelValues = {}) const {
        std::string key = makeLabelKey(labelValues);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(key);
        return it != values_.end() ? it->second : 0.0;
    }

    std::string toPrometheus() const {
        std::stringstream ss;
        ss << "# HELP " << name_ << " " << help_ << "\n";
        ss << "# TYPE " << name_ << " gauge\n";

        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, value] : values_) {
            ss << name_;
            if (!key.empty()) ss << "{" << key << "}";
            ss << " " << std::fixed << std::setprecision(2) << value << "\n";
        }
        return ss.str();
    }

private:
    std::string makeLabelKey(const std::vector<std::string>& values) const {
        if (values.empty() || labelNames_.empty()) return "";
        std::stringstream ss;
        for (size_t i = 0; i < std::min(labelNames_.size(), values.size()); ++i) {
            if (i > 0) ss << ",";
            ss << labelNames_[i] << "=\"" << values[i] << "\"";
        }
        return ss.str();
    }

    std::string name_;
    std::string help_;
    std::vector<std::string> labelNames_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, double> values_;
};

// ============================================================================
// HISTOGRAM METRIC
// ============================================================================

class Histogram {
public:
    Histogram(const std::string& name, const std::string& help,
              const std::vector<double>& buckets = defaultBuckets(),
              const std::vector<std::string>& labelNames = {})
        : name_(name), help_(help), buckets_(buckets), labelNames_(labelNames) {
        std::sort(buckets_.begin(), buckets_.end());
    }

    void observe(double value, const std::vector<std::string>& labelValues = {}) {
        std::string key = makeLabelKey(labelValues);
        std::lock_guard<std::mutex> lock(mutex_);

        auto& data = histograms_[key];
        data.sum += value;
        data.count++;

        for (size_t i = 0; i < buckets_.size(); ++i) {
            if (value <= buckets_[i]) {
                if (data.bucketCounts.size() <= i) {
                    data.bucketCounts.resize(buckets_.size(), 0);
                }
                data.bucketCounts[i]++;
            }
        }
    }

    std::string toPrometheus() const {
        std::stringstream ss;
        ss << "# HELP " << name_ << " " << help_ << "\n";
        ss << "# TYPE " << name_ << " histogram\n";

        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, data] : histograms_) {
            std::string labels = key.empty() ? "" : key + ",";

            // Cumulative bucket counts
            uint64_t cumulative = 0;
            for (size_t i = 0; i < buckets_.size() && i < data.bucketCounts.size(); ++i) {
                cumulative += data.bucketCounts[i];
                ss << name_ << "_bucket{" << labels << "le=\"" << buckets_[i] << "\"} "
                   << cumulative << "\n";
            }
            ss << name_ << "_bucket{" << labels << "le=\"+Inf\"} " << data.count << "\n";

            ss << name_ << "_sum";
            if (!key.empty()) ss << "{" << key << "}";
            ss << " " << std::fixed << std::setprecision(4) << data.sum << "\n";

            ss << name_ << "_count";
            if (!key.empty()) ss << "{" << key << "}";
            ss << " " << data.count << "\n";
        }
        return ss.str();
    }

    static std::vector<double> defaultBuckets() {
        return {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
    }

private:
    struct HistogramData {
        double sum = 0;
        uint64_t count = 0;
        std::vector<uint64_t> bucketCounts;
    };

    std::string makeLabelKey(const std::vector<std::string>& values) const {
        if (values.empty() || labelNames_.empty()) return "";
        std::stringstream ss;
        for (size_t i = 0; i < std::min(labelNames_.size(), values.size()); ++i) {
            if (i > 0) ss << ",";
            ss << labelNames_[i] << "=\"" << values[i] << "\"";
        }
        return ss.str();
    }

    std::string name_;
    std::string help_;
    std::vector<double> buckets_;
    std::vector<std::string> labelNames_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, HistogramData> histograms_;
};

// ============================================================================
// METRICS REGISTRY
// ============================================================================

class MetricsRegistry {
public:
    static MetricsRegistry& instance() {
        static MetricsRegistry inst;
        return inst;
    }

    // Counter factory
    Counter& counter(const std::string& name, const std::string& help,
                     const std::vector<std::string>& labelNames = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counters_.find(name);
        if (it == counters_.end()) {
            counters_.emplace(name, std::make_unique<Counter>(name, help, labelNames));
        }
        return *counters_[name];
    }

    // Gauge factory
    Gauge& gauge(const std::string& name, const std::string& help,
                 const std::vector<std::string>& labelNames = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gauges_.find(name);
        if (it == gauges_.end()) {
            gauges_.emplace(name, std::make_unique<Gauge>(name, help, labelNames));
        }
        return *gauges_[name];
    }

    // Histogram factory
    Histogram& histogram(const std::string& name, const std::string& help,
                         const std::vector<double>& buckets = Histogram::defaultBuckets(),
                         const std::vector<std::string>& labelNames = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = histograms_.find(name);
        if (it == histograms_.end()) {
            histograms_.emplace(name, std::make_unique<Histogram>(name, help, buckets, labelNames));
        }
        return *histograms_[name];
    }

    // Export all metrics in Prometheus format
    std::string toPrometheus() const {
        std::stringstream ss;

        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& [_, counter] : counters_) {
            ss << counter->toPrometheus() << "\n";
        }
        for (const auto& [_, gauge] : gauges_) {
            ss << gauge->toPrometheus() << "\n";
        }
        for (const auto& [_, histogram] : histograms_) {
            ss << histogram->toPrometheus() << "\n";
        }

        return ss.str();
    }

    // Export as JSON
    json toJson() const {
        json result;

        std::lock_guard<std::mutex> lock(mutex_);

        result["counters"] = json::object();
        for (const auto& [name, counter] : counters_) {
            result["counters"][name] = counter->get();
        }

        result["gauges"] = json::object();
        for (const auto& [name, gauge] : gauges_) {
            result["gauges"][name] = gauge->get();
        }

        return result;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.clear();
        gauges_.clear();
        histograms_.clear();
    }

private:
    MetricsRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;
};

// ============================================================================
// PACIFICDB SPECIFIC METRICS
// ============================================================================

class PacificDBMetrics {
public:
    static PacificDBMetrics& instance() {
        static PacificDBMetrics inst;
        return inst;
    }

    // Database operations
    void recordOperation(const std::string& operation, const std::string& database,
                         double durationSeconds, bool success) {
        operationsTotal_.inc(1.0, {operation, database, success ? "success" : "failure"});
        operationDuration_.observe(durationSeconds, {operation});
    }

    // Connection metrics
    void connectionOpened() { activeConnections_.inc(); }
    void connectionClosed() { activeConnections_.dec(); }
    void setActiveConnections(double count) { activeConnections_.set(count); }

    // Raft metrics
    void setRaftState(const std::string& nodeId, const std::string& state) {
        raftState_.set(state == "leader" ? 1 : 0, {nodeId, state});
    }
    void recordRaftElection() { raftElections_.inc(); }
    void recordRaftCommit(double latencySeconds) {
        raftCommitLatency_.observe(latencySeconds);
    }

    // Storage metrics
    void setStorageSize(const std::string& database, double bytes) {
        storageBytes_.set(bytes, {database});
    }
    void setDocumentCount(const std::string& database, const std::string& collection,
                          double count) {
        documentCount_.set(count, {database, collection});
    }
    void recordCompaction(double durationSeconds, double bytesReclaimed) {
        compactionDuration_.observe(durationSeconds);
        compactionBytesReclaimed_.inc(bytesReclaimed);
    }

    // WAL metrics
    void recordWALWrite(double bytes) { walBytesWritten_.inc(bytes); }
    void setWALSize(double bytes) { walCurrentSize_.set(bytes); }
    void recordWALSync(double durationSeconds) {
        walSyncDuration_.observe(durationSeconds);
    }

    // Shard metrics
    void setShardCount(double count) { shardCount_.set(count); }
    void recordShardRebalance(double durationSeconds) {
        shardRebalanceDuration_.observe(durationSeconds);
    }
    void setShardDocuments(const std::string& shardId, double count) {
        shardDocumentCount_.set(count, {shardId});
    }

    // Query metrics
    void recordQuery(const std::string& type, double durationSeconds, bool indexUsed) {
        queryTotal_.inc(1.0, {type, indexUsed ? "indexed" : "scan"});
        queryDuration_.observe(durationSeconds, {type});
    }

    // Replication metrics
    void setReplicationLag(const std::string& follower, double lagSeconds) {
        replicationLag_.set(lagSeconds, {follower});
    }
    void recordReplication(double bytes) { replicationBytesTransferred_.inc(bytes); }

    // Backup metrics
    void recordBackup(double durationSeconds, double sizeBytes, bool success) {
        backupTotal_.inc(1.0, {success ? "success" : "failure"});
        backupDuration_.observe(durationSeconds);
        backupSize_.set(sizeBytes);
    }

    // Error metrics
    void recordError(const std::string& type, const std::string& operation) {
        errorsTotal_.inc(1.0, {type, operation});
    }

    // System metrics
    void setCPUUsage(double percent) { cpuUsage_.set(percent); }
    void setMemoryUsage(double bytes) { memoryUsage_.set(bytes); }
    void setGoroutines(double count) { goroutines_.set(count); }

    // Export
    std::string toPrometheus() const {
        return MetricsRegistry::instance().toPrometheus();
    }

    json toJson() const {
        return MetricsRegistry::instance().toJson();
    }

private:
    PacificDBMetrics()
        // Operations
        : operationsTotal_(MetricsRegistry::instance().counter(
              "pacificdb_operations_total",
              "Total number of database operations",
              {"operation", "database", "status"}))
        , operationDuration_(MetricsRegistry::instance().histogram(
              "pacificdb_operation_duration_seconds",
              "Duration of database operations",
              {0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0},
              {"operation"}))

        // Connections
        , activeConnections_(MetricsRegistry::instance().gauge(
              "pacificdb_active_connections",
              "Number of active client connections"))

        // Raft
        , raftState_(MetricsRegistry::instance().gauge(
              "pacificdb_raft_state",
              "Raft node state (1=leader)",
              {"node_id", "state"}))
        , raftElections_(MetricsRegistry::instance().counter(
              "pacificdb_raft_elections_total",
              "Total number of Raft elections"))
        , raftCommitLatency_(MetricsRegistry::instance().histogram(
              "pacificdb_raft_commit_latency_seconds",
              "Latency of Raft commits"))

        // Storage
        , storageBytes_(MetricsRegistry::instance().gauge(
              "pacificdb_storage_bytes",
              "Total storage size in bytes",
              {"database"}))
        , documentCount_(MetricsRegistry::instance().gauge(
              "pacificdb_document_count",
              "Number of documents",
              {"database", "collection"}))
        , compactionDuration_(MetricsRegistry::instance().histogram(
              "pacificdb_compaction_duration_seconds",
              "Duration of compaction operations"))
        , compactionBytesReclaimed_(MetricsRegistry::instance().counter(
              "pacificdb_compaction_bytes_reclaimed_total",
              "Total bytes reclaimed by compaction"))

        // WAL
        , walBytesWritten_(MetricsRegistry::instance().counter(
              "pacificdb_wal_bytes_written_total",
              "Total bytes written to WAL"))
        , walCurrentSize_(MetricsRegistry::instance().gauge(
              "pacificdb_wal_size_bytes",
              "Current WAL size in bytes"))
        , walSyncDuration_(MetricsRegistry::instance().histogram(
              "pacificdb_wal_sync_duration_seconds",
              "Duration of WAL sync operations",
              {0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1}))

        // Shards
        , shardCount_(MetricsRegistry::instance().gauge(
              "pacificdb_shard_count",
              "Number of active shards"))
        , shardRebalanceDuration_(MetricsRegistry::instance().histogram(
              "pacificdb_shard_rebalance_duration_seconds",
              "Duration of shard rebalance operations"))
        , shardDocumentCount_(MetricsRegistry::instance().gauge(
              "pacificdb_shard_documents",
              "Number of documents per shard",
              {"shard_id"}))

        // Queries
        , queryTotal_(MetricsRegistry::instance().counter(
              "pacificdb_queries_total",
              "Total number of queries",
              {"type", "index_usage"}))
        , queryDuration_(MetricsRegistry::instance().histogram(
              "pacificdb_query_duration_seconds",
              "Duration of query operations",
              {0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0},
              {"type"}))

        // Replication
        , replicationLag_(MetricsRegistry::instance().gauge(
              "pacificdb_replication_lag_seconds",
              "Replication lag in seconds",
              {"follower"}))
        , replicationBytesTransferred_(MetricsRegistry::instance().counter(
              "pacificdb_replication_bytes_total",
              "Total bytes transferred for replication"))

        // Backup
        , backupTotal_(MetricsRegistry::instance().counter(
              "pacificdb_backup_total",
              "Total number of backups",
              {"status"}))
        , backupDuration_(MetricsRegistry::instance().histogram(
              "pacificdb_backup_duration_seconds",
              "Duration of backup operations"))
        , backupSize_(MetricsRegistry::instance().gauge(
              "pacificdb_backup_size_bytes",
              "Size of last backup in bytes"))

        // Errors
        , errorsTotal_(MetricsRegistry::instance().counter(
              "pacificdb_errors_total",
              "Total number of errors",
              {"type", "operation"}))

        // System
        , cpuUsage_(MetricsRegistry::instance().gauge(
              "pacificdb_cpu_usage_percent",
              "CPU usage percentage"))
        , memoryUsage_(MetricsRegistry::instance().gauge(
              "pacificdb_memory_usage_bytes",
              "Memory usage in bytes"))
        , goroutines_(MetricsRegistry::instance().gauge(
              "pacificdb_goroutines",
              "Number of active goroutines/threads"))
    {}

    // Operations
    Counter& operationsTotal_;
    Histogram& operationDuration_;

    // Connections
    Gauge& activeConnections_;

    // Raft
    Gauge& raftState_;
    Counter& raftElections_;
    Histogram& raftCommitLatency_;

    // Storage
    Gauge& storageBytes_;
    Gauge& documentCount_;
    Histogram& compactionDuration_;
    Counter& compactionBytesReclaimed_;

    // WAL
    Counter& walBytesWritten_;
    Gauge& walCurrentSize_;
    Histogram& walSyncDuration_;

    // Shards
    Gauge& shardCount_;
    Histogram& shardRebalanceDuration_;
    Gauge& shardDocumentCount_;

    // Queries
    Counter& queryTotal_;
    Histogram& queryDuration_;

    // Replication
    Gauge& replicationLag_;
    Counter& replicationBytesTransferred_;

    // Backup
    Counter& backupTotal_;
    Histogram& backupDuration_;
    Gauge& backupSize_;

    // Errors
    Counter& errorsTotal_;

    // System
    Gauge& cpuUsage_;
    Gauge& memoryUsage_;
    Gauge& goroutines_;
};

} // namespace observability
} // namespace pacificdb
