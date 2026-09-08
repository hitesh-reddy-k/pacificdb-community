#pragma once
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <mutex>

class MetricsExporter {
public:
    // Generate Prometheus-formatted metrics
    static std::string getMetrics();

    // Add custom metrics
    static void recordCustomMetric(const std::string& name, double value);

    // Increment counter
    static void incrementCounter(const std::string& name, double increment = 1.0);

    // Update histogram
    static void recordHistogram(const std::string& name, double value);

    // Health check
    static std::string getHealth();

private:
    static std::string formatGauge(const std::string& name,
                                   const std::string& help,
                                   double value);

    static std::string formatCounter(const std::string& name,
                                     const std::string& help,
                                     uint64_t value);

    static std::string formatHistogram(const std::string& name,
                                       const std::string& help,
                                       const std::vector<std::pair<double, uint64_t>>& buckets,
                                       uint64_t count,
                                       double sum);

    static std::string formatLabels(const std::map<std::string, std::string>& labels);

    // Custom metrics storage
    static std::mutex customMetricsMutex_;
    static std::map<std::string, double> customMetrics_;
};
