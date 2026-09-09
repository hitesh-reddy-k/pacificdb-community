# PacificDB Community CLI and Media Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship one honest Community shell, in both npm and native installers, with working local projects, queries, manual backups, API keys, resumable media, and vector search.

**Architecture:** Keep the direct JSON-over-TCP connection. Store Community metadata and media chunks through reserved engine actions backed by normal Raft-replicated documents; keep security and backup invariants in their existing C++ owners. Both CLIs expose only commands that have executable handlers.

**Tech Stack:** C++17, nlohmann/json, OpenSSL, Node.js 18+ standard library, CMake, `node:test`.

**Spec:** `docs/superpowers/specs/2026-09-09-community-cli-and-media-design.md`

## Global Constraints

- Do not copy Enterprise/Cloud control-plane code or dependencies.
- Keep JSON-over-TCP, existing command flags, and raw JSON input compatible.
- Reserve `pacificdb_meta` at the C++ boundary; ordinary requests cannot access it.
- Media has no application-level total-size cap and is transferred in bounded Base64-safe chunks.
- Never log or persist plaintext passwords or full API keys.
- Unsupported aggregation stages fail explicitly.
- Publishing, pushing, npm release, and installer publication remain out of scope.

---

### Task 1: Reserved Community metadata catalog

**Files:**
- Create: `engine/include/community_catalog.hpp`
- Create: `engine/src/community_catalog.cpp`
- Create: `engine/test/community_catalog_test.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/src/server.cpp`

**Interfaces:**
- Produces: `pacificdb::community::isReservedDatabase(std::string_view)`.
- Produces: `CommunityCatalog::createProject/listProjects/getProject/deleteProject/mapDatabase`.
- Produces: `CommunityCatalog::beginMedia/putMediaChunk/finalizeMedia/listMedia/getMedia/getMediaChunk/deleteMedia/cleanupMedia`.
- Produces: `CommunityCatalog::recordRestore/listRestores`.
- Persists in `pacificdb_meta.projects`, `database_projects`, `media_manifests`, and `media_chunks` through `DatabaseEngine`.

- [x] **Step 1: Write the failing catalog test**

```cpp
int main() {
    auto& catalog = pacificdb::community::CommunityCatalog::instance();
    catalog.initialize("system");
    auto project = catalog.createProject("system", "demo");
    assert(project.at("name") == "demo");
    assert(catalog.mapDatabase("system", project.at("id"), "app"));
    assert(catalog.databaseProject("system", "app").at("project_id") == project.at("id"));
    assert(pacificdb::community::isReservedDatabase("pacificdb_meta"));
}
```

- [x] **Step 2: Run the focused target and verify RED**

Run: `cmake -S engine -B build && cmake --build build -j2 --target db_engine_community_catalog_test`

Expected: compilation fails because `community_catalog.hpp` and the target do not exist.

- [x] **Step 3: Implement the catalog and reserved request guard**

```cpp
namespace pacificdb::community {
bool isReservedDatabase(std::string_view name) { return name == "pacificdb_meta"; }

class CommunityCatalog {
public:
    static CommunityCatalog& instance();
    void initialize(const std::string& userId);
    json createProject(const std::string& userId, const std::string& name);
    json listProjects(const std::string& userId);
    json getProject(const std::string& userId, const std::string& id);
    bool deleteProject(const std::string& userId, const std::string& id);
    bool mapDatabase(const std::string& userId, const std::string& projectId,
                     const std::string& databaseName);
};
}
```

Add action handlers named `community_project_create`, `community_project_list`,
`community_project_get`, `community_project_delete`, and
`community_database_map`. Before ordinary create/drop/list/find/insert/update/
delete actions, reject `dbName == "pacificdb_meta"` with
`reserved_namespace`. Privileged raw access additionally requires the existing
`ADMIN` permission and `internalAdmin: true`. Catalog initialization creates
the reserved schema on each node before reserved Raft entries are applied.
Catalog methods call `DatabaseEngine` directly; its existing synchronous Raft
path supplies durability. Use deterministic logical IDs.

- [x] **Step 4: Add media lifecycle assertions to the same focused test**

```cpp
auto upload = catalog.beginMedia("system", "app", "photos", "movie.mp4",
                                 "video/mp4", 12, 4, "filehash");
assert(catalog.putMediaChunk("system", upload.at("id"), 0, "AAAA", 3, "c0").at("stored"));
bool mismatched = false;
try { catalog.putMediaChunk("system", upload.at("id"), 0, "BBBB", 3, "different"); }
catch (...) { mismatched = true; }
assert(mismatched);
assert(catalog.listMedia("system", false).empty());
assert(catalog.finalizeMedia("system", upload.at("id")).at("status") == "ready");
```

- [x] **Step 5: Implement media catalog methods and server actions**

Actions: `community_media_begin`, `community_media_put_chunk`,
`community_media_finalize`, `community_media_list`, `community_media_get`,
`community_media_get_chunk`, `community_media_delete`, and
`community_media_cleanup`. Validate chunk index, encoded data type, declared
size, and SHA-256; treat equal ID/checksum as idempotent and reject mismatch.

- [x] **Step 6: Run focused tests and commit**

Run: `cmake --build build -j2 --target db_engine_community_catalog_test && ./build/db_engine_community_catalog_test`

Expected: PASS.

Commit: `feat(engine): add reserved community metadata catalog`

### Task 2: Least-privilege API keys and identity

**Files:**
- Modify: `engine/include/security_manager.hpp`
- Modify: `engine/src/security_manager.cpp`
- Modify: `engine/src/server.cpp`
- Create: `engine/test/community_api_key_test.cpp`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Produces: `ApiKeyRecord` with `id`, `name`, `role`, `secretHash`, `createdBy`, `createdAt`, `lastUsedAt`, and `revokedAt`.
- Produces: `SecurityManager::createApiKey/listApiKeys/getApiKey/revokeApiKey/validateApiKey`.
- Engine actions: `security_whoami`, `api_key_create`, `api_key_list`, `api_key_get`, `api_key_revoke`.

- [x] **Step 1: Write the failing API-key test**

```cpp
auto created = security.createApiKey("ci", "readwrite", "admin");
assert(created.at("key").get<std::string>().rfind("pdb_", 0) == 0);
assert(created.dump().find("secret_hash") == std::string::npos);
auto validated = security.validateApiKey(created.at("key"));
assert(validated && validated->role == Role::WRITE);
assert(security.listApiKeys().dump().find(created.at("key").get<std::string>()) == std::string::npos);
assert(security.revokeApiKey(created.at("id"), "admin"));
assert(!security.validateApiKey(created.at("key")));
```

- [x] **Step 2: Build and verify RED**

Run: `cmake --build build -j2 --target db_engine_community_api_key_test`

Expected: compilation fails because the API-key methods do not exist.

- [x] **Step 3: Implement structured keys and atomic persistence**

Generate `pdb_<12-hex-id>_<32-random-bytes-base64url>`. Persist records to
`DATA_ROOT/security/api_keys.json` using a temporary file plus rename. Hash only
the secret with SHA-256. Resolve by parsed ID and compare hashes with
`CRYPTO_memcmp`. Map roles `read`, `readwrite`, and `admin` to existing roles.

- [x] **Step 4: Integrate API keys with authorization and actions**

Make `validateToken`, `hasPermission`, `getTokenRole`, and `getTokenUsername`
accept either an active session token or an active API key without storing the
plaintext key. `security_whoami` returns username/creator and role. Creation and
revocation require `ADMIN`; list/show require the current identity.

- [x] **Step 5: Run focused tests and commit**

Run: `cmake --build build -j2 --target db_engine_community_api_key_test && ./build/db_engine_community_api_key_test`

Expected: PASS.

Commit: `feat(security): add community API keys`

### Task 3: Bounded aggregation, explain, and manual backup lifecycle

**Files:**
- Create: `engine/include/community_query.hpp`
- Create: `engine/src/community_query.cpp`
- Create: `engine/test/community_query_test.cpp`
- Modify: `engine/include/backup_manager.hpp`
- Modify: `engine/src/backup_manager.cpp`
- Modify: `engine/src/server.cpp`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Produces: `aggregateDocuments(json documents, json pipeline)`.
- Produces: `explainFind(json filter)` that mirrors the actual equality-index/full-scan selection in `DatabaseEngine::find`.
- Actions: `aggregate`, `explain`, `get_backup`, `delete_backup`, `list_restores`, and `export_backup_manifest`.

- [x] **Step 1: Write failing aggregation tests**

```cpp
json docs = {{{"id", "1"}, {"team", "a"}, {"score", 2}},
             {{"id", "2"}, {"team", "b"}, {"score", 1}}};
auto out = aggregateDocuments(docs, json::array({
    {{"$match", {{"team", "a"}}}}, {{"$project", {{"score", 1}}}},
    {{"$sort", {{"score", -1}}}}, {{"$limit", 1}}
}));
assert(out.at("documents").size() == 1);
bool rejected = false;
try { aggregateDocuments(docs, json::array({{{"$group", json::object()}}})); }
catch (...) { rejected = true; }
assert(rejected);
```

- [x] **Step 2: Build and verify RED**

Run: `cmake --build build -j2 --target db_engine_community_query_test`

Expected: compilation fails because the query helper does not exist.

- [x] **Step 3: Implement only the certified aggregation subset**

Support `$match`, inclusion `$project`, single/multi-field `$sort`, nonnegative
`$skip`, positive `$limit`, and terminal `$count`. Return per-stage explain
records. Reject unknown stages and malformed values before scanning.

- [x] **Step 4: Add engine actions and honest explain**

`aggregate` reads through `DatabaseEngine::find` and passes results to the pure
helper. `explain` reports `INDEX_LOOKUP` when the actual find path receives a
simple scalar equality field and `FULL_SCAN` otherwise; include field, limit,
offset, and consistency without inventing index names.

- [x] **Step 5: Add backup lifecycle actions and restore journal**

`get_backup` filters `listBackups()`. `delete_backup` calls
`BackupManager::deleteBackup`. `restore_backup` remains synchronous and records
success/failure and counts in `CommunityCatalog::recordRestore`.
`export_backup_manifest` returns one backup's real metadata/checksum manifest;
the CLI writes it to a local file.

- [x] **Step 6: Run focused tests and commit**

Run: `cmake --build build -j2 --target db_engine_community_query_test db_engine_community_manual_operations_test && ./build/db_engine_community_query_test && ./build/db_engine_community_manual_operations_test`

Expected: PASS.

Commit: `feat(engine): add community query and backup commands`

### Task 4: Sequential media transfer in the Node client

**Files:**
- Modify: `sdk/node/src/index.js`
- Modify: `sdk/node/test/client.test.js`
- Modify: `sdk/node/README.md`

**Interfaces:**
- Produces: `capabilities()`.
- Produces: `uploadMediaFile(collection, filename, options)` returning the ready manifest.
- Produces: `downloadMediaFile(mediaId, destination)` returning `{id, destination, sizeBytes, sha256}`.
- Keeps `putMedia/getMedia` compatible for Buffer-sized callers.
- Engine action `community_capabilities` returns the same configured
  `max_request_bytes` value enforced by the connection reader.

- [x] **Step 1: Write a failing multi-chunk transfer test**

```js
const source = Buffer.alloc(700_000, 7);
await writeFile(input, source);
const uploaded = await client.uploadMediaFile('videos', input, { chunkBytes: 262_144 });
assert.equal(requests.filter(r => r.action === 'community_media_put_chunk').length, 3);
await client.downloadMediaFile(uploaded.id, output);
assert.deepEqual(await readFile(output), source);
```

- [x] **Step 2: Run and verify RED**

Run: `npm test --workspace @pacificdb/client`

Expected: FAIL because `uploadMediaFile` is undefined.

- [x] **Step 3: Implement adaptive sequential upload**

Read `max_request_bytes` from `capabilities`. Compute
`min(4 MiB, floor((max_request_bytes - 65536) * 3 / 4))`, allow a smaller
test override, and reject values below 64 KiB. Serialize each request before
sending and reduce the source chunk if it still exceeds the configured cap.
Use `fs.createReadStream` and send one `community_media_put_chunk` request at
a time with deterministic index and SHA-256. Begin first, resume with
`options.resume`, finalize last.

- [x] **Step 4: Implement sequential download and compatibility wrappers**

Fetch the manifest, request chunks in index order, verify each checksum, update
the whole-file SHA-256, and write with `createWriteStream`. Remove a partial
destination on checksum failure. Keep old Buffer methods unchanged.

- [x] **Step 5: Run tests and commit**

Run: `npm test --workspace @pacificdb/client`

Expected: PASS.

Commit: `feat(node): add resumable media file transfer`

### Task 5: Unified npm Community shell

**Files:**
- Create: `cli/src/shell.js`
- Modify: `cli/src/cli.js`
- Modify: `cli/test/cli.test.js`
- Modify: `cli/README.md`

**Interfaces:**
- Produces: `runShell(client, streams, options)`.
- Produces: friendly command parser with the exact categories in the spec.
- Persists context/history at `PACIFICDB_CLI_HOME` for tests or the platform application-data directory.

- [x] **Step 1: Write failing help/context tests**

```js
assert.match(text, /Authentication\n[\s\S]*whoami/);
assert.match(text, /Backups\n[\s\S]*show backup/);
assert.match(text, /Media\n[\s\S]*download media/);
assert.match(text, /Vectors\n[\s\S]*query vector/);
assert.doesNotMatch(text, /autoscal|billing|organization/i);
```

- [x] **Step 2: Run and verify RED**

Run: `npm test --workspace @pacificdb/cli`

Expected: FAIL because categorized help is absent.

- [x] **Step 3: Implement banner, context, history, and system commands**

Use Node standard library only. Write context/history with mode `0600`.
Implement `help [topic]`, `context show`, `context clear`, `status`, `history`,
`clear`, `request <json>`, and `exit`. Redact passwords, tokens, full API keys,
and internal engine fields.

- [x] **Step 4: Add project/database/document command tests, verify RED, then implement**

Commands use exact forms:

```text
create project <name> | list projects | use project <id> | show project | delete project <id>
create database <name> | list databases | use <name> | show database | drop database <name>
insert <collection> <json> | find <collection> [json] | findOne <collection> [json]
update <collection> <filter-json> <update-json> | delete <collection> <filter-json>
count <collection> [filter-json] | aggregate <collection> <pipeline-json>
explain <collection> [filter-json]
```

Database creation calls `createDatabase`, then `community_database_map` when a
project is selected, and reports mapping failure explicitly.

- [x] **Step 5: Add backup/API-key/media/vector command tests, verify RED, then implement**

Wire every command from the approved catalog. Password prompts bypass history.
`backup export` writes returned JSON with mode `0600`. `upload` and `download`
call the sequential SDK methods. `media cleanup` requires an explicit media ID
or `--all-incomplete` confirmation; it never deletes ready media.

- [x] **Step 6: Run npm tests and commit**

Run: `npm run test:npm`

Expected: all client and CLI tests PASS.

Commit: `feat(cli): add complete community shell`

### Task 6: Native installer shell parity

**Files:**
- Create: `engine/include/community_shell.hpp`
- Create: `engine/src/community_shell.cpp`
- Modify: `engine/src/shell.cpp`
- Create: `engine/test/native_shell_parser_test.cpp`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Produces the same command names and engine action payloads as `cli/src/shell.js`.
- Uses bounded buffers per media chunk and never loads a whole media file.

- [ ] **Step 1: Extract and test native command parsing**

```cpp
assert(parseShellCommand("find users {\"active\":true}", context).at("action") == "find");
assert(parseShellCommand("query vector embeddings [1,0] --k 3", context).at("k") == 3);
bool rejected = false;
try { parseShellCommand("create organization demo", context); }
catch (...) { rejected = true; }
assert(rejected);
```

- [ ] **Step 2: Build and verify RED**

Run: `cmake --build build -j2 --target db_engine_native_shell_parser_test`

Expected: compilation fails because the parser target does not exist.

- [ ] **Step 3: Implement native categorized shell and persistent context**

Use C++17 and existing nlohmann/json/OpenSSL only. Store owner-readable context
and history under `%LOCALAPPDATA%/PacificDB` on Windows,
`~/Library/Application Support/PacificDB` on macOS, and
`${XDG_STATE_HOME:-~/.local/state}/pacificdb` on Linux.

- [ ] **Step 4: Replace whole-file media handling with sequential chunks**

Encode only the derived chunk buffer with `EVP_EncodeBlock`, send the same
begin/chunk/finalize actions as the Node SDK, and stream download chunks to an
output file while verifying SHA-256.

- [ ] **Step 5: Run focused tests and commit**

Run: `cmake --build build -j2 --target pacificdb db_engine_native_shell_parser_test && ./build/db_engine_native_shell_parser_test`

Expected: PASS.

Commit: `feat(cli): add native community shell parity`

### Task 7: Integrated verification and public documentation

**Files:**
- Modify: `README.md`
- Modify: `COMMUNITY_SCOPE.md`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- Documents only commands proven by the final integration run.

- [ ] **Step 1: Update documentation and scope matrix**

Document the exact commands, reserved namespace, API-key roles, synchronous
restore, resume ID, cleanup, and precise media capacity wording from the spec.

- [ ] **Step 2: Run formatting/static checks**

Run: `git diff --check && node --check cli/src/cli.js && node --check cli/src/shell.js && node --check sdk/node/src/index.js`

Expected: exit 0.

- [ ] **Step 3: Build and run retained suites**

Run: `cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 && npm run test:npm && scripts/test-community.sh build`

Expected: exit 0 for every command.

- [ ] **Step 4: Run local end-to-end command matrix**

Start one local engine with a disposable absolute data root, then exercise every
help-listed command, including a three-chunk media upload/download checksum,
vector result ordering, API-key revocation, backup manifest export, and cleanup
of an incomplete media ID. Capture commands and outputs under a temporary test
directory only.

- [ ] **Step 5: Confirm repository boundary and commit**

Run:

```bash
! rg -i 'enterprise|billing|autoscal|saml|oidc|kms|hsm|point.in.time|scheduled backup' \
  cli sdk/node engine/include/community_catalog.hpp engine/src/community_catalog.cpp
git status --short
```

Expected: the boundary scan has no product-code matches; status contains only
the planned files.

Commit: `docs: document complete community CLI`
