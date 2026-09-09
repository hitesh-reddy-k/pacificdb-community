#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace pacificdb::cli {

struct ShellContext {
    std::string database;
    std::string projectId;
    std::string token;
};

nlohmann::json parseShellCommand(const std::string& line,
                                 const ShellContext& context);
const char* shellHelp();

}  // namespace pacificdb::cli
