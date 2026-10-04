# Database-first Workflow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users connect/create database → collection directly in Node, both CLIs and Workbench, retaining legacy project SDK APIs.

**Architecture:** Existing engine `createDatabase` and `listDatabases` remain authoritative. A shared URL fixture pins grammar across four language implementations. Workbench retains per-request clients and its current pagination, four-worker summary loader and scope generations; expensive overview work becomes demand-driven.

**Tech Stack:** Node >=18, C++17 and existing OpenSSL, JSON/TCP, HTML/CSS/JavaScript, existing Playwright/Electron development tools.

**Spec:** [Database-first design](../specs/2026-10-03-database-first-cli-workbench-design.md).

## Global Constraints

- “No engine format, durable sync, Raft apply/acknowledgement order, RBAC or namespace-containment change is authorized here.”
- “Existing raw engine/project APIs and SDK opt-in methods remain available.”
- “Secrets are never included in generated Workbench URLs, saved CLI context, logs, exception messages, process examples, or diagnostic output.”
- “Each operation captures its database, collection, and authentication scope before asynchronous or blocking I/O begins so later selection changes cannot redirect an in-flight request.”
- “Keep per-request SDK clients for now; a shared mutable `client.database` would introduce concurrent scope races.”
- “No package is published, no release tag is created, and no installer is deployed during implementation.”
- No new runtime dependencies; original checkout and stored project metadata remain untouched.

## Review Focus

- URL parser differences (IPv6, percent encoding, duplicate parameters, URL/flag conflicts): all consumers must pass the same fixtures; tasks 1 and 3.
- Older context has a database and project/token fields: preserve and validate only the database, remove secrets/project ID on save; tasks 2 and 3.
- Selection changes during a transfer/write: original operation scope survives every await, obsolete UI result is ignored; tasks 1, 4 and 5.
- Media ID from another database/collection: manifest checks still deny access before download/delete; task 4.
- Hidden overview or stale count: no background all-collection scans, unknown is not displayed as zero; task 5.

---

## Shared URL contract

Create `sdk/contracts/connection-urls.json` with `valid` and `invalid` cases. Each valid case has `url` and normalized `host`, `port`, `database`, `tls`, `userId`, `timeoutMs`, `poolSize`, optional `caFile`, `username`, `password`; test-only credentials in fixtures are never real secrets. Invalid cases have `url`, optional conflicting `options`, and public `code`.

- Schemes: `pacificdb` and `pacificdbs`; host required; default port 9000; explicit port must be decimal integer 1–65535.
- Database is one required, strictly percent-decoded UTF-8 path segment; reject decoded slash/backslash, empty name, NUL/control characters, malformed UTF-8/escapes, trailing/multiple path segments and fragments. Engine retains namespace/reserved-name authority.
- Query keys: exactly `userId`, `timeoutMs`, `poolSize`, `caFile`. Reject unknown, repeated or empty values. `timeoutMs` is decimal integer 1–1,800,000, default 30,000; `poolSize` 1–32, default 16; `userId` defaults `system`; `caFile` requires TLS. Explicit options which conflict with URL values fail, rather than override silently.
- Optional userinfo requires both strictly decoded nonempty username and password. URL credentials are private authentication input, never part of public/stringified connection information. No token query parameter.
- Generated Workbench URL has no userinfo, token or CA filesystem path. Include nondefault `userId` when necessary. Authenticated examples read `PACIFICDB_URL` from the environment instead of putting secrets in example strings.
- Both CLIs accept `--url` and `PACIFICDB_URL`; flag wins over environment. Explicit host/port/database conflicting with URL fail before local-engine startup. Native shell validates `poolSize` for grammar parity but stays single-request at a time; it does not claim a pool.

### Task 1: Node direct API and URL connection lifecycle

**Files:** Create `sdk/node/src/connection-url.js`, `sdk/contracts/connection-urls.json`, `sdk/node/test/connection.test.js`; modify `sdk/node/src/index.js`, `sdk/node/test/client.test.js`, `sdk/node/README.md`.

**Interfaces:** Export `parseConnectionUrl(url, options = {})` returning normalized private constructor options; export `PacificDB` as the beginner alias. Add `PacificDBClient.fromUrl(url, options = {})` (lazy) and static async `PacificDBClient.connect(url, options = {})` (eager). Preserve instance `connect()`, constructor options, `request`, all existing project methods and response shapes. Direct `createDatabase(name, dbType = 'binary')` selects on success; `useDatabase(name)` validates an actual `listDatabases` array. File methods capture a private request scope once at entry.

- [ ] Write `connection.test.js` cases for every URL fixture, zero sockets on invalid input, lazy concurrent requests authenticate once, eager auth failure closes resources, copied/repr/error output omits passwords/tokens, and direct create→collection→insert/find sends zero catalog calls. Add failed create/use tests preserving selection; keep explicit project membership/mapping tests.
- [ ] Run `node --test sdk/node/test/connection.test.js sdk/node/test/client.test.js`; expect failures for missing URL/direct APIs before implementation.
- [ ] Implement parser using native `URL` plus strict decoding/validation; initialize credentials privately and coordinate lazy authentication with one promise. Use current pool without another coordinator. Direct mode uses engine APIs; explicit `projectId` mode retains catalog behavior. Capture payload before awaits, including every request in upload/download/backup export; never retry a write after disconnect.
- [ ] Add transfer-scope regression: pause capability/stat/chunk response, switch database, then assert all later frames retain original database. Assert failed authentication produces no follow-on write and no reusable client.
- [ ] Run `npm test --workspace @pacificdb/client`; expect all direct/legacy/transport/media/backup tests to pass. Commit changed SDK, fixtures and tests as `feat: add direct database and URL connections to Node client`.

### Task 2: npm shell and context transition

**Files:** Modify `cli/src/cli.js`, `cli/src/shell.js`, `cli/test/cli.test.js`, `cli/test/local-engine.test.js`, `cli/README.md`; create `cli/test/database-first.test.js`.

**Interfaces:** `main(args, streams)` accepts `--url`/environment through task 1 parser; `runShell(client, streams, options)` keeps its existing signature. Saved context becomes `{database}` only. `parseShellCommand(line, context)` retains document/media/vector/raw grammar; friendly project commands become unknown-command errors.

- [ ] Write tests `direct shell selects only successful creations`, `old project context keeps database without credentials`, `URL overrides saved context but conflicting flag fails`, `restored database validated before dependent operation`, `failed drop retains context`, `raw project request remains usable`, and `URL secret never reaches prompt/history`. Capture exact engine action sequences: listing/use=`listDatabases`, creation=`createDatabase`, no catalog calls.
- [ ] Run `node --test cli/test/database-first.test.js cli/test/cli.test.js`; expect projectless commands/URL/context tests to fail on baseline.
- [ ] Remove project prerequisites/help/prompt/context output and default project injection. Route create/list/use directly; set client and shell database only after success. Validate a restored selection once before dependent use, clearing only on successful selected-database drop/context clear. Thread resolved URL options through Workbench startup too; close one-shot clients in `finally`.
- [ ] Run `npm test --workspace @pacificdb/cli`; verify old project-bearing and projectless context files, unknown project commands, missing flag values, password redaction and failed-operation behavior. Commit as `feat: make npm shell database first`.

### Task 3: Native shell direct flow and TLS URL parity

**Files:** Modify `engine/include/community_shell.hpp`, `engine/src/community_shell.cpp`, `engine/src/shell.cpp`, `engine/test/native_shell_parser_test.cpp`, `engine/CMakeLists.txt`, `scripts/test-community-autostart.sh`; create `engine/include/cli_connection.hpp`, `engine/src/cli_connection.cpp`, `engine/test/cli_connection_test.cpp`, `scripts/test-database-first-cli.mjs`.

**Interfaces:** In `pacificdb::cli`, add `ConnectionOptions {host, port, database, userId, timeoutMs, poolSize, tls, caFile, username, password}` and `parseConnectionUrl(const std::string&) -> ConnectionOptions`; keep secrets out of printable output. Add CLI-only `requestJson(const ConnectionOptions&, const nlohmann::json&) -> nlohmann::json`, using RAII and existing JSON-line response convention. Keep shell parser signature; legacy `ShellContext.projectId` may remain inert for source compatibility, never injected/saved/displayed.

- [ ] Change parser tests to expect direct create/list/use/drop without projects; assert collection needs only a database and project friendly commands fail. Add native URL tests consuming the same fixture, including malformed UTF-8/IPv6/secret redaction. Add fake TCP/TLS integration for matching host, wrong host, untrusted CA, timeout and auth-before-command.
- [ ] Configure `cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release`; build `db_engine_native_shell_parser_test` and new `db_engine_cli_connection_test` with `cmake --build build --target db_engine_native_shell_parser_test db_engine_cli_connection_test -j2`. Run them; expect direct/URL failures before implementations.
- [ ] Implement strict fixture-conformant CLI URL parsing without a new URL library. Extend CLI-only transport with verified TLS (`OpenSSL::SSL`, default/custom trust, DNS/IP hostname verification and SNI for DNS), deadlines and RAII; preserve engine socket/runtime semantics. Never disable certificate checks. Resolve/authenticate URL scope before shell/raw/media/vector operations; remote/TLS errors never trigger automatic local engine startup. Context transition mirrors npm shell, with owner-only history/context.
- [ ] Build normal Release `cmake --build build -j2`; run both parser targets, `node scripts/test-database-first-cli.mjs build` and `scripts/test-community-autostart.sh build`. Integration covers both binaries, old contexts, failed operations, direct CRUD/restart, URL auth and conflicts. Commit as `feat: make native CLI database first with verified URL transport`.

### Task 4: Workbench direct request adapter

**Files:** Modify `cli/src/workbench.js`, `cli/test/workbench.test.js`, `scripts/test-workbench-e2e.mjs`.

**Interfaces:** Retain `createWorkbenchServer` and `startWorkbench`. Keep operation names for databases/collections/documents/indexes/vectors/media; remove `projects.*`. Scope body/headers contain only database and collection. `databases.list` returns `{databases: [...]}` normalized from engine array for UI; `connection.info` returns secret-free `{host, port, tls, userId, authenticationRequired}`.

- [ ] Write adapter tests using recorded engine frames: mapped+unmapped databases visible, direct create/list/delete, CRUD has no catalog/list-all probes, project operation rejected, malformed/scope/limit/read-only failure preserved. Retain Host/origin/session-token tests. Media tests send a valid ID with wrong database/collection and require denial before download/delete; test parallel HTTP writes to separate scopes.
- [ ] Run `node --test cli/test/workbench.test.js`; expect database-only calls to fail and project/extra-probe assertions to fail on baseline.
- [ ] Replace project helpers with input validation and direct engine scope. Let engine validate ordinary CRUD existence/auth; list/select helpers use engine-backed listings. Keep per-request clients, media manifest scope checks, request limits and browser security protections; do not echo credential-bearing URLs.
- [ ] Run adapter suite and `node scripts/test-workbench-e2e.mjs build` against fresh engine. Assert explicit action counts (one ordinary query, no catalog calls) and preserve old mapped data. Commit as `feat: remove project prerequisites from Workbench adapter`.

### Task 5: Database navigation, URL examples and demand-driven summaries

**Files:** Modify `cli/workbench/app.js`, `cli/workbench/index.html`, `cli/workbench/app.css`, `scripts/test-workbench-browser.mjs`, `scripts/test-workbench-desktop.mjs`, `docs/WORKBENCH.md`.

**Interfaces:** UI state retains selected database/collection, current database's summaries and existing navigation generation; remove project state. `scope()` returns `{database, collection}`. `refreshDatabases()` retains valid selection, `createResource()` selects successful created database, `loadCollectionSummaries()` starts only for visible overview/explicit overview refresh with at most four workers.

- [ ] Adapt browser test to fresh database→collection→document→query/edit/delete without project/use steps; verify connection URL and Python/Java examples, disabled copy before database, keyboard/mobile behavior and destructive confirmations. Add delayed-response navigation tests and request capture: Documents/Query/Media trigger zero all-collection counts, Overview has maximum four summary jobs, switching database cannot display old results, unknown counts render `—`.
- [ ] Run `npm run test:workbench:browser -- build`; expect old project UI and background-count tests to fail before UI change.
- [ ] Reuse existing navigation/layout/components; remove project tree/stats/search/breadcrumb/ID controls, expose New database and direct database selection. Add URL/Python dialog options with environment examples for auth. Gate existing summary loader by view/scope generation, mark affected summaries dirty and reuse only valid selected-database summaries. Capture mutation scope, disable pending writes, and ignore responses from obsolete views; retain pagination/history/internal-field protections/vector/index/native-dialog flows.
- [ ] Run browser suite, `node scripts/test-desktop-engine.mjs`, prepare/package existing desktop via `npm run desktop:unpacked`, then `PACIFICDB_TEST_DESKTOP="$PWD/dist/desktop/linux-unpacked/pacificdb-workbench" xvfb-run -a npm run test:workbench:desktop`. Record Linux smoke, screenshot/request-count artifacts and unavailable platform checks. Commit as `feat: simplify Workbench navigation and defer hidden overview work`.

### Task 6: Workflow documentation and compatibility qualification

**Files:** Modify `README.md`, `cli/README.md`, `sdk/node/README.md`, `docs/WORKBENCH.md`, `site/docs.html`, `site/index.html`, relevant shell docs/tests; create `docs/DATABASE_FIRST_MIGRATION.md` and `docs/evidence/2026-10-03-database-first-workflow.md`.

**Interfaces:** Documentation replaces mandatory projects with direct command/API examples; legacy section explicitly documents opt-in APIs/raw commands and that no catalog/data migration occurs.

- [ ] Add documentation assertions to current tests: copyable beginner snippets lack project/use prerequisite, URL option visible, legacy API remains documented, no secret literals in examples. Run tests and confirm old docs fail targeted assertions.
- [ ] Update quickstarts and migration guide; distinguish friendly-command removal from retained SDK/engine compatibility. Record request observations as UI diagnostics, not database throughput benchmarks.
- [ ] Run `npm run test:npm`, `node scripts/test-site-docs.mjs`, `node scripts/test-database-first-cli.mjs build`, Workbench API/browser/desktop checks and `git diff --check`. Ensure isolated package resolution points into this worktree before claiming SDK coverage. Commit documentation/evidence as `docs: document direct database workflows and compatibility`.
