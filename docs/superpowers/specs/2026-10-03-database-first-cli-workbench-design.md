# Database-first CLI, SDK, Workbench, and Publishable Java/Python Clients

## Intent and approved scope

Users should create or connect directly to a database and create collections without creating or selecting a project. The selected database is used automatically. The same flow must work in the Node, Java, and Python SDKs, npm CLI, bundled native CLI, browser Workbench, and desktop Workbench. The user approved retaining legacy engine/SDK project APIs and stored project metadata for compatibility while removing project commands/screens and mandatory selection from CLI/Workbench.

All clients accept a database-specific `pacificdb://` connection URL, and Workbench exposes a copyable URL for the selected database. Java and Python must provide the same supported public Community engine capabilities as the Node client and raw protocol, organized so common database work remains beginner-friendly while advanced operational APIs remain discoverable and explicit.

This is a workflow simplification and reduction of unnecessary UI/request work. It makes no cross-product throughput or enterprise-certification claim.

## Current behavior and evidence

The engine already accepts `createDatabase` without `project_id`, provides `listDatabases` for the requesting user, and permits ordinary collection operations without project membership. Its reserved-namespace filtering and authorization remain authoritative. Existing database deletion cleans up any catalog mapping after successful deletion.

The current clients introduce the requirement: `sdk/node/src/index.js` rejects projectless `createDatabase`, `useDatabase` and `createCollection`; both shell implementations require project context; `cli/src/workbench.js` validates project membership before database/collection operations. The Workbench UI additionally displays project nesting, creation, counts, IDs and project-dependent connection examples.

Workbench currently refreshes collection summaries for every collection after navigation and several mutations, even when the overview is hidden. Each summary/query also repeats project and database listing work. Its existing pagination, request cancellation, generation checks, four-worker summary limit and fragment rendering are useful and should be retained.

Fresh `npm run test:npm` on the current working checkout passed 15 SDK tests and 18 CLI/Workbench tests. Evidence is `/tmp/pacificdb-database-first-npm-baseline-20261003.log`. These tests characterize the old workflow, not the new design.

## Approach and alternatives

Use the engine's existing direct database APIs and remove project prerequisites at the client boundaries. Add connection-URL parsing in clients and shells rather than adding an engine endpoint. This is the selected approach: no implicit default project, catalog migration, engine format change, or new runtime dependency is needed.

Merely hiding project controls while silently creating/selecting a project would retain mapping work and exclude existing unmapped databases. Deleting the engine catalog would break legacy SDK consumers and introduce an unnecessary migration; the user explicitly chose to retain it.

## CLI behavior

Both shells expose `create database <name>`, `list databases`, `use <name>`, `show database`, `drop database <name>` and collection/document/media/vector commands directly. Remove friendly project commands from parsing/help, project IDs from prompts/context output, and automatic `project_id` injection in native shell requests. Raw JSON requests remain available for legacy APIs.

Creation uses `createDatabase` without mapping. Listing and `use` use `listDatabases`; accept its actual array response and retain explicit missing-database errors. Database context changes only after successful validation. Collection creation requires a selected database, never a project. Successful database creation selects the created database automatically; `use <name>` remains available for deliberate switching and compatibility.

On loading an older saved context, ignore/remove `projectId` while preserving its database selection. Continue credential stripping and owner-only context/history handling. Validate a restored database through the current engine before the first operation that needs it; an invalid selection must fail clearly rather than silently redirect to another database. The context supplied by a connection URL takes precedence over stale saved selection. `--database` works without a project; a conflicting URL database and `--database` fail clearly instead of silently choosing one. `context clear` clears the database. Successful deletion clears a selected database; failed operations preserve it.

## Connection URL

The canonical forms are `pacificdb://host:port/database` for TCP and `pacificdbs://host:port/database` for TLS. SDK connect factories also accept percent-encoded `username:password@` user information and supported query parameters for user identity, timeout, pool size, and CA configuration. A credential-bearing connect factory authenticates before returning; a lazily constructed client authenticates exactly once before its first non-authentication request. Authentication failure closes opened connections and returns no usable client context. Invalid schemes, ports, missing database paths, malformed encoding, unsupported parameters, and conflicting explicit options fail before opening a socket.

The database path is selected immediately and attached to every request. Database selection remains mutable only through the compatibility switching method or shell `use` command. Each operation captures its database, collection, and authentication scope before asynchronous or blocking I/O begins so later selection changes cannot redirect an in-flight request.

Secrets are never included in generated Workbench URLs, saved CLI context, logs, exception messages, process examples, or diagnostic output. Errors and string representations redact URL passwords and tokens. Workbench produces a directly usable secret-free URL for local/default-auth connections and an environment-variable example for authenticated deployments. Both shells accept `--url`; the Node, Python, and Java packages accept the same URL grammar.

## Node SDK compatibility

Without `projectId`, `createDatabase(name, dbType)` sends the direct create request and sets `database` only after success. A constructor or connection URL containing a database uses it automatically; beginner examples never require `useDatabase`. The compatibility `useDatabase(name)` method checks `listDatabases`; `createCollection(name)` requires a database and relies on the engine for existence, namespace, and authorization checks. No catalog query is needed for projectless collection creation.

Retain `createProject`, `useProject`, the constructor's `projectId` option, and explicitly selected legacy project behavior. A client that explicitly opts into a project retains validated mapping/membership behavior; a new ordinary client does not need one. Existing legacy tests continue to cover that opt-in flow, with separate tests for the new direct flow. Engine errors propagate, and failure does not update client context.

## Java and Python SDK capability design

Keep `PacificDBClient` source compatibility and add the shorter `PacificDB` entry point shown in beginner documentation. Common usage is connect by URL, then create a collection and read/write data; selecting a database is not a separate step. Both languages expose deterministic close semantics (`with` in Python and `AutoCloseable` in Java), bounded thread-safe connection pools, connect/request timeouts, TLS hostname verification, and per-operation scope capture. Python uses only the standard library at runtime; Java retains Jackson as its only runtime dependency.

Both SDKs cover the supported public Community capability families: connection and capabilities; authentication and token lifecycle; database and collection lifecycle; insert, bulk insert/write, find, pagination, count, aggregate, explain, update, and delete variants; index creation/listing/validation/rebuild/deletion; vector insert/query; in-memory media; bounded file upload/download with SHA-256 verification and resumable state; media listing/deletion/cleanup; backup creation/listing/inspection/verification/export/restore/deletion; API-key lifecycle; health, metrics, and operation status; and advanced tenant, cluster, shard, configuration, replication, storage, and integrity operations.

A checked-in capability matrix enumerates every action dispatched by the Community server and classifies it as beginner SDK, advanced SDK, compatibility alias, or internal/test-only. Every public action has a named Java and Python entry point or a documented compatibility alias; only internal diagnostics and test-only actions may remain raw-request-only. Contract tests fail when a public matrix entry lacks either language implementation.

Beginner methods remain on the main client. Advanced families are grouped under discoverable accessors such as `media`, `vectors`, `indexes`, `backups`, `security`, and `admin` so destructive or operational calls do not clutter basic usage. Raw `request` remains available for forward compatibility. Method defaults, request fields, validation, engine-error preservation, and failure-safe context changes match across Node, Python, and Java where their language conventions allow.

Large media and backup transfers use bounded chunks and temporary destination files followed by atomic rename. They never buffer the whole file in memory. Interrupted media uploads expose typed resumable state. Write requests are not silently retried. Typed Python and Java exceptions retain the engine code, public message, response data, and relevant resumable-media fields without exposing credentials.

## Workbench flow and UI

Replace project → database → collection with database → collection. The primary action is **New database**; the empty state offers database creation, then collection creation once a database is selected. Creating or clicking a database selects and uses it immediately. Remove project statistics, project identity/copy controls, project deletion, project IDs, and any separate “use database” step from normal UI flows. Use the existing layout/components and CSS; do not add another framework or replace unrelated tools.

The connection dialog adds a **URL** option alongside CLI, Node, Python, and Java examples. It shows the current database-specific URL and disables copying until a database is selected. Examples construct clients from that URL and do not call `useDatabase`. The dialog never receives or renders engine passwords or tokens.

Show all accessible ordinary databases, including databases previously mapped to projects and databases created through direct APIs. Expand/render collections only for the current selected database; reuse the current selection model instead of adding a cross-database cache. Explicit refresh retains a valid selection, and deletion/missing selections produce a clear empty state. Keep keyboard navigation, mobile navigation, accessible labels, destructive confirmations and disabled controls while writes are pending.

Keep document/media pagination, editor protections for internal fields, query history scoped to database/collection, vector/index tools and native file dialogs. Scope identifiers captured for a mutation must remain attached to that operation even if the user changes selection while it runs.

## Workbench request path and responsiveness

Use `listDatabases` for navigation and `createDatabase` for creation. Remove Workbench project operations and project headers/body fields. Listing/selection performs engine-backed existence checks; ordinary document/vector/index requests validate input and send the selected database/collection directly to the engine. Avoid repeated catalog and list-all membership probes on each CRUD request. Do not weaken the server's Host/origin/session-token checks, payload limits or engine authorization.

Media lookup/download/delete must still compare manifest database and collection to the captured request scope. Removing project lookup must not permit downloading/deleting media belonging to another database or collection.

Load expensive collection counts/index totals only while the overview that displays them is visible, or on its explicit refresh. The document editor/query/media views must not start counts for every collection. Reuse the existing bounded four-worker loader, invalidation generation and scope checks; leaving overview or changing database invalidates obsolete results. Reuse current summaries within a valid selected-database view, mark affected summaries dirty after mutations, and refresh them when needed. Display unknown counts as unknown, not zero. Do not claim to cancel already-running engine queries merely because a browser request was aborted.

Keep per-request SDK clients for now; a shared mutable `client.database` would introduce concurrent scope races. Connection pooling redesign, virtualized navigation and engine query/count/index redesign are outside this package.

## Storage and compatibility boundary

No databases, collections, documents, project records, settings or user data are deleted or moved by this change. Existing catalog metadata remains readable through legacy APIs. No engine format, durable sync, Raft apply/acknowledgement order, RBAC or namespace-containment change is authorized here.

Friendly project shell commands are intentionally removed; document direct replacements. Existing raw engine/project APIs and SDK opt-in methods remain available. No package is published, no release tag is created, and no installer is deployed during implementation. Local artifacts and tag-gated publication workflows are prepared and validated.

## Java and Python publication readiness

Python metadata includes a PyPI-renderable README, license files, supported Python classifiers, project URLs, package discovery, and complete source/wheel contents. Verification builds an sdist and wheel, checks metadata and rendering with installed tooling, inspects contents, installs into a clean environment, and runs smoke/tests against the installed artifact. A tag-gated GitHub workflow uses PyPI Trusted Publishing with a protected production environment and does not store a long-lived token.

The Java POM includes required name, description, URL, licenses, developer, and SCM metadata. Release output contains binary, sources, and Javadoc JARs plus Central-compatible signatures and checksums. The Central publishing plugin is configured for validation and manual publication by default. Verification tests the packaged artifact from a local repository and checks bundle contents without requiring release credentials.

Registry project ownership, `io.pacificdb` namespace verification, PyPI trusted-publisher registration, signing keys, protected environments, and final human approval remain external prerequisites. “Production-ready” means the specified compatibility, safety, packaging, and verification gates pass; it does not claim zero defects.

## Source isolation

The original checkout has pending Workbench/desktop/package changes and an unrelated `engine/src/lsm.cpp` edit. Implementation uses `ux/database-first-20261003` at `.worktrees/database-first-20261003`, based on `6e65e55`. The relevant pending Workbench/desktop/package baseline was imported byte-for-byte with a file/hash manifest in commit `b44284f`; generated desktop engine binaries, the unrelated LSM edit, and unrelated site-migration documents were excluded. Never overwrite the original checkout. A final change report distinguishes imported baseline from new workflow and SDK changes.

Core change surfaces: `sdk/node`, `sdk/python`, `sdk/java`, `cli/src/{cli.js,shell.js,workbench.js}`, `cli/workbench/{app.js,index.html,app.css}`, `engine/src/{community_shell.cpp,shell.cpp}`, package/release workflows, and their current tests. Desktop consumes the same Workbench assets; update its smoke expectations and generated examples rather than adding another UI implementation. Update Node/Java/Python/CLI/Workbench/Community documentation where it describes mandatory projects or connection syntax.

## Acceptance and verification

1. Fresh Node, Python, and Java clients connect through a database URL or create a database, then create/query a collection without any project or `useDatabase` call. Explicit legacy Node project flow still passes.
2. Both shells accept the same connection URL. CLI context restoration works for old project-bearing files and direct `--database`; nonexistent databases, URL/flag conflicts, malformed URLs, and unauthorized/reserved namespaces are rejected with no context corruption or secret disclosure.
3. Browser and desktop tests start from a fresh root and create database → collection → document, then query/edit/delete. No project controls, project IDs, separate use step, or prerequisite instructions remain in normal flows. The connection dialog copies the selected database URL and URL-based examples.
4. Existing mapped and direct databases appear together through the current user's authorized database listing. No data migration occurs; restart retains existing data.
5. Media scope checks, malformed requests, request limits, read-only failures, stale navigation, destructive confirmations, vector/index operations and keyboard/mobile behavior stay covered by current checks adapted to direct scope.
6. Captured engine-request sequences show zero catalog calls in direct workflows. Navigating to Documents/Query/Media starts no all-collection summary scan; overview counts remain bounded and ignore obsolete selections. Record request-count/elapsed observations as UI diagnostics, not database benchmarks.
7. Java/Python contract tests cover every supported public capability family, malformed responses, error preservation, TLS, timeout, concurrency, pool shutdown, credential redaction, bounded streaming, resume, checksum failure, atomic destinations, and destructive-operation failures.
8. Build and inspect Python sdist/wheel and Java binary/source/Javadoc/Central bundle artifacts; install each locally and run smoke tests from the installed artifact. Publication workflows remain tag-gated and manually approved.
9. Run npm suites, native parser checks, native/npm CLI integration, Workbench API/browser/desktop smoke, SDK matrices, package-content checks, and a normal Release build. Reuse installed tooling and add no runtime dependency. Record platform/runtime limitations instead of claiming untested Windows/macOS or registry validation.

## Review status

The user approved the database-first compatibility choice, full Java/Python capability target, automatic database selection, connection URL design, safety model, and publication-readiness gates in conversation. The subsequent instruction to implement this active written spec approves it for planning. The [implementation program](../plans/2026-10-03-database-first-program.md) separates workflow, SDK capabilities and package qualification; written-plan review is the next gate. The imported baseline is isolated in `b44284f`; no new workflow or SDK implementation has begun.
