#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace pacificdb::cli {
struct ConnectionOptions {
    std::string host = "127.0.0.1", database, userId = "system", caFile, username, password;
    int port = 9000, timeoutMs = 30000, poolSize = 16;
    bool tls = false;
};
ConnectionOptions parseConnectionUrl(const std::string& url);
// CLI-only, single request, no retries; scopes are copied before I/O.
nlohmann::json requestJson(const ConnectionOptions& options, const nlohmann::json& command);
} // namespace pacificdb::cli
