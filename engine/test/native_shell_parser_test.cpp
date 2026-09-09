#include "community_shell.hpp"

#include <cassert>
#include <iostream>

int main() {
    pacificdb::cli::ShellContext context;
    context.database = "app";
    assert(pacificdb::cli::parseShellCommand(
               "find users {\"active\":true}", context)
               .at("command")
               .at("action") == "find");
    assert(pacificdb::cli::parseShellCommand(
               "findOne users {}", context)
               .at("command")
               .at("limit") == 1);
    assert(pacificdb::cli::parseShellCommand(
               "query vector embeddings [1,0] --k 3", context)
               .at("command")
               .at("k") == 3);
    assert(pacificdb::cli::parseShellCommand("find media movie", context)
               .at("kind") == "media_find");
    assert(pacificdb::cli::parseShellCommand("delete media media_1", context)
               .at("command")
               .at("action") == "community_media_delete");
    assert(pacificdb::cli::parseShellCommand("delete backup backup-1", context)
               .at("command")
               .at("action") == "delete_backup");
    bool rejected = false;
    try {
        pacificdb::cli::parseShellCommand("create organization demo", context);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    std::cout << "NATIVE_SHELL_PARSER_PASS\n";
}
