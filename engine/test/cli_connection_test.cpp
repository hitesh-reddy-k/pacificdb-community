#include "cli_connection.hpp"
#include <cassert>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1]);
    assert(input);
    nlohmann::json fixture;
    input >> fixture;
    for (const auto& test : fixture.at("valid")) {
        auto o = pacificdb::cli::parseConnectionUrl(test.at("url"));
        auto actual = nlohmann::json{{"host", o.host}, {"port", o.port},
            {"database", o.database}, {"tls", o.tls}, {"userId", o.userId},
            {"timeoutMs", o.timeoutMs}, {"poolSize", o.poolSize}};
        if (!o.caFile.empty()) actual["caFile"] = o.caFile;
        if (!o.username.empty()) { actual["username"] = o.username; actual["password"] = o.password; }
        // Explicit option overrides are tested by the CLI integration driver.
        if (!test.contains("options")) assert(actual == test.at("expected"));
    }
    for (const auto& test : fixture.at("invalid")) {
        if (test.contains("options")) continue;
        bool rejected = false;
        try { pacificdb::cli::parseConnectionUrl(test.at("url")); }
        catch (const std::invalid_argument& e) {
            rejected = std::string(e.what()).find("invalid_connection_url") != std::string::npos;
            assert(std::string(e.what()).find(test.at("url").get<std::string>()) == std::string::npos);
        }
        assert(rejected);
    }
    std::cout << "CLI_CONNECTION_FIXTURES_PASS\n";
}
