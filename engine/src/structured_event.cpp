#include "structured_event.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <string_view>

namespace pacificdb::observability {
namespace {

bool secretKey(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    for (const std::string_view secret : {
             "password", "token", "key", "authorization", "data", "payload"}) {
        if (key.find(secret) != std::string::npos) return true;
    }
    return false;
}

nlohmann::json safeFields(const nlohmann::json& fields) {
    nlohmann::json safe = nlohmann::json::object();
    if (!fields.is_object()) return safe;
    for (auto entry = fields.begin(); entry != fields.end(); ++entry) {
        if (secretKey(entry.key())) {
            safe[entry.key()] = "[redacted]";
        } else if (entry->is_primitive()) {
            safe[entry.key()] = *entry;
        } else {
            safe[entry.key()] = "[omitted]";
        }
    }
    return safe;
}

}  // namespace

std::string formatStructuredEvent(const StructuredEvent& event) {
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    nlohmann::json value{
        {"timestamp_ms", timestamp},
        {"severity", event.severity},
        {"subsystem", event.subsystem},
        {"code", event.code},
        {"message", event.message},
        {"fields", safeFields(event.fields)},
    };
    if (!event.operationId.empty()) {
        value["operation_id"] = event.operationId;
    }
    return value.dump();
}

void emitStructuredEvent(std::ostream& output, const StructuredEvent& event) {
    static std::mutex outputMutex;
    const auto line = formatStructuredEvent(event);
    std::lock_guard<std::mutex> lock(outputMutex);
    output << line << '\n';
    output.flush();
}

}  // namespace pacificdb::observability
