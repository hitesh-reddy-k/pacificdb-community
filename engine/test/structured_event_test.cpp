#include "structured_event.hpp"

#include <cassert>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

int main() {
    using pacificdb::observability::StructuredEvent;

    StructuredEvent event{
        "error",
        "media",
        "media_chunk_failed",
        "media_1",
        "chunk rejected",
        {
            {"token", "secret"},
            {"index", 2},
            {"nested", nlohmann::json{{"document", "private"}}},
        },
    };
    const auto line = pacificdb::observability::formatStructuredEvent(event);
    const auto value = nlohmann::json::parse(line);
    assert(value.at("timestamp_ms").get<long long>() > 0);
    assert(value.at("severity") == "error");
    assert(value.at("subsystem") == "media");
    assert(value.at("code") == "media_chunk_failed");
    assert(value.at("operation_id") == "media_1");
    assert(value.at("message") == "chunk rejected");
    assert(value.at("fields").at("token") == "[redacted]");
    assert(value.at("fields").at("index") == 2);
    assert(value.at("fields").at("nested") == "[omitted]");
    assert(line.find("secret") == std::string::npos);
    assert(line.find("private") == std::string::npos);

    std::ostringstream output;
    pacificdb::observability::emitStructuredEvent(output, event);
    assert(output.str() == line + "\n");

    StructuredEvent credentials{
        "warn", "auth", "auth_rejected", "", "rejected",
        {
            {"PASSWORD", "one"},
            {"apiKey", "two"},
            {"Authorization", "three"},
            {"binaryData", "four"},
            {"payload", "five"},
            {"safe", true},
            {"empty", nullptr},
        },
    };
    const auto redacted = nlohmann::json::parse(
        pacificdb::observability::formatStructuredEvent(credentials));
    for (const char* key : {
             "PASSWORD", "apiKey", "Authorization", "binaryData", "payload"}) {
        assert(redacted.at("fields").at(key) == "[redacted]");
    }
    assert(redacted.at("fields").at("safe") == true);
    assert(redacted.at("fields").at("empty").is_null());
    return 0;
}
