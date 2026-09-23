#include "community_shell.hpp"

#include <cassert>
#include <iostream>

int main() {
    pacificdb::cli::ShellContext context;
    context.projectId = "project_1";
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
    const auto projectDatabases =
        pacificdb::cli::parseShellCommand("list databases", context).at("command");
    assert(projectDatabases.at("action") == "community_database_list");
    assert(projectDatabases.at("project_id") == "project_1");
    context.projectId.clear();
    for (const auto* command : {"create database app", "list databases", "use app",
                                "create collection users"}) {
        bool needsProject = false;
        try {
            pacificdb::cli::parseShellCommand(command, context);
        } catch (const std::invalid_argument& error) {
            needsProject = std::string(error.what()).find("select a project") != std::string::npos;
        }
        assert(needsProject);
    }
    context.projectId = "project_1";
    context.database.clear();
    bool needsDatabase = false;
    try {
        pacificdb::cli::parseShellCommand("create collection users", context);
    } catch (const std::invalid_argument& error) {
        needsDatabase = std::string(error.what()).find("select a database") != std::string::npos;
    }
    assert(needsDatabase);
    for (const auto* command : {"login admin", "whoami", "logout"}) {
        bool unknown = false;
        try {
            pacificdb::cli::parseShellCommand(command, context);
        } catch (const std::invalid_argument&) {
            unknown = true;
        }
        assert(unknown);
    }
    assert(std::string(pacificdb::cli::shellHelp()).find("Authentication") == std::string::npos);
    bool rejected = false;
    try {
        pacificdb::cli::parseShellCommand("create organization demo", context);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    const pacificdb::cli::MediaUploadInterrupted interrupted(
        "media_resume", 1, 1, 65536, "connection closed");
    const auto resumable = interrupted.publicResponse();
    assert(resumable.at("status") == "resumable");
    assert(resumable.at("error") == "media_upload_interrupted");
    assert(resumable.at("upload_id") == "media_resume");
    assert(resumable.at("next_chunk") == 1);
    std::cout << "NATIVE_SHELL_PARSER_PASS\n";
}
