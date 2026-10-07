# Security Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close all nine findings from the `89735a9` security scan and produce test-backed, exact-revision release evidence.

**Architecture:** Persist database ownership and grants in replicated database metadata and enforce them once at the engine request boundary, with resource-derived checks for media. Apply narrow bounds at Raft, media, SDK, lockout, configuration, and regex boundaries; use RE2 where the standard library cannot provide bounded matching.

**Tech Stack:** C++17, CMake/CTest, OpenSSL, LZ4, RE2, Node.js test runner, Electron/Workbench scripts, Python evidence tooling.

**Spec:** `docs/superpowers/specs/2026-10-07-security-remediation-design.md`

## Global Constraints

- Work only in `pacificdb-community-production-readiness` on `codex/workbench-production-readiness`; do not touch the dirty original checkout.
- Preserve authentication-disabled managed-local behavior and authenticated clustered production behavior.
- Treat a database as `(userId, dbName)`; `system` is not an authorization grant.
- Effective data permission is the lower of global/API-key role and database grant; `SUPERADMIN` overrides are audited.
- Existing databases without ownership metadata remain superadmin-only until assigned; no stored data is moved.
- Invalid and oversized inputs fail before allocation or expensive work; do not silently truncate or reinterpret them.
- Every task follows red-green TDD and commits only its focused source and regression tests.
- Do not claim independent external review without a separate reviewer or organization signing the exact-revision bundle.

## Review Focus

- An API key created by an admin must retain its own role ceiling while resolving the creator principal for database grants; Task 2 tests this.
- An idempotent `createDatabase` request must not claim or replace ownership of an existing database; Task 1 tests this.
- A bulk request with a safe top-level scope but a foreign nested scope or delete operation must be rejected before any suboperation runs; Task 2 tests this.
- A media request using the `db` alias or an ID from another database must not bypass filtering or reveal the foreign resource; Task 3 tests this.
- Config reload must not report that an already-bound listener moved; Task 4 tests startup-effective status separately from mutable configured values.

---

### Task 1: Replicated database ownership metadata

**Files:**
- Modify: `engine/include/database_engine.hpp`
- Modify: `engine/src/database_engine.cpp`
- Modify: `engine/src/server.cpp`
- Modify: `engine/CMakeLists.txt`
- Create: `engine/test/database_access_test.cpp`

**Interfaces:**
- Produces: `DatabaseEngine::getDatabaseSecurity(userId, dbName) -> json` and `DatabaseEngine::setDatabaseSecurity(userId, dbName, security) -> bool`.
- Produces: `DatabaseEngine::createDatabase(..., security = json::object())`, preserving existing unauthenticated callers through the default argument.
- Persists `{version:1, owners:[...], grants:{principal:level}}` inside `db.meta` through atomic replacement.
- Produces replicated actions for initial assignment, grant, revoke, co-owner grant, and ownership transfer.

- [ ] **Step 1: Write the failing metadata tests**

  Add tests for creator ownership, missing-security metadata being unassigned,
  idempotent create preserving the first owner, refusing removal of the last
  owner, atomic transfer, and round-trip persistence after reopen.

- [ ] **Step 2: Run the focused test and verify RED**

  Run: `cmake --build build -j2 --target db_engine_database_access_test && ctest --test-dir build -R db_engine_database_access_test --output-on-failure`

  Expected: build or assertions fail because the ownership APIs and metadata do not exist.

- [ ] **Step 3: Implement the minimal metadata and Raft apply support**

  Reuse the existing `db.meta` atomic-update pattern. Validate version, owner
  names, grant levels, and nonempty final owner set at the mutation boundary.
  Include the authenticated creator in the replicated create entry; never let
  replay or idempotent create overwrite existing security metadata.

- [ ] **Step 4: Run the focused test and adjacent metadata tests**

  Run: `cmake --build build -j2 --target db_engine_database_access_test db_engine_comprehensive_test && ctest --test-dir build -R 'db_engine_(database_access|comprehensive)_test' --output-on-failure`

  Expected: PASS.

- [ ] **Step 5: Commit**

  Run: `git add engine/include/database_engine.hpp engine/src/database_engine.cpp engine/src/server.cpp engine/CMakeLists.txt engine/test/database_access_test.cpp && git commit -m "fix: persist replicated database ownership"`

### Task 2: Central database authorization and ACL operations

**Files:**
- Modify: `engine/include/security_manager.hpp`
- Modify: `engine/src/security_manager.cpp`
- Modify: `engine/src/server.cpp`
- Modify: `scripts/test-engine-auth-boundaries.mjs`

**Interfaces:**
- Consumes: database security metadata from Task 1.
- Produces: one request-scope authorization path resolving principal, global role, `(userId, dbName)`, ACL level, and audited superadmin override.
- Produces protocol actions to list unassigned databases; assign/inspect/transfer ownership; and grant/revoke database access.

- [ ] **Step 1: Extend the real-engine auth test and verify RED**

  Add two namespaces with colliding database names and principals for owner,
  read-only grantee, read-write grantee, denied user, API keys, and superadmin.
  Assert global-role/grant intersection, audited override, unassigned migration,
  ACL mutations, and rejection of nested bulk foreign scope/delete before any
  mutation.

  Run: `node scripts/test-engine-auth-boundaries.mjs build`

  Expected: FAIL because request `userId` is not tied to database ownership.

- [ ] **Step 2: Implement central scope authorization**

  Resolve JWT/API-key principal through existing SecurityManager helpers. Check
  global permission first, then database ACL; special-case only new database
  creation and authenticated ACL administration. Validate every bulk
  suboperation's effective action and scope before dispatch. Keep auth-disabled
  local and trusted Raft/recovery execution unchanged.

- [ ] **Step 3: Implement audited ACL protocol operations and fail-closed bootstrap**

  Route ACL mutations through the replicated Task 1 actions. Reject a new
  authenticated production data root with neither an existing superadmin nor
  both operator bootstrap values. Emit actor, database tuple, operation, target
  principal, and outcome for grants, revocations, ownership changes, and
  superadmin overrides.

- [ ] **Step 4: Run focused and neighboring authentication checks**

  Run: `cmake --build build -j2 --target db_engine && node scripts/test-engine-auth-boundaries.mjs build && node scripts/test-community-contract-matrix.mjs build`

  Expected: PASS.

- [ ] **Step 5: Commit**

  Run: `git add engine/include/security_manager.hpp engine/src/security_manager.cpp engine/src/server.cpp scripts/test-engine-auth-boundaries.mjs && git commit -m "fix: enforce database ownership at request boundary"`

### Task 3: Resource-derived media authorization

**Files:**
- Modify: `engine/include/community_catalog.hpp`
- Modify: `engine/src/community_catalog.cpp`
- Modify: `engine/src/server.cpp`
- Modify: `scripts/test-engine-auth-boundaries.mjs`
- Modify: `engine/test/community_catalog_test.cpp`

**Interfaces:**
- Consumes: central database authorizer from Task 2.
- Produces: side-effect-free manifest scope lookup for ID-based media authorization.
- Preserves: begin/resume/finalize idempotency and existing public not-found errors for inaccessible foreign IDs.

- [ ] **Step 1: Add cross-database media tests and verify RED**

  Cover get, chunk, put, finalize, ready resume, delete, targeted cleanup,
  unfiltered list, `db` alias list, and namespace cleanup using one allowed and
  one forbidden database. Assert foreign ID operations look missing and global
  operations neither expose nor mutate foreign resources.

  Run: `cmake --build build -j2 --target db_engine_community_catalog_test db_engine && ctest --test-dir build -R db_engine_community_catalog_test --output-on-failure && node scripts/test-engine-auth-boundaries.mjs build`

  Expected: FAIL on the baseline authorization behavior.

- [ ] **Step 2: Authorize stored scope before media work**

  Resolve a manifest without progress reconstruction, authorize its immutable
  database under the media lock, canonicalize `db`/`dbName`, filter lists before
  pagination output, and constrain global cleanup to caller-deletable resources.

- [ ] **Step 3: Run focused media and auth checks**

  Run: `cmake --build build -j2 --target db_engine_community_catalog_test db_engine && ctest --test-dir build -R db_engine_community_catalog_test --output-on-failure && node scripts/test-engine-auth-boundaries.mjs build && node scripts/test-p0-media.mjs build`

  Expected: PASS.

- [ ] **Step 4: Commit**

  Run: `git add engine/include/community_catalog.hpp engine/src/community_catalog.cpp engine/src/server.cpp scripts/test-engine-auth-boundaries.mjs engine/test/community_catalog_test.cpp && git commit -m "fix: authorize media by owning database"`

### Task 4: Fail-closed listener configuration

**Files:**
- Modify: `desktop/engine.mjs`
- Modify: `cli/src/local-engine.js`
- Modify: `engine/src/local_engine.cpp`
- Modify: `engine/src/server.cpp`
- Modify: `engine/src/raft_core.cpp`
- Modify: `scripts/test-desktop-engine.mjs`
- Modify: `scripts/test-engine-auth-boundaries.mjs`
- Modify: `engine/test/local_engine_state_test.cpp`

**Interfaces:**
- Produces: managed-local engine and Raft loopback binding in all launchers.
- Produces: one resolved startup configuration source for bind/auth enforcement and reported effective listener values.

- [ ] **Step 1: Add runtime socket/config tests and verify RED**

  Assert both managed listeners are loopback-only, invalid engine and Raft hosts
  fail, file-only bind/auth values are the values actually enforced, and a
  config reload does not misreport the live startup bind.

  Run: `cmake --build build -j2 --target db_engine db_engine_local_engine_state_test && node scripts/test-desktop-engine.mjs build && node scripts/test-engine-auth-boundaries.mjs build`

  Expected: FAIL because Raft binds wildcard and listeners bypass merged config.

- [ ] **Step 2: Implement resolved startup settings and loopback launchers**

  Reuse `EnvConfig` for effective bind/auth values, snapshot listener settings
  at startup for status, set `RAFT_BIND_HOST=127.0.0.1` in each managed launcher,
  and make invalid addresses fatal in both listeners.

- [ ] **Step 3: Run focused and deployment checks**

  Run: `cmake --build build -j2 --target db_engine db_engine_local_engine_state_test && node scripts/test-desktop-engine.mjs build && node scripts/test-engine-auth-boundaries.mjs build && python3 scripts/test-deployment-contract.py`

  Expected: PASS.

- [ ] **Step 4: Commit**

  Run: `git add desktop/engine.mjs cli/src/local-engine.js engine/src/local_engine.cpp engine/src/server.cpp engine/src/raft_core.cpp scripts/test-desktop-engine.mjs scripts/test-engine-auth-boundaries.mjs engine/test/local_engine_state_test.cpp && git commit -m "fix: fail closed on managed listener binds"`

### Task 5: Bound Raft wire frames before allocation

**Files:**
- Modify: `engine/src/raft_core.cpp`
- Create: `scripts/test-raft-frame-limit.mjs`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- Enforces: `0 < frame length <= kMaxRaftPayloadBytes` before allocation and before sender narrowing.

- [ ] **Step 1: Add the raw-socket regression and verify RED safely**

  Start a disposable loopback engine, send a `64 MiB + 1` header without a body,
  assert prompt connection rejection, bounded RSS growth, and continued client
  health. Do not send a multi-gigabyte declaration.

  Run: `node scripts/test-raft-frame-limit.mjs build`

  Expected: FAIL because the receiver allocates and waits for the declared body.

- [ ] **Step 2: Add pre-allocation receiver and sender checks**

  Reject zero/oversized lengths before constructing the payload, use size-safe
  counters, and reject oversized outbound payloads before converting to
  `uint32_t`.

- [ ] **Step 3: Run Raft protocol and snapshot checks**

  Run: `cmake --build build -j2 --target db_engine db_engine_raft_binary_log_test db_engine_raft_snapshot_metadata_test && node scripts/test-raft-frame-limit.mjs build && ctest --test-dir build -R 'db_engine_raft_(binary_log|snapshot_metadata)_test' --output-on-failure`

  Expected: PASS.

- [ ] **Step 4: Commit**

  Run: `git add engine/src/raft_core.cpp scripts/test-raft-frame-limit.mjs scripts/test-community.sh && git commit -m "fix: bound Raft frames before allocation"`

### Task 6: Bound sparse media work by stored chunks

**Files:**
- Modify: `engine/src/community_catalog.cpp`
- Modify: `engine/test/community_catalog_test.cpp`

**Interfaces:**
- Produces: bounded-page enumeration/deletion of actual `media_chunks` rows.
- Preserves: out-of-order chunks, duplicate retries, restart reconciliation, and legacy manifests.

- [ ] **Step 1: Add sparse-manifest tests and verify RED**

  Exercise huge declared counts with zero chunks and one high-index stored chunk
  through get, delete, cleanup, and reconcile in an externally bounded test.

  Run: `cmake --build build -j2 --target db_engine_community_catalog_test && timeout 30s build/db_engine_community_catalog_test`

  Expected: timeout or assertion failure because work follows the declared range.

- [ ] **Step 2: Replace declared-range discovery with actual-row paging**

  Reuse the existing delete-from-offset-zero pattern. Page and order real rows,
  preserve query-result byte ceilings, and validate stored indices against the
  manifest without probing absent indices.

- [ ] **Step 3: Run media lifecycle checks**

  Run: `cmake --build build -j2 --target db_engine_community_catalog_test db_engine_media_upload_state_test && ctest --test-dir build -R 'db_engine_(community_catalog|media_upload_state)_test' --output-on-failure && node scripts/test-p0-media.mjs build`

  Expected: PASS.

- [ ] **Step 4: Commit**

  Run: `git add engine/src/community_catalog.cpp engine/test/community_catalog_test.cpp && git commit -m "fix: bound media cleanup to stored chunks"`

### Task 7: Cap Node SDK response buffering

**Files:**
- Modify: `sdk/node/src/index.js`
- Modify: `sdk/node/test/connection.test.js`

**Interfaces:**
- Enforces: 64 MiB raw response ceiling before decode/append.
- Preserves: fragmented UTF-8, connection-pool recovery, and no automatic write replay.

- [ ] **Step 1: Add oversized response tests and verify RED**

  Cover an unterminated stream and a single newline-terminated oversized chunk;
  assert one rejection, socket retirement, bounded buffering, and success of a
  later request on a fresh connection.

  Run: `node --test sdk/node/test/connection.test.js`

  Expected: FAIL because the client has no response byte ceiling.

- [ ] **Step 2: Count raw bytes and retire oversized connections**

  Reset the counter for every request and terminal connection state. Check the
  byte ceiling before passing data to `StringDecoder` or concatenating strings.

- [ ] **Step 3: Run the full Node SDK suite**

  Run: `npm test --workspace @pacificdb/client`

  Expected: PASS.

- [ ] **Step 4: Commit**

  Run: `git add sdk/node/src/index.js sdk/node/test/connection.test.js && git commit -m "fix: cap Node response buffering"`

### Task 8: Persist real account lockout expiry

**Files:**
- Modify: `engine/include/security_manager.hpp`
- Modify: `engine/src/security_manager.cpp`
- Create: `engine/test/security_manager_lockout_test.cpp`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Adds: persisted `lockedUntil` epoch seconds on `User`.
- Enforces: immediate attempts remain locked; expiry or admin password reset clears state.

- [ ] **Step 1: Add deterministic lockout tests and verify RED**

  Test threshold lock, immediate correct-password rejection, zero-duration
  expiry, persistence across reload, password-reset clearing, and a legacy
  locked record without a timestamp remaining locked.

  Run: `cmake --build build -j2 --target db_engine_security_manager_lockout_test && ctest --test-dir build -R db_engine_security_manager_lockout_test --output-on-failure`

  Expected: build or assertions fail because the next attempt clears the lock.

- [ ] **Step 2: Store and enforce the deadline**

  Set the deadline only when entering the locked state, compare it before
  password verification, persist state transitions, and clear all lock fields
  on success or administrator password reset.

- [ ] **Step 3: Run focused and engine auth checks**

  Run: `cmake --build build -j2 --target db_engine_security_manager_lockout_test db_engine && ctest --test-dir build -R db_engine_security_manager_lockout_test --output-on-failure && node scripts/test-engine-auth-boundaries.mjs build`

  Expected: PASS.

- [ ] **Step 4: Commit**

  Run: `git add engine/include/security_manager.hpp engine/src/security_manager.cpp engine/test/security_manager_lockout_test.cpp engine/CMakeLists.txt && git commit -m "fix: enforce persisted account lockout expiry"`

### Task 9: Replace unbounded query regex with RE2

**Files:**
- Modify: `vcpkg.json`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/src/query_parser.cpp`
- Modify: `engine/include/query.hpp`
- Modify: `engine/test/community_query_test.cpp`
- Modify: `.github/workflows/ci.yml`
- Modify: `.github/workflows/codeql.yml`
- Modify: `.github/workflows/release.yml`
- Modify: `.github/workflows/workbench-desktop.yml`
- Modify: `.github/workflows/sdk-packages.yml`
- Modify: `deploy/docker/Dockerfile`
- Modify: `THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Produces: RE2 partial-match semantics with `i`, a 4 KiB pattern ceiling, and explicit errors for invalid/unsupported syntax or options.

- [ ] **Step 1: Add valid, unsupported, and adversarial tests and verify RED**

  Assert normal search and case-insensitive matching, explicit rejection of
  backreferences/lookarounds/unknown options/oversized patterns, and bounded
  completion of a catastrophic-backtracking pattern in a subprocess.

  Run: `cmake --build build -j2 --target db_engine_community_query_test && timeout 20s build/db_engine_community_query_test`

  Expected: timeout or assertion failure under `std::regex` semantics.

- [ ] **Step 2: Add RE2 and replace the shared matcher**

  Add repository-native package declarations for every release platform. Cache
  compiled immutable RE2 objects, validate at parse time, and use partial match
  at the single shared evaluator reached by find/count/vector/update/delete/
  aggregate/nested filters.

- [ ] **Step 3: Run query, build-contract, and package-definition checks**

  Run: `cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 --target db_engine_community_query_test db_engine_query_limit_test db_engine_timeout_test && ctest --test-dir build -R 'db_engine_(community_query|query_limit|timeout)_test' --output-on-failure && python3 scripts/test-deployment-contract.py`

  Expected: PASS.

- [ ] **Step 4: Commit**

  Run: `git add vcpkg.json engine/CMakeLists.txt engine/src/query_parser.cpp engine/include/query.hpp engine/test/community_query_test.cpp .github/workflows deploy/docker/Dockerfile THIRD_PARTY_NOTICES.md && git commit -m "fix: use bounded query regex matching"`

### Task 10: Integrated review, verification, and exact-revision evidence

**Files:**
- Modify only if a confirmed bypass/regression from the required review is within the approved finding boundaries.
- Generate ignored artifacts under `build/` using repository-owned scripts.

**Interfaces:**
- Consumes: Tasks 1-9.
- Produces: one immutable remediation revision, fresh security report, qualification status, load/power evidence, and external-review bundle.

- [ ] **Step 1: Run the required fresh bypass/regression review**

  Give a fresh read-only reviewer only the nine findings, repository policy,
  current candidate diff, and authorized scope. Confirm every reported
  hypothesis in source or focused execution; apply at most one correction
  cycle and rerun affected checks.

- [ ] **Step 2: Run narrow-to-broad verification**

  Run: `cmake --build build -j2`

  Run: `bash scripts/test-community.sh build`

  Run: `npm run test:npm`

  Run: `npm run test:workbench:browser && npm run test:workbench:desktop`

  Run: `python3 -m unittest scripts/test_security_review.py -v`

  Run: `npm audit --omit=dev --json`

  Expected: every required check exits zero; any unavailable platform remains explicitly unverified.

- [ ] **Step 3: Inspect and commit the integrated source revision**

  Run: `git diff --check && git status --short && git diff --stat`

  Commit any review corrections, then record `git rev-parse HEAD`. Do not modify tracked source after evidence starts.

- [ ] **Step 4: Rerun repository-owned security and qualification workflows**

  Invoke `codex-security:security-scan` against the whole immutable HEAD,
  regenerate dependency and available-platform package evidence with the
  repository qualification scripts, and update the release-status artifact
  without treating internal review as external.

- [ ] **Step 5: Run endurance and physical-power evidence on the immutable HEAD**

  Start the eight-hour workload and retain its result. Run the physical-power
  harness against the USB-backed test host and pause only when the operator must
  cut or restore power. Both artifacts must name the same HEAD.

- [ ] **Step 6: Generate and validate the external-review bundle**

  Run: `python3 scripts/security_review.py bundle --revision "$(git rev-parse HEAD)" --output build/external/security-review`

  Deliver the bundle to a separate reviewer. When their signed
  `review-result.json` is returned, run:

  `python3 scripts/security_review.py validate --bundle build/external/security-review --review-result build/external/security-review/review-result.json`

  Until then, report the independent-review gate as blocked rather than complete.
