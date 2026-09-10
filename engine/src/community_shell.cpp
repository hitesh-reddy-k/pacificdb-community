#include "community_shell.hpp"

#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace pacificdb::cli {
namespace {
using json = nlohmann::json;

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

json request(json command) { return {{"kind", "request"}, {"command", std::move(command)}}; }

void requireDatabase(const ShellContext& context) {
    if (context.database.empty())
        throw std::invalid_argument("select a database with: use <name>");
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> result;
    std::string token;
    char quote = 0;
    for (char character : text) {
        if (quote) {
            if (character == quote) quote = 0;
            else token.push_back(character);
        } else if (character == '\'' || character == '"') quote = character;
        else if (std::isspace(static_cast<unsigned char>(character))) {
            if (!token.empty()) { result.push_back(token); token.clear(); }
        } else token.push_back(character);
    }
    if (quote) throw std::invalid_argument("unterminated quote");
    if (!token.empty()) result.push_back(token);
    return result;
}

std::string flag(const std::vector<std::string>& tokens,
                 const std::string& name, const std::string& fallback = {}) {
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] != name) continue;
        if (i + 1 == tokens.size()) throw std::invalid_argument(name + " requires a value");
        return tokens[i + 1];
    }
    return fallback;
}

std::pair<json, std::string> takeJson(const std::string& input) {
    const std::string text = trim(input);
    if (text.empty() || (text[0] != '{' && text[0] != '['))
        throw std::invalid_argument("JSON value required");
    const char opening = text[0];
    const char closing = opening == '{' ? '}' : ']';
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char character = text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
        } else if (character == '"') quoted = true;
        else if (character == opening) ++depth;
        else if (character == closing && --depth == 0) {
            return {json::parse(text.substr(0, i + 1)), trim(text.substr(i + 1))};
        }
    }
    throw std::invalid_argument("incomplete JSON value");
}

}  // namespace

const char* shellHelp() {
    return R"HELP(Authentication
  login <username>                     Sign in
  whoami                              Show current identity
  logout                              Clear the current credential

Projects
  create project <name>               Create project
  list projects                       List projects
  use project <id>                    Switch project
  show project                        Show project
  delete project <id>                 Delete project

Databases and queries
  create database <name>              Create database
  list databases                      List databases
  use <name>                          Switch database
  show database                       Show database
  drop database <name>                Drop database
  create collection <name>            Create collection
  list collections                    List collections
  insert <collection> <json>          Insert document
  find <collection> [filter-json]     Find documents
  findOne <collection> [filter-json]  Find one document
  update <collection> <filter> <json> Update one document
  delete <collection> <filter-json>   Delete one document
  aggregate <collection> <pipeline>   Run bounded aggregation
  count <collection> [filter-json]    Count documents
  explain <collection> [filter-json]  Explain a find

Backups
  create backup [--name <name>]       Create manual backup
  list backups                        List backups
  show backup <id>                    Show backup
  restore backup <id>                 Restore synchronously
  list restores                       List restore attempts
  delete backup <id>                  Delete backup
  backup verify <id>                  Verify backup
  backup export <id> [file]           Export complete backup JSON

Security / API keys
  create api-key [--name <n>] [--role read|readwrite|admin]
  list api-keys                       List keys
  show api-key <id>                   Show key metadata
  revoke api-key <id>                 Revoke key

Media
  upload image|video|media <path> [--collection <name>] [--resume <id>]
  download media <id> <path>          Download and verify media
  list media [--all]                  List ready or all media
  find media <query>                  Find media metadata
  show media <id>                     Show media metadata
  delete media <id>                   Delete media and chunks
  media cleanup <id>                  Delete one incomplete upload

Vectors
  put vector <collection> <id> <json-vector>
  query vector <collection> <json-vector> [--k <n>] [--metric <name>]

System
  help [topic]                        Show help
  context show                        Show context
  context clear                       Clear context and credentials
  status                              Show connection status
  history                             Show command history
  clear                               Clear screen
  request <json>                      Send a raw request
  exit | quit                         Exit shell
)HELP";
}

json parseShellCommand(const std::string& input, const ShellContext& context) {
    const std::string text = trim(input);
    if (text.empty()) return {{"kind", "empty"}};
    if (text.front() == '{') return request(json::parse(text));
    if (text.rfind("request ", 0) == 0) return request(json::parse(text.substr(8)));
    if (text == "exit" || text == "quit") return {{"kind", "exit"}};
    if (text == "help" || text.rfind("help ", 0) == 0) return {{"kind", "help"}};
    if (text == "clear") return {{"kind", "clear"}};
    if (text == "history") return {{"kind", "history"}};
    if (text == "context show") return {{"kind", "context_show"}};
    if (text == "context clear") return {{"kind", "context_clear"}};
    if (text == "status") return request({{"action", "ping"}});
    if (text == "logout") return {{"kind", "logout"}};
    if (text.rfind("login ", 0) == 0)
        return {{"kind", "login"}, {"username", trim(text.substr(6))}};
    if (text == "whoami") return request({{"action", "security_whoami"}});

    std::smatch match;
    if (std::regex_match(text, match, std::regex(R"(^create project (.+)$)")))
        return request({{"action", "community_project_create"}, {"name", match[1].str()}});
    if (text == "list projects") return request({{"action", "community_project_list"}});
    if (std::regex_match(text, match, std::regex(R"(^use project (\S+)$)")))
        return {{"kind", "use_project"}, {"id", match[1].str()}};
    if (text == "show project") {
        if (context.projectId.empty()) throw std::invalid_argument("no project selected");
        return request({{"action", "community_project_get"}, {"id", context.projectId}});
    }
    if (std::regex_match(text, match, std::regex(R"(^delete project(?: (\S+))?$)"))) {
        const std::string id = match[1].matched ? match[1].str() : context.projectId;
        if (id.empty()) throw std::invalid_argument("project id required");
        auto result = request({{"action", "community_project_delete"}, {"id", id}});
        result["clear_project"] = id;
        return result;
    }

    if (std::regex_match(text, match, std::regex(R"(^create database (\S+)$)")))
        return {{"kind", "create_database"}, {"name", match[1].str()}};
    if (text == "list databases") return request({{"action", "listDatabases"}});
    if (std::regex_match(text, match, std::regex(R"(^use (\S+)$)")))
        return {{"kind", "use_database"}, {"name", match[1].str()}};
    if (text == "show database") {
        requireDatabase(context);
        return {{"kind", "show_database"}};
    }
    if (std::regex_match(text, match, std::regex(R"(^drop database (\S+)$)"))) {
        auto result = request({{"action", "dropDatabase"}, {"dbName", match[1].str()}});
        result["clear_database"] = match[1].str();
        return result;
    }
    if (std::regex_match(text, match, std::regex(R"(^create collection (\S+)$)"))) {
        requireDatabase(context);
        return request({{"action", "createCollection"}, {"collection", match[1].str()}});
    }
    if (text == "list collections") {
        requireDatabase(context);
        return request({{"action", "listCollections"}});
    }

    if (std::regex_match(text, match, std::regex(R"(^find media (.+)$)")))
        return {{"kind", "media_find"}, {"query", match[1].str()}};
    if (std::regex_match(text, match, std::regex(R"(^delete media (\S+)$)")))
        return request({{"action", "community_media_delete"}, {"media_id", match[1].str()}});
    if (std::regex_match(text, match, std::regex(R"(^delete backup (\S+)$)")))
        return request({{"action", "delete_backup"}, {"backup_id", match[1].str()}});

    if (std::regex_match(text, match, std::regex(R"(^insert (\S+)\s+(.+)$)"))) {
        requireDatabase(context);
        return request({{"action", "insert"}, {"collection", match[1].str()},
                        {"data", json::parse(match[2].str())}});
    }
    if (std::regex_match(text, match,
                         std::regex(R"(^(findOne|find|count|explain) (\S+)(?:\s+(.+))?$)"))) {
        requireDatabase(context);
        const std::string name = match[1].str();
        json command{{"action", name == "findOne" ? "find" : name},
                     {"collection", match[2].str()},
                     {"filter", match[3].matched ? json::parse(match[3].str()) : json::object()}};
        if (name == "findOne") command["limit"] = 1;
        return request(std::move(command));
    }
    if (std::regex_match(text, match, std::regex(R"(^update (\S+)\s+(.+)$)"))) {
        requireDatabase(context);
        auto [filter, rest] = takeJson(match[2].str());
        auto [update, trailing] = takeJson(rest);
        if (!trailing.empty()) throw std::invalid_argument("unexpected text after update document");
        return request({{"action", "updateOne"}, {"collection", match[1].str()},
                        {"filter", filter}, {"update", update}});
    }
    if (std::regex_match(text, match, std::regex(R"(^delete (\S+)\s+(.+)$)"))) {
        requireDatabase(context);
        return request({{"action", "deleteOne"}, {"collection", match[1].str()},
                        {"filter", json::parse(match[2].str())}});
    }
    if (std::regex_match(text, match, std::regex(R"(^aggregate (\S+)\s+(.+)$)"))) {
        requireDatabase(context);
        return request({{"action", "aggregate"}, {"collection", match[1].str()},
                        {"pipeline", json::parse(match[2].str())}});
    }

    if (text.rfind("create backup", 0) == 0) {
        const auto tokens = words(text);
        return request({{"action", "create_backup"},
                        {"description", flag(tokens, "--name", "manual backup")}});
    }
    if (text == "list backups") return request({{"action", "list_backups"}});
    if (std::regex_match(text, match, std::regex(R"(^show backup (\S+)$)")))
        return request({{"action", "get_backup"}, {"backup_id", match[1].str()}});
    if (std::regex_match(text, match, std::regex(R"(^restore backup (\S+)$)")))
        return request({{"action", "restore_backup"}, {"backup_id", match[1].str()}});
    if (text == "list restores") return request({{"action", "list_restores"}});
    if (std::regex_match(text, match, std::regex(R"(^backup verify (\S+)$)")))
        return request({{"action", "verify_backup"}, {"backup_id", match[1].str()}});
    if (std::regex_match(text, match, std::regex(R"(^backup export (\S+)(?:\s+(.+))?$)")))
        return {{"kind", "backup_export"}, {"id", match[1].str()},
                {"filename", match[2].matched ? match[2].str() : ""}};

    if (text.rfind("create api-key", 0) == 0) {
        const auto tokens = words(text);
        return request({{"action", "api_key_create"}, {"name", flag(tokens, "--name", "default")},
                        {"role", flag(tokens, "--role", "readwrite")}});
    }
    if (text == "list api-keys") return request({{"action", "api_key_list"}});
    if (std::regex_match(text, match, std::regex(R"(^show api-key (\S+)$)")))
        return request({{"action", "api_key_get"}, {"id", match[1].str()}});
    if (std::regex_match(text, match, std::regex(R"(^revoke api-key (\S+)$)")))
        return request({{"action", "api_key_revoke"}, {"id", match[1].str()}});

    if (std::regex_match(text, match,
                         std::regex(R"(^upload (image|video|media)\s+(.+)$)"))) {
        requireDatabase(context);
        const auto tokens = words(match[2].str());
        if (tokens.empty()) throw std::invalid_argument("media path required");
        return {{"kind", "media_upload"}, {"filename", tokens[0]},
                {"collection", flag(tokens, "--collection", "media")},
                {"resume", flag(tokens, "--resume")}};
    }
    if (std::regex_match(text, match, std::regex(R"(^download media (\S+)\s+(.+)$)")))
        return {{"kind", "media_download"}, {"id", match[1].str()},
                {"filename", match[2].str()}};
    if (text == "list media" || text == "list media --all")
        return request({{"action", "community_media_list"},
                        {"all", text == "list media --all"}});
    if (std::regex_match(text, match, std::regex(R"(^show media (\S+)$)")))
        return request({{"action", "community_media_get"}, {"media_id", match[1].str()}});
    if (std::regex_match(text, match, std::regex(R"(^media cleanup (\S+)$)")))
        return request({{"action", "community_media_cleanup"}, {"media_id", match[1].str()}});

    if (std::regex_match(text, match,
                         std::regex(R"(^put vector (\S+)\s+(\S+)\s+(.+)$)"))) {
        requireDatabase(context);
        return {{"kind", "vector_put"}, {"collection", match[1].str()},
                {"id", match[2].str()}, {"vector", json::parse(match[3].str())}};
    }
    if (std::regex_match(text, match,
                         std::regex(R"(^query vector (\S+)\s+(.+)$)"))) {
        requireDatabase(context);
        auto [vector, rest] = takeJson(match[2].str());
        const auto tokens = words(rest);
        return request({{"action", "queryVector"}, {"collection", match[1].str()},
                        {"vector", vector}, {"k", std::stoi(flag(tokens, "--k", "10"))},
                        {"metric", flag(tokens, "--metric", "cosine")}});
    }

    throw std::invalid_argument("unknown command: " + text);
}

}  // namespace pacificdb::cli
