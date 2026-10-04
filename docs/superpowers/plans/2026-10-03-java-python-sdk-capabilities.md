# Java and Python SDK Capability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide simple direct-database Java/Python APIs, safe reusable connections and named coverage of supported Community actions.

**Architecture:** Preserve current `PacificDBClient` constructors and methods while adding a short `PacificDB` entry point. Keep URL parsing, JSON-line transport/pooling, errors, public operations and file transfers separate. A shared capability matrix records exact server actions and both language methods; wire tests and real-engine integration verify it.

**Tech Stack:** Python >=3.10 standard library; Java >=11 with existing Jackson; pytest and JUnit test dependencies; shared JSON URL fixtures from the workflow plan.

**Spec:** [Database-first design](../specs/2026-10-03-database-first-cli-workbench-design.md). Depends on workflow task 1's connection fixture, not on Workbench internals.

## Global Constraints

- “Keep `PacificDBClient` source compatibility and add the shorter `PacificDB` entry point shown in beginner documentation.”
- “Python uses only the standard library at runtime; Java retains Jackson as its only runtime dependency.”
- “Write requests are not silently retried.”
- “Large media and backup transfers use bounded chunks and temporary destination files followed by atomic rename.”
- “Each operation captures its database, collection, and authentication scope before asynchronous or blocking I/O begins so later selection changes cannot redirect an in-flight request.”
- “No engine format, durable sync, Raft apply/acknowledgement order, RBAC or namespace-containment change is authorized here.”
- “No package is published, no release tag is created, and no installer is deployed during implementation.”
- “Every public action has a named Java and Python entry point or a documented compatibility alias; only internal diagnostics and test-only actions may remain raw-request-only.”

## Review Focus

- A caller's dictionary/map or selected database changes while a request waits for a connection: wire bytes and captured principal/database remain fixed (tasks 1–3).
- A peer closes after a reply, advertises keepalive incorrectly, fragments UTF-8 or sends malformed/oversized JSON: reject corrupt frames and discard socket; never replay a sent write (tasks 1–2).
- Close/auth failure races with queued or active calls: every waiter wakes with a useful error and connections are closed (tasks 1–2).
- Response contains an array, nested engine error, credentials or resumable fields: preserve public contract while redacting diagnostic secrets (tasks 1–4).
- Large/corrupt/resumed media or backup manifest: enforce bounds/scope/checksums and preserve destination until verified atomic replacement (task 5).

---

## API and transport decisions

Python: `PacificDB` is an exported alias of `PacificDBClient`; `PacificDB.connect(url, **options)` is eager, `PacificDB.from_url(url, **options)` lazy. Existing host/port/user_id/database/use_tls/ca_file/timeout constructor parameters remain. Add keyword-only `pool_size=16`, `max_response_bytes=64*1024*1024`. URL `timeoutMs` converts to seconds; explicit Python option names map to the normalized fixture keys.

Java: `PacificDB` is a small final entry-point class whose `connect(String url)` and `fromUrl(String url)` return `PacificDBClient`; overloads accept `ConnectionOptions`. Existing client constructors and package-private injectable `SocketFactory` constructor remain. `PacificDBClient implements AutoCloseable`; keep `request(Map<String,Object>) -> Map<String,Object>` for source-compatible object responses, add `requestValue(Map<String,Object>) -> Object` for array/object protocol responses. The object-only method reports a typed unexpected-shape error if an array arrives; typed list methods use `requestValue`. Do not silently wrap an array into a different response.

Both: same URL keys/defaults/ranges as `sdk/contracts/connection-urls.json`; immutable connection settings, mutable selected database only through existing Python property/Java `useDatabase` context methods. Connection pool defaults 16, maximum 32, one in-flight request per socket. Waiting for a slot, connect/handshake and complete request have bounded deadlines; timeout invalidates the affected socket. An idle socket is reused only when the response explicitly advertises `_pacificdb_connection_keepalive=true` without `_pacificdb_connection_close=true`; otherwise close after reply, matching Node's compatibility contract. Strip these internal fields from returned objects. Array replies are accepted but do not advertise reuse. Close is idempotent and wakes waiting requests. There is no hidden reconnect-and-retry after a request was sent.

Serialize/copy a command with captured database/user/token scope before waiting for pool I/O. File operations capture one scoped requester before hashing/stat/networking and use it throughout. URL credentials authenticate once, before returning from eager factory or before any lazy non-authentication operation; failure closes the client. Error types subclass existing Python `RuntimeError` / Java `IllegalStateException` to preserve existing catch behavior, with `code`, sanitized public message and sanitized response available. Transport error explains that a sent write's outcome may be unknown, without auto-retrying it. Public representations and exception chains must not echo private credentials/URLs.

### Task 1: Python connection, framing, lifecycle and typed errors

**Files:** Create `sdk/python/pacificdb/connection.py`, `sdk/python/pacificdb/transport.py`, `sdk/python/pacificdb/errors.py`, `sdk/python/tests/test_connection.py`, `sdk/python/tests/test_transport.py`; modify `sdk/python/pacificdb/client.py`, `sdk/python/pacificdb/__init__.py`, `sdk/python/tests/test_client.py`.

**Interfaces:** `parse_connection_url(url: str, options: dict | None = None) -> dict`; `ConnectionPool.request(wire: bytes) -> Any`, `close() -> None`; `PacificDBError(RuntimeError)` with `code`, `response`; `MediaUploadError(PacificDBError)` with `upload_id`, `next_chunk`, `received_chunks`, `received_bytes`, `resumable`. Client adds classmethods `connect`/`from_url`, `close`, `__enter__`/`__exit__`; preserve current `request(command) -> Any` and `authenticate(username,password)` signatures.

- [x] Write fixture-based URL tests with zero sockets for invalid input. Fake JSON-line server tests assert fragmented Unicode, object/array response, keepalive reuse, legacy close-after-reply, error code/message preservation/redaction, EOF/non-JSON/oversized response, timeout, bounded simultaneous sockets, close waking queued callers, and captured scope/caller input during queue delay. Auth tests assert concurrent lazy calls send exactly one auth, no command before success, eager/lazy failure closes pool and never sends a write.
- [x] Run `PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests/test_connection.py sdk/python/tests/test_transport.py`; expect new API/lifecycle assertions to fail on baseline.
- [x] Implement strict stdlib URL decoding, credential-private connection configuration and lazy-auth coordination. Use a condition/lock to bound leased sockets; retain read framing per connection and deadline accounting across pool wait/read/write. Use standard verified TLS with `server_hostname`. Discard failed sockets; after send do not retry. Sanitize error diagnostics and implement deterministic close/context manager.
- [x] Add generated test-only trusted TLS server and assert matching host succeeds, wrong host/untrusted CA fails before credential bytes; include timeout during incomplete newline and socket-close race. Run complete Python suite. Commit as `feat: add safe pooled Python connections and simple URL client`.

### Task 2: Java connection, framing, lifecycle and typed errors

**Files:** Create under `sdk/java/src/main/java/io/pacificdb/`: `PacificDB.java`, `ConnectionOptions.java`, `ConnectionPool.java`, `PacificDBException.java`, `MediaUploadException.java`; create matching `ConnectionOptionsTest.java`, `ConnectionPoolTest.java` under `src/test/java/io/pacificdb`; modify `PacificDBClient.java`, `PacificDBClientTest.java`.

**Interfaces:** `ConnectionOptions.fromUrl(String url)`, `withOverrides(Map<String,Object>)`; immutable host/port/database/userId/tls/timeoutMs/poolSize/caFile settings with private credentials. `ConnectionPool.request(byte[] wire) -> Object`, `close()`. `PacificDBException extends IllegalStateException` getters `getCode()`, `getResponse()`. `MediaUploadException` adds resumable getters. `PacificDBClient` adds `requestValue`, `close`, static `fromUrl`/`connect` through `PacificDB` entry point, and scoped request construction.

- [x] Add tests using shared URL fixture and a reusable test-only JSON/TLS server: byte-for-byte scope/action preservation, object/array response handling, fragmented UTF-8, old peer/keepalive, typed engine errors, bad/oversized frame, lost reply without retry, pool bound, close while waiters/active calls exist, connect/request timeout, caller map mutation after scope capture and concurrent lazy authentication exactly once.
- [x] Run `mvn -B -q -f sdk/java/pom.xml -Dtest=ConnectionOptionsTest,ConnectionPoolTest,PacificDBClientTest test`; expect missing APIs/tests to fail before implementation.
- [x] Implement strict URI/UTF-8 percent decoding and verified SSL trust/hostname/SNI using standard Java facilities and existing Jackson. Use bounded leases and one socket per active call. Bound complete request including stalled write with a pool-owned deadline task that closes the affected socket; cancel tasks on completion and shut down timer on close. Preserve injected SocketFactory tests/old constructors; never swallow handshake errors or retry sent writes. Avoid duplicating CRUD on the `PacificDB` facade.
- [x] Run complete Maven test suite; retain current trusted/wrong-host TLS tests, add auth-failure shutdown/redaction assertions and confirm no thread/socket leaks after repeated close. Commit as `feat: add safe pooled Java connections and URL entry point`.

### Task 3: Common database and document methods

**Files:** Modify both client classes and exports; create `sdk/python/tests/test_operations.py`, `sdk/java/src/test/java/io/pacificdb/OperationsTest.java`; update both SDK READMEs.

**Interfaces:** Python snake_case and Java camelCase equivalents of `capabilities`, `create_database(name, db_type='binary')`, `list_databases()`, `use_database(name)`, `drop_database(name)`, `create_collection(name)`, `list_collections()`, `drop_collection(name)`, `insert(collection, document)`, `insert_many(collection, documents)`, `find(collection, filter={}, limit=-1, offset=0)`, `find_one(collection, filter={})`, `count(collection, filter={})`, `aggregate(collection, pipeline)`, `explain(collection, filter={})`, `update_one/update_many(collection, filter, update)`, `delete_one/delete_many(collection, filter)`, `bulk_write(collection, operations)`. Java adds no-filter/simple overloads and uses Map/List arguments for JSON. Return original protocol response values except documented `listDatabases` (list of names) and `findOne` (one row or null); existing Python methods keep response shapes/defaults.

- [x] Write request/response contract tests for every method, exact action/field/default shape and no mutation of caller JSON; test `createDatabase` selects only on success, rejected use/drop preserves context, successful selected-drop clears it, database-required calls fail clearly, actual array `listDatabases` works, and update/delete/bulk failures retain typed engine response. Assert direct create→collection→CRUD sends no catalog action.
- [x] Run Python `test_operations.py` and Java `OperationsTest`; expect missing methods and old no-selection behavior to fail.
- [x] Implement thin named methods over captured-scope request transport. Keep engine as authority for permissions, reserved namespaces and name validity; local checks give missing-context/type/pagination errors without pretending to authorize. Use verified listing for switching; create/drop change context only on success. Add basic URL/context-manager/try-with-resources examples without project/use prerequisite.
- [x] Run both full SDK suites and existing Node suite for wire/shape alignment. Commit as `feat: provide beginner database and document APIs in Java and Python`.

### Task 4: Capability matrix and advanced operation families

**Files:** Create `sdk/contracts/community-capabilities.json`, `docs/SDK_CAPABILITIES.md`, `scripts/test-sdk-capability-matrix.py`; create Python `pacificdb/operations.py`, `tests/test_capabilities.py`; Java `Operations.java` (named family types), `CapabilitiesTest.java`; modify client accessors and existing Community contract harness as needed.

**Interfaces:** Matrix rows contain `action`, `classification` (`beginner`, `advanced`, `alias`, `internal`), `python`, `java`, `required_fields`, `scope`, `alias_of` when applicable, and `reason` for every internal entry. Python uses `client.media`, `.vectors`, `.indexes`, `.backups`, `.security`, `.admin`; Java uses `media()`, `vectors()`, `indexes()`, `backups()`, `security()`, `admin()`. Family methods accept keyword/options maps using exact engine field names and return decoded original response, with fixed action protected from options override. Required scope is attached by the client; options cannot silently replace captured principal/token/database. Raw request retains forward-compatibility escape hatch.

- [x] Enumerate dispatch guards and delegated handlers reachable in `engine/src/server.cpp`; author the matrix against source/engine contracts. Include public project/catalog operations as named advanced compatibility APIs (never used by beginner flows). Map wire aliases to one documented canonical method. Classify only actual failpoints/private diagnostics/test actions internal, with source reason; do not hide unimplemented public methods by relabeling them internal. Existing Community restrictions must be documented, not represented as working features.
- [x] Add coverage tests that fail for omitted public dispatch, missing language callable, wrong alias or duplicate action. Parameterized fake-server wire tests invoke every public matrix entry with required fixture fields, assert its exact action, scope and response/error behavior. Run `python3 scripts/test-sdk-capability-matrix.py`, Python capabilities and Java `CapabilitiesTest`; expect absent family methods to fail.
- [x] Implement thin named family methods for indexes (create/list/validate/rebuild/drop), vectors (insert/query), media (begin/chunk/finalize/get/list/delete/cleanup), backups (create/list/get/verify/restore/delete/list-restores/export manifest/chunks), security (authenticate/refresh/validate/whoami/API-key create/list/get/revoke/metrics), admin (health/metrics/status and public tenant/cluster/shard/config/replication/storage/integrity actions). Names are normal snake_case/camelCase conversions of canonical actions after removing family prefix; the matrix explicitly binds each name. Preserve true wire payloads and expose unsupported engine behavior as typed errors, not emulated success.
- [x] Run all matrix/wire tests. Add capability docs with beginner vs advanced groups, auth prerequisites, destructive operations and exact alias mapping. Commit as `feat: cover Community capabilities in Java and Python SDKs`.

### Task 5: Bounded media and backup file APIs

**Files:** Create Python `pacificdb/files.py`, `tests/test_files.py`; Java `FileTransfers.java`, `FileTransfersTest.java`; modify media/backup accessors, both README/API docs and current Node transfer tests as contract references.

**Interfaces:** Python `media.put(collection,id,data,metadata={})`, `get(collection,id)`, `upload_file(collection,path,content_type=None,chunk_bytes=None,resume=None)`, `download_file(media_id,destination)`; Java equivalent `put`, `get`, `uploadFile(String,Path,...)`, `downloadFile(String,Path)` with simple overloads. Backups `export(backup_id,destination,chunk_bytes=1048576)` / `export(String,Path,...)`. Returned media manifest and resumable error fields match current Node protocol. Upload chunks maximum 4 MiB source, minimum 64 KiB, further bounded by negotiated request/media limits minus 64 KiB JSON reserve; backup chunks 1 byte–1 MiB. All file paths use native pathlib/Path.

- [x] Write tests with files larger than one chunk: exact in-memory media roundtrip, negotiated JSON wire limit, short/invalid capabilities, interrupted upload exposes progress, resume skips recorded chunks, wrong resume scope/hash fails, file changed after hashing fails final checksum, strict base64/chunk/whole SHA-256/size/index validation, no whole-file buffering and destination preserved on transfer/checksum/rename failure. Include wrong database/collection manifest and selection changes during hashing/chunk I/O. Backup tests reject unsafe/duplicate manifest paths, wrong offsets, zero-progress chunks and invalid whole-file hashes; preserve existing destination.
- [x] Run both file test suites; expect missing APIs and atomic/bounded assertions to fail.
- [x] Implement sequential bounded hashing/chunks and private scoped requester. Capture lifecycle at entry, validate manifest dimensions and protocol bounds before loops/allocations, use stdlib strict base64 and SHA-256. Upload exposes structured resume state on interruption; no silent write replay. Download/export create unique owner-only sibling temporary file and atomically replace destination after validation; clean own temp files on failure, never overwrite another transfer's `.part`. Export the current Node backup JSON format as a streamed document. Java unsupported atomic replacement must fail with destination intact, never silently downgrade to unsafe copy.
- [x] Run SDK file suites plus real-engine media/backup tests from task 6; confirm received values and hashes after restart. Commit as `feat: add verified streaming media and backup transfers to SDKs`.

### Task 6: Authenticated real-engine SDK qualification and examples

**Files:** Create `scripts/test-cross-sdk-e2e.mjs`, Python `tests/integration_client.py`, Java `src/test/java/io/pacificdb/IntegrationClient.java`, `docs/evidence/2026-10-03-java-python-sdk.md`; update SDK READMEs and `scripts/test-community.sh`.

**Interfaces:** Node controller owns only disposable engine roots/free ports and spawns language harnesses against a passed engine build; harnesses accept endpoint/temporary directory without logging passwords. Java/Python run same semantic vectors for database/document/index/vector/media/backup/security/admin families. Advanced cluster topology is not changed on an existing server.

- [x] Add harness assertions: direct no-project create/select/collection CRUD, changing values, pagination/count/aggregate/explain/bulk, missing/reserved/unauthorized database, read-only key and revoked key errors, legacy project APIs, index/vector results, transfer checksums/resume, backup verify/export/restore, and operation/health status. Check exact expected application fields before and after forced engine restart. Wrong-host TLS test lives in SDK unit suites, actual authenticated TCP/TLS smoke is included here.
- [x] Run `node scripts/test-cross-sdk-e2e.mjs build` against the implemented clients. Diagnose any failed semantic assertion before changing code; a newly added integration check that already passes does not need an artificial regression to create a RED result.
- [x] Finish integration harness and short copyable examples: Python `with PacificDB.connect(url) as db`, Java `try (var db = PacificDB.connect(url))`; database URL supplies selection. Document server must be running, auth/TLS environment configuration, typed exceptions, no implicit retries and concurrency/scope rules. Integrate matrix/SDK checks into Community gate without asserting unavailable platform/registry results.
- [x] Run `PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests`, `mvn -B -q -f sdk/java/pom.xml test`, capability validator, `node scripts/test-cross-sdk-e2e.mjs build`, then full `scripts/test-community.sh build`. Record exact versions, source revision and exclusions (including missing mixed-version artifact). Commit as `test: qualify Java and Python clients against authenticated engine`.
