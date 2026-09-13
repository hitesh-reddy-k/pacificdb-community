# PacificDB P0 Lifecycle and Media Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make local-engine lifecycle, startup discovery, request framing, and resumable media reliable across Windows, macOS, and Linux.

**Architecture:** Introduce small cross-platform socket and local-process boundaries, keep the engine independent from client lifetime, and persist media progress as a crash-reconcilable state machine in the existing reserved Community catalog. Prove behavior through public protocol and installed-package tests, then validate the actual installer on the supplied Windows SSH host.

**Tech Stack:** C++17, Winsock/Win32 process APIs, POSIX sockets/process APIs, nlohmann/json, OpenSSL, Node.js 18+ standard library, CMake, `node:test`, GitHub Actions, NSIS, CPack.

**Spec:** `docs/superpowers/specs/2026-09-13-p0-lifecycle-media-reliability-design.md`

## Global Constraints

- Support Windows x64, macOS Intel/Apple silicon, and Linux amd64 without changing the public command names.
- Preserve the numeric `engine.pid`, newline-JSON protocol, binary-wire protocol, existing media reads, and existing `uploading`/`ready` JSON values.
- Persist new media states as lowercase `uploading`, `verifying`, `ready`, `failed`, and `aborted`.
- `--no-start` must never spawn, stop, signal, adopt, or clean up a process.
- Never remove a live DATA_ROOT lock or delete `ready` media from cleanup.
- Never acknowledge a write or media finalization before its existing durability contract succeeds.
- Never log credentials, API keys, tokens, document bodies, media bytes, or Base64 chunks.
- Use isolated temporary roots and dynamic ports for every automated and remote test.
- Preserve existing user data and installations on `hites@10.0.0.9`.
- Do not publish a release after P0 alone; P1 and P2 plus final end-to-end checks must also pass.
- Per the user's instruction, do not create intermediate commits. Keep all task checkpoints uncommitted and make one commit only after all requested work is finished and verified.

## File Structure

- `engine/include/socket_runtime.hpp`, `engine/src/socket_runtime.cpp`: cross-platform socket timeout encoding, receive-error classification, and retryable-send waiting.
- `engine/include/local_engine.hpp`, `engine/src/local_engine.cpp`: native local-engine observations, state classification, metadata, process identity, spawn, and readiness monitoring.
- `engine/include/structured_event.hpp`, `engine/src/structured_event.cpp`: secret-safe JSON lifecycle/media event formatting.
- `engine/include/media_upload_state.hpp`, `engine/src/media_upload_state.cpp`: legal media transitions, progress reconstruction, and lease decisions.
- `engine/src/shell.cpp`: command parsing and protocol client; delegates startup to `local_engine` and surfaces resumable native-upload errors.
- `engine/src/server.cpp`: uses socket runtime, returns complete framed errors, publishes health identity, and records lifecycle/media events.
- `engine/src/main.cpp`, `engine/src/storage_root_guard.cpp`: engine lifecycle logging, fatal boundary, and readable lock-owner metadata.
- `engine/src/community_catalog.cpp`: durable media operations and reconciliation using the pure state helper.
- `cli/src/local-engine.js`: npm launcher with the same discovery states and log behavior.
- `sdk/node/src/index.js`: resumable upload progress and `MediaUploadError`.
- `engine/test/*`, `sdk/node/test/*`, `cli/test/*`, `scripts/test-p0-*.mjs`: focused, integration, restart, load, and package checks.

---

### Task 1: Correct socket timeouts and guarantee protocol error frames

**Files:**
- Create: `engine/include/socket_runtime.hpp`
- Create: `engine/src/socket_runtime.cpp`
- Create: `engine/test/socket_runtime_test.cpp`
- Create: `scripts/test-p0-protocol-errors.mjs`
- Modify: `engine/src/server.cpp:29-57, 2030-2280, 6200-6255`
- Modify: `engine/CMakeLists.txt:108-252`
- Modify: `scripts/test-community.sh:4-36`

**Interfaces:**
- Produces: `pacificdb::net::NativeSocket` and `pacificdb::net::invalidSocket`.
- Produces: `SocketTimeoutOption makeSocketTimeoutOption(std::chrono::milliseconds)` with `const char* data()` and portable `int size()` (the Windows `setsockopt` length type is `int`; POSIX callers perform a checked cast to `socklen_t`).
- Produces: `void setSocketTimeouts(NativeSocket, std::chrono::milliseconds receive, std::chrono::milliseconds send)`; throws with the socket error when either option fails.
- Produces: `ReceiveFailure classifyReceiveFailure(int result, int nativeError)` returning `none`, `timeout`, `disconnected`, `reset`, or `other`.
- Consumes: the existing `clientTransportRecv`, `clientTransportSend`, JSON framing, and binary `PDB2` framing in `server.cpp`.

- [x] **Step 1: Add the failing timeout encoding test**

```cpp
#include "socket_runtime.hpp"
#include <cassert>
#include <chrono>
#include <system_error>

int main() {
    using namespace std::chrono_literals;
    const auto encoded = pacificdb::net::makeSocketTimeoutOption(2s);
#ifdef _WIN32
    assert(encoded.size() == sizeof(DWORD));
    assert(encoded.millisecondsForTest() == 2000);
    assert(pacificdb::net::classifyReceiveFailure(-1, WSAETIMEDOUT) ==
           pacificdb::net::ReceiveFailure::timeout);
    assert(pacificdb::net::classifyReceiveFailure(-1, WSAECONNRESET) ==
           pacificdb::net::ReceiveFailure::reset);
#else
    assert(encoded.size() == sizeof(timeval));
    assert(encoded.millisecondsForTest() == 2000);
    assert(pacificdb::net::classifyReceiveFailure(-1, EAGAIN) ==
           pacificdb::net::ReceiveFailure::timeout);
    assert(pacificdb::net::classifyReceiveFailure(-1, ECONNRESET) ==
           pacificdb::net::ReceiveFailure::reset);
#endif

    bool rejectedInvalidSocket = false;
    try {
        pacificdb::net::setSocketTimeouts(
            pacificdb::net::invalidSocket, 2s, 2s);
    } catch (const std::system_error&) {
        rejectedInvalidSocket = true;
    }
    assert(rejectedInvalidSocket);
}
```

- [x] **Step 2: Run the focused target and verify RED**

Run:

```sh
cmake -S engine -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j2 --target db_engine_socket_runtime_test
```

Expected: compilation fails because `socket_runtime.hpp` and the target do not exist. On Windows, preserve a separate baseline run showing the old server's `timeval` option cannot represent the intended 2,000 ms Winsock timeout.

- [x] **Step 3: Implement the platform encoding and checked setters**

```cpp
namespace pacificdb::net {
enum class ReceiveFailure { none, timeout, disconnected, reset, other };

class SocketTimeoutOption {
public:
    const char* data() const noexcept;
    int size() const noexcept;
    long long millisecondsForTest() const noexcept;
private:
#ifdef _WIN32
    DWORD value_ = 0;
#else
    timeval value_{};
#endif
    friend SocketTimeoutOption makeSocketTimeoutOption(
        std::chrono::milliseconds timeout);
};

SocketTimeoutOption makeSocketTimeoutOption(std::chrono::milliseconds timeout);
void setSocketTimeouts(NativeSocket socket,
                       std::chrono::milliseconds receive,
                       std::chrono::milliseconds send);
ReceiveFailure classifyReceiveFailure(int result, int nativeError) noexcept;
}
```

Clamp negative durations to zero and values above the native maximum to that maximum. On Windows encode milliseconds as `DWORD`; on POSIX encode seconds and microseconds as `timeval`. Call `setsockopt` separately for receive and send and throw if either call fails.

- [x] **Step 4: Add failing public protocol framing cases**

In `scripts/test-p0-protocol-errors.mjs`, start a real engine and use raw sockets to assert literal response objects for:

```js
assert.deepEqual(await sendLine(port, '{not-json}'), {
  error: 'invalid_json'
});
assert.deepEqual(await sendPartialJsonAndWait('127.0.0.1', port, '{"action":"ping"'), {
  error: 'request_incomplete', retryable: true
});
assert.deepEqual(await sendOversizedLegacyFrame(port, maxBytes + 1), {
  error: 'payload_too_large', max_bytes: maxBytes
});
assert.deepEqual(await sendOversizedPdb2Frame(port, maxBytes + 1), {
  error: 'payload_too_large', max_bytes: maxBytes
});
```

Also cover a valid request that triggers an execution failure and a deliberately saturated admission path; both must return one complete, stable error object. The helpers must parse one complete newline JSON or one complete `PDB2` frame and fail if EOF arrives first.

- [x] **Step 5: Run the protocol test and verify RED**

Run: `node scripts/test-p0-protocol-errors.mjs build`

Expected: the incomplete/oversized binary cases report EOF before a complete response on the old server.

- [x] **Step 6: Route server timeouts and early errors through one response path**

Replace raw `timeval` calls with `setSocketTimeouts`. Add a local `sendProtocolError` that uses the already-detected framing:

```cpp
bool sendProtocolError(pacificdb::net::NativeSocket socket,
                       bool binaryWireV2,
                       const json& error) {
    std::string payload;
    if (binaryWireV2) {
        const auto packed = json::to_msgpack(error);
        const std::uint32_t length = htonl(
            static_cast<std::uint32_t>(packed.size()) | 0x80000000U);
        payload.assign(kEngineWireV2Magic, sizeof(kEngineWireV2Magic));
        payload.append(reinterpret_cast<const char*>(&length), sizeof(length));
        payload.append(reinterpret_cast<const char*>(packed.data()), packed.size());
    } else {
        payload = error.dump() + "\n";
    }
    return sendTrackedPayload(socket, payload, 0).ok;
}
```

Classify Winsock and POSIX timeout/reset errors explicitly. If bytes are present but the request remains incomplete, return `request_incomplete`; if no bytes ever arrived and the peer is still writable, return the same error. If the peer has disconnected, log nondelivery and return without requesting shutdown.

- [x] **Step 7: Verify GREEN and inspect the slice**

Run:

```sh
cmake --build build -j2 --target db_engine_socket_runtime_test db_engine
./build/db_engine_socket_runtime_test
node scripts/test-p0-protocol-errors.mjs build
git diff --check
```

Expected: all commands exit 0; the raw clients receive complete error frames. Review the diff for payload logging and unrelated socket behavior. Do not commit.

---

### Task 2: Add lock ownership and local-engine state classification

**Files:**
- Create: `engine/include/local_engine.hpp`
- Create: `engine/src/local_engine.cpp`
- Create: `engine/test/local_engine_state_test.cpp`
- Create: `engine/test/storage_root_guard_owner_test.cpp`
- Create: `engine/test/fixtures/local_engine_states.json`
- Modify: `engine/include/storage_root_guard.hpp`
- Modify: `engine/src/storage_root_guard.cpp:150-250`
- Modify: `engine/src/main.cpp:260-305`
- Modify: `engine/src/server.cpp:2290-2430`
- Modify: `engine/src/shell.cpp:70-282, 838-890`
- Modify: `cli/src/local-engine.js`
- Modify: `cli/test/cli.test.js`
- Modify: `engine/CMakeLists.txt:108-252`

**Interfaces:**
- Produces: `EngineState`, `EngineObservations`, `classifyEngineState(const EngineObservations&)`, and one JSON decision-table fixture consumed by both the C++ and Node implementations.
- Produces: `EngineProcessMetadata {version, pid, executable, instanceId, dataRootFingerprint, discoveryNonce, port, startedAtMs}` with owner-only, atomic `readProcessMetadata` and `writeProcessMetadata`.
- Produces: `LocalEngineOptions {host, port, home, enginePath, autoStart, startupTimeout}` and `LocalEngineResult ensureLocalEngine(const LocalEngineOptions&, std::ostream&)`.
- Produces: `StorageRootOwner {pid, clusterId, nodeId, canonicalRoot, instanceId}` readable while the owner holds the lock. Preserve the existing four-argument `StorageRootGuard::acquire`; add a five-argument overload (or a defaulted final argument) for `instanceId`.
- Produces: public ping field `edition`, plus nonce-gated local-discovery fields `instance_id`, `engine_pid`, and `data_root_fingerprint`; never exposes a raw path or the nonce in a response/log.

- [x] **Step 1: Write the failing state-table test**

```cpp
using S = pacificdb::cli::EngineState;
using O = pacificdb::cli::EngineObservations;
O observations{};
observations.protocolHealthy = true;
observations.identityMatches = true;
assert(classifyEngineState(observations) == S::healthy_existing);

observations = O{};
observations.pidPresent = true;
assert(classifyEngineState(observations) == S::stale_pid);

observations = O{};
observations.pidPresent = true;
observations.pidLive = true;
observations.executableMatches = false;
assert(classifyEngineState(observations) == S::pid_reused);

observations = O{};
observations.pidPresent = true;
observations.pidLive = true;
observations.executableMatches = true;
assert(classifyEngineState(observations) == S::starting);

observations = O{};
observations.portOpen = true;
assert(classifyEngineState(observations) == S::port_conflict);

observations = O{};
observations.lockOwned = true;
observations.lockIdentityMatches = false;
assert(classifyEngineState(observations) == S::data_root_in_use);
```

Store these cases, including expected stable result/error codes and `no_start` behavior, in `engine/test/fixtures/local_engine_states.json`. The C++ test and Node test must both consume the same fixture so their classification cannot drift.

- [x] **Step 2: Build and verify RED**

Run: `cmake --build build -j2 --target db_engine_local_engine_state_test`

Expected: compilation fails because the state interface and target do not exist.

- [x] **Step 3: Implement pure classification and metadata compatibility**

Define every enum value from the spec and a total decision table. `readProcessMetadata` must tolerate a missing metadata document while still reading numeric `engine.pid`. Write metadata to a same-directory temporary file, flush it, and atomically rename it. Reject malformed version, nonpositive PID, invalid port, relative executable path, or missing instance ID as unusable metadata without deleting a live lock.

Extend the focused test to round-trip metadata through a Unicode/space path, assert the temporary file is gone after replacement, assert the resulting permissions are owner-only, and verify that malformed metadata is reported as unusable without modifying either metadata or PID files.

- [x] **Step 4: Write the failing lock-owner test**

```cpp
const auto first = StorageRootGuard::acquire(root, "cluster", "node", "2", "instance-a");
const auto owner = StorageRootGuard::readOwner(root);
assert(owner && owner->instanceId == "instance-a");
assert(owner->pid == currentProcessId());
bool rejected = false;
try { auto second = StorageRootGuard::acquire(root, "cluster", "node", "2", "instance-b"); }
catch (const std::exception&) { rejected = true; }
assert(rejected);
```

- [x] **Step 5: Implement readable owner metadata without weakening exclusion**

On Windows open the lock with `FILE_SHARE_READ` but no shared writer/delete, write owner JSON through `WriteFile`, flush it, and allow read-only opens. On POSIX continue `flock(LOCK_EX|LOCK_NB)` and replace the current key-value body with the same versioned JSON. Release remains handle/destructor-owned; no code deletes the lock to acquire it.

- [x] **Step 6: Add nonce-gated health identity and real process adapters**

Generate `PACIFICDB_INSTANCE_ID` and a cryptographically random `PACIFICDB_DISCOVERY_NONCE` before lock acquisition. Persist the nonce only in the owner-readable process metadata (`0600` on POSIX; current-user ACL on Windows), and never put it in the lock body. An ordinary `ping` remains compatible and returns `status: "pong"` plus `edition: "community"`. Only when a request supplies the exact nonce in `local_discovery_nonce` does the response add:

```json
{
  "edition": "community",
  "instance_id": "engine_<random>",
  "engine_pid": 1234,
  "data_root_fingerprint": "sha256:<hex>"
}
```

Compare nonces in constant time. A missing, malformed, or incorrect nonce returns the ordinary ping shape; it is not an authentication error and is never logged. The launcher validates `instance_id`, PID, executable, and data-root fingerprint against metadata before adopting the process. Metadata creation must not follow symlinks/reparse points and replacement must preserve owner-only permissions.

Use `QueryFullProcessImageNameW`, `proc_pidpath`, or `/proc/<pid>/exe` to compare canonical executables. A permission error is `unhealthy`/unverified, never proof that a PID matches.

- [x] **Step 7: Extract native startup into the supervisor and align the npm launcher**

Move home/path/environment/spawn logic out of `shell.cpp`. `shell.cpp` supplies the protocol probe and calls:

```cpp
const auto result = pacificdb::cli::ensureLocalEngine({
    host, numericPort, localDataHome(), executablePath(argv[0]).parent_path() / engineName,
    autoStart, configuredStartupTimeout()
}, std::cout);
```

The supervisor probes before spawn, joins the `.engine-starting` owner, validates PID/executable/instance metadata, retains the spawned process handle while waiting, and reports early exit code plus `engine.log`. It does not kill a live process on deadline.

Refactor `cli/src/local-engine.js` around the same observation names and the shared JSON state table. Keep its Node process adapters native to Node, but apply identical state precedence, nonce-gated health validation, 120-second monotonic deadline, early-exit reporting, and `--no-start` read-only behavior. Do not make the npm launcher shell out to the native CLI.

- [x] **Step 8: Verify GREEN and inspect the slice**

Run:

```sh
cmake --build build -j2 --target db_engine_local_engine_state_test db_engine_storage_root_guard_owner_test pacificdb db_engine
./build/db_engine_local_engine_state_test
./build/db_engine_storage_root_guard_owner_test
npm test --workspace @pacificdb/cli
scripts/test-community-autostart.sh build
git diff --check
```

Expected: all exit 0; existing numeric PID assertions still pass. Do not commit.

---

### Task 3: Make engine lifecycle explicit and observable

**Files:**
- Create: `engine/include/structured_event.hpp`
- Create: `engine/src/structured_event.cpp`
- Create: `engine/test/structured_event_test.cpp`
- Create: `scripts/test-p0-client-lifecycle.mjs`
- Modify: `engine/src/main.cpp:220-679`
- Modify: `engine/src/server.cpp:110-410, 1940-2260, 6319-7179`
- Modify: `engine/src/local_engine.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- Produces: `StructuredEvent {severity, subsystem, code, operationId, message, fields}`.
- Produces: `std::string formatStructuredEvent(const StructuredEvent&)` and `void emitStructuredEvent(std::ostream&, const StructuredEvent&)`.
- Produces: lifecycle codes `engine_starting`, `engine_ready`, `engine_shutdown_requested`, `engine_shutdown_complete`, `engine_fatal`, `client_connected`, `client_disconnected`, and `response_delivery_failed`.
- Consumes: local-engine `instanceId`, request/trace IDs, existing shutdown functions, durability drain, and `engine.log` redirection.

- [x] **Step 1: Write a failing redaction/shape test**

```cpp
StructuredEvent event{"error", "media", "media_chunk_failed", "media_1",
                      "chunk rejected", {{"token", "secret"}, {"index", 2}}};
const auto line = formatStructuredEvent(event);
const auto value = nlohmann::json::parse(line);
assert(value.at("severity") == "error");
assert(value.at("subsystem") == "media");
assert(value.at("code") == "media_chunk_failed");
assert(value.at("operation_id") == "media_1");
assert(value.at("fields").at("token") == "[redacted]");
assert(line.find("secret") == std::string::npos);
```

- [x] **Step 2: Build and verify RED**

Run: `cmake --build build -j2 --target db_engine_structured_event_test`

Expected: compilation fails because the event helper is absent.

- [x] **Step 3: Implement the structured event helper**

Emit one compact JSON object per line with UTC epoch milliseconds. Redact keys matching `password`, `token`, `key`, `authorization`, `data`, `binaryData`, or `payload` case-insensitively. Accept only scalar safe context fields; replace arrays/objects with `"[omitted]"`.

- [x] **Step 4: Add the baseline client-lifecycle harness and run it before lifecycle edits**

The script must start a real engine, create a database/collection, then repeat five cycles of 16 concurrent clients. Each cycle writes deterministic IDs, destroys half the sockets normally and half abruptly, verifies the original PID is alive, pings the original port, reconnects, and asserts exact acknowledged IDs.

Run: `PACIFICDB_P0_WRITES=1000 node scripts/test-p0-client-lifecycle.mjs build`

Expected: record whether beta.12 reproduces engine exit. If it exits, retain the exit status, last 200 log lines, last acknowledged IDs, and first failing subsystem. If it remains healthy, keep the test as a regression and do not invent a client-reference-count fix: source inspection already shows disconnect paths do not call `requestServerShutdown`.

- [x] **Step 5: Add explicit lifecycle logging and a fatal boundary**

At startup, install `std::set_terminate` and the supported signal/control handlers. Wrap the engine top-level body so uncaught standard exceptions produce `engine_fatal` with `what()`, phase, instance ID, and exit code; unknown exceptions use a fixed safe message. On Windows use `SetConsoleCtrlHandler`; on POSIX retain `SIGINT`/`SIGTERM` and ignored `SIGPIPE`.

Normal client EOF/reset logs `client_disconnected` and closes only that socket. Server shutdown is requested only by the explicit process handlers. Log each ordered shutdown phase and write `engine_shutdown_complete` only after the clean-shutdown marker succeeds.

- [x] **Step 6: Make child logging and detachment cross-platform**

Before spawn, open `engine.log` append-only with owner permissions. On Windows pass only the intended standard handles through `STARTUPINFOEXW` plus `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, set `STARTF_USESTDHANDLES`, and use `CreateProcessW` with `CREATE_NO_WINDOW | DETACHED_PROCESS`. If the parent is in a job, first attempt `CREATE_BREAKAWAY_FROM_JOB`; retry without it only for the documented access-denied/unsupported result, and retain the child process handle through readiness or early exit. On POSIX retain `setsid`, `dup2`, and `exec`. Close launcher copies after successful spawn; the engine owns its inherited descriptors.

- [x] **Step 7: Verify lifecycle behavior and existing recovery**

Run:

```sh
cmake --build build -j2 --target db_engine_structured_event_test pacificdb db_engine
./build/db_engine_structured_event_test
PACIFICDB_P0_WRITES=1000 node scripts/test-p0-client-lifecycle.mjs build
node scripts/test-community-restart-matrix.mjs build
git diff --check
```

Expected: the PID remains stable through all disconnect cycles, restart recovery passes, and log lines identify startup/shutdown. Do not commit.

---

### Task 4: Implement durable media states, progress, leases, and reconciliation

**Files:**
- Create: `engine/include/media_upload_state.hpp`
- Create: `engine/src/media_upload_state.cpp`
- Create: `engine/test/media_upload_state_test.cpp`
- Modify: `engine/include/community_catalog.hpp:25-58`
- Modify: `engine/src/community_catalog.cpp:18-398`
- Modify: `engine/test/community_catalog_test.cpp`
- Modify: `engine/src/server.cpp:2410-2525`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Produces: `MediaState parseMediaState(std::string_view)` and `std::string_view mediaStateName(MediaState)`.
- Produces: `bool canTransition(MediaState from, MediaState to)`.
- Produces: `MediaProgress reconstructMediaProgress(const json& manifest, const std::vector<json>& chunks, long long nowMs)` returning `receivedChunks`, `receivedBytes`, `nextMissingIndex`, and `leaseExpiresAtMs`.
- Produces: `bool leaseExpired(const json& manifest, long long nowMs)`.
- Extends: `CommunityCatalog::reconcileMedia(userId, optionalMediaId)` and `cleanupMedia(userId, mediaId)`.
- Extends public media responses with `state_version`, `received_chunks`, `received_bytes`, `next_chunk`, `lease_expires_at_ms`, and `resumable`.

- [x] **Step 1: Write the failing pure state tests**

```cpp
assert(canTransition(MediaState::uploading, MediaState::verifying));
assert(canTransition(MediaState::uploading, MediaState::aborted));
assert(canTransition(MediaState::verifying, MediaState::ready));
assert(canTransition(MediaState::verifying, MediaState::failed));
assert(!canTransition(MediaState::ready, MediaState::aborted));

json manifest{{"chunk_count", 3}, {"size_bytes", 9},
              {"status", "uploading"}, {"lease_expires_at_ms", 5000}};
std::vector<json> chunks = {
    {{"index", 0}, {"size_bytes", 4}},
    {{"index", 2}, {"size_bytes", 1}}
};
const auto progress = reconstructMediaProgress(manifest, chunks, 1000);
assert(progress.receivedChunks == 2);
assert(progress.receivedBytes == 5);
assert(progress.nextMissingIndex == 1);
assert(!leaseExpired(manifest, 4999));
assert(leaseExpired(manifest, 5000));
```

- [x] **Step 2: Build and verify RED**

Run: `cmake --build build -j2 --target db_engine_media_upload_state_test`

Expected: compilation fails because the helper and target are absent.

- [x] **Step 3: Implement the pure state and progress rules**

Reject unknown states, negative or duplicate chunk indices with conflicting metadata, progress beyond manifest limits, and nonpositive lease durations. Sort durable indices, sum sizes with overflow checks, and choose the lowest missing index. Treat legacy `uploading`/`ready` manifests as state version 1 and new manifests as version 2.

- [x] **Step 4: Expand the catalog test with failing state-machine assertions**

Add assertions that:

```cpp
assert(upload.at("status") == "uploading");
assert(upload.at("received_chunks") == 0);
assert(catalog.putMediaChunk(/* index 0 */).at("next_chunk") == 1);
assert(catalog.beginMedia(/* same immutable fields, resumeId */).at("id") == upload.at("id"));
assert(catalog.finalizeMedia(/* missing chunk */).at("resumable") == true);
assert(catalog.cleanupMedia("system", upload.at("id")) == 1);
assert(catalog.cleanupMedia("system", upload.at("id")) == 0);
assert(catalog.cleanupMedia("system", readyId) == 0);
```

Insert a chunk directly without updating its manifest, call `reconcileMedia`, and assert progress is rebuilt. Insert an orphan chunk, reconcile, and assert it is removed. Configure a one-hour lease and assert broad cleanup preserves a fresh upload but removes an expired one.

- [x] **Step 5: Run catalog tests and verify RED**

Run: `cmake --build build -j2 --target db_engine_community_catalog_test && ./build/db_engine_community_catalog_test`

Expected: assertions fail because progress, legal transitions, leases, and safe broad cleanup are absent.

- [x] **Step 6: Implement catalog serialization and transitions**

Serialize mutations per media ID with 64 striped mutexes. Begin writes a version-2 `uploading` manifest with a 24-hour default lease from `PACIFICDB_MEDIA_UPLOAD_LEASE_MS`. Chunk writes validate/decode/hash before mutation, persist deterministic chunk content, then recompute and persist progress. Identical existing chunks return success; conflicts transition to `failed`, record `media_chunk_conflict`, and clean only owned chunks.

Finalize returns structured resumable progress when chunks are missing, transitions to `verifying`, streams checksum/size validation, and writes `ready` only after all storage calls return successfully. A checksum/size mismatch transitions to `failed` and cleans chunks without erasing the failure record.

Explicit cleanup transitions a non-ready manifest to `aborted`, deletes its chunks, and returns success once; subsequent calls return `{status:"ok", already_clean:true}` at the server boundary. Broad cleanup processes only expired non-ready manifests plus proven orphan chunks. Reconciliation returns counts for repaired progress, reset verifying states, removed orphans, and expired uploads.

- [x] **Step 7: Add structured server responses and events**

`community_media_begin`, `community_media_put_chunk`, `community_media_finalize`, and `community_media_cleanup` return the catalog's full stable status. Emit event codes without including `data` or SHA-256 values. An application validation failure is a complete JSON error response, not a socket close.

- [x] **Step 8: Verify GREEN and inspect the slice**

Run:

```sh
cmake --build build -j2 --target db_engine_media_upload_state_test db_engine_community_catalog_test db_engine
./build/db_engine_media_upload_state_test
./build/db_engine_community_catalog_test
git diff --check
```

Expected: all exit 0 and existing project/catalog assertions remain green. Do not commit.

---

### Task 5: Surface resumable upload identity in Node and native shells

**Files:**
- Modify: `sdk/node/src/index.js:1-220`
- Modify: `sdk/node/test/client.test.js:1-190`
- Modify: `cli/src/shell.js:410-435`
- Modify: `cli/test/cli.test.js`
- Modify: `engine/src/shell.cpp:680-835, 1040-1090`
- Modify: `engine/test/native_shell_parser_test.cpp`
- Modify: `sdk/node/README.md`
- Modify: `cli/README.md`

**Interfaces:**
- Produces: exported `MediaUploadError extends Error` with `code`, `uploadId`, `nextChunk`, `receivedChunks`, `receivedBytes`, `resumable`, and optional `cause`.
- Extends: `PacificDBClient.uploadMediaFile` to consume begin/resume progress and retransmit only missing/uncertain chunks.
- Produces: native `MediaUploadInterrupted` carrying an equivalent public JSON object.
- Shell output for interrupted media is one JSON object with `status:"resumable"`, `error:"media_upload_interrupted"`, and the stable upload ID.

- [x] **Step 1: Write failing Node SDK interruption/resume tests**

Use a real local TCP fixture that acknowledges begin with `media_resume`, accepts chunk 0, then destroys the socket before responding to chunk 1. Assert:

```js
await assert.rejects(
  client.uploadMediaFile('videos', filename),
  (error) => error instanceof MediaUploadError &&
    error.code === 'media_upload_interrupted' &&
    error.uploadId === 'media_resume' &&
    error.nextChunk === 1 && error.resumable === true
);
```

On the next call with `{resume:'media_resume'}`, make begin report received indices `[0,1]`; assert only remaining indices are transmitted and the original ID becomes `ready`.

- [x] **Step 2: Run Node tests and verify RED**

Run: `npm test --workspace @pacificdb/client`

Expected: the current generic connection error has no upload ID or progress, and resume retransmits every chunk.

- [x] **Step 3: Implement `MediaUploadError` and progress-aware resume**

Track `media.id`, acknowledged indices, bytes, and next index after begin. Wrap only failures after an ID is known:

```js
throw new MediaUploadError('media upload interrupted', {
  code: 'media_upload_interrupted', uploadId: media.id,
  nextChunk, receivedChunks, receivedBytes, resumable: true,
  cause: error
});
```

Treat server `received_indices` as authoritative durable progress. Retransmit an uncertain last chunk because chunk IDs are idempotent. Never start a new begin request implicitly after a known-ID failure.

- [x] **Step 4: Add failing npm-shell output test**

Drive `runShell` with a client whose `uploadMediaFile` throws `MediaUploadError` and assert the output parses as:

```json
{"status":"resumable","error":"media_upload_interrupted","upload_id":"media_resume","next_chunk":1}
```

Run: `npm test --workspace @pacificdb/cli`

Expected: old output is only an unstructured error line.

- [x] **Step 5: Implement npm and native shell reporting**

Catch `MediaUploadError` only around the media command and pass its public fields through `printResponse`. In the native upload loop, track the begin manifest and last acknowledgement; translate connection exceptions after begin into `MediaUploadInterrupted`. Keep validation errors before begin as normal failures.

- [x] **Step 6: Verify GREEN and compatibility**

Run:

```sh
npm test --workspace @pacificdb/client
npm test --workspace @pacificdb/cli
cmake --build build -j2 --target db_engine_native_shell_parser_test pacificdb
./build/db_engine_native_shell_parser_test
git diff --check
```

Expected: all exit 0; existing `putMedia/getMedia` and command parsing remain compatible. Do not commit.

---

### Task 6: Add deterministic lifecycle, discovery, and durability regressions

**Files:**
- Create: `scripts/test-p0-discovery.mjs`
- Complete: `scripts/test-p0-client-lifecycle.mjs`
- Create: `scripts/test-p0-load.mjs`
- Modify: `scripts/test-community-autostart.sh`
- Modify: `scripts/test-community-restart-matrix.mjs`
- Modify: `scripts/test-community.sh`
- Modify: `engine/src/test_failpoint.cpp`
- Modify: `engine/include/test_failpoint.hpp`

**Interfaces:**
- Consumes: local supervisor result codes, engine metadata, structured log events, `PacificDBClient`, and test-only failpoints.
- Produces: `PACIFICDB_TEST_STARTUP_DELAY_MS` only when failpoints are compiled and `PACIFICDB_TEST_MODE=local_engine_startup`; production builds ignore it.
- Produces: environment controls `PACIFICDB_P0_WRITES` (default 10000), `PACIFICDB_P0_CLIENTS` (default 8), `PACIFICDB_P0_CYCLES` (default 5), and `PACIFICDB_P0_SOAK` (enables 100000/16 mixed workload).

- [x] **Step 1: Add a guarded startup-delay failpoint test mode**

Extend the existing failpoint allowlist with `local_engine_startup`. At `FP_SHUTDOWN_BEFORE_LISTENER_BIND`, support a `delay` action that parses `PACIFICDB_TEST_STARTUP_DELAY_MS` between 1 and 180000 and sleeps only when the existing confirmation phrase is present. Invalid/missing values exit 87. Add a focused assertion that production/nonconfirmed invocation never delays.

- [x] **Step 2: Write the discovery matrix before changing remaining discovery behavior**

Use isolated fake processes and a real test engine to assert exact result codes for healthy existing engine, stale PID, reused unrelated PID, delayed startup, early exit, live unhealthy process, unrelated port, concurrent eight-launcher race, and valid lock held by another instance. Each scenario records process count before/after and asserts `--no-start` makes no process or metadata changes.

- [x] **Step 3: Run the discovery matrix and verify RED**

Run: `node scripts/test-p0-discovery.mjs build`

Expected: old behavior lacks result codes/metadata, cannot identify reused PIDs, and reports delayed startup as generic failure.

- [x] **Step 4: Make the smallest supervisor corrections exposed by the matrix**

Correct state precedence and metadata races only where the failing scenarios demonstrate a defect. Concurrent callers must share the startup directory lock, re-probe protocol while waiting, validate the eventual instance, and leave exactly one live engine. Remove stale PID/metadata only after proving the PID dead; never delete root locks.

- [x] **Step 5: Complete the lifecycle and load harnesses**

`test-p0-client-lifecycle.mjs` performs five 16-client disconnect cycles and abrupt socket termination. `test-p0-load.mjs` writes deterministic IDs, records only acknowledged operations, checks exact IDs rather than count alone, disconnects all clients, verifies the same PID, restarts, and verifies exact IDs again.

Default workload: 10,000 inserts through 8 clients. Soak workload: 100,000 inserts through 16 clients, one read per 100 writes, and one update per 250 writes. Record elapsed time, RSS, handles/descriptors, threads, and storage bytes without performance assertions.

- [x] **Step 6: Run RED/GREEN lifecycle and durability evidence**

Run:

```sh
node scripts/test-p0-discovery.mjs build
PACIFICDB_P0_WRITES=10000 PACIFICDB_P0_CLIENTS=8 node scripts/test-p0-client-lifecycle.mjs build
PACIFICDB_P0_WRITES=10000 PACIFICDB_P0_CLIENTS=8 node scripts/test-p0-load.mjs build
node scripts/test-community-restart-matrix.mjs build
```

Expected: all exit 0; PID is unchanged during disconnects and exact acknowledged records survive restart. If the original post-load engine exit reproduces, first add a focused regression for the logged failing subsystem and fix that mechanism before accepting this step. If it does not reproduce after 20 repeated cycles, record it as externally observed and covered by the new regression without speculative production changes. Do not commit.

---

### Task 7: Add media boundary, interruption, cleanup, and crash recovery regressions

**Files:**
- Create: `scripts/test-p0-media.mjs`
- Create: `scripts/test-p0-media-crash.mjs`
- Modify: `scripts/test-community-e2e.mjs`
- Modify: `scripts/test-community-restart-matrix.mjs`
- Modify: `scripts/test-community-disk-full.mjs`
- Modify: `scripts/test-community.sh`
- Modify: `engine/src/test_failpoint.cpp`
- Modify: `engine/include/test_failpoint.hpp`

**Interfaces:**
- Consumes: media begin/chunk/finalize/resume/cleanup responses and `MediaUploadError`.
- Produces: test-only media failpoint names `FP_MEDIA_AFTER_MANIFEST`, `FP_MEDIA_AFTER_CHUNK`, `FP_MEDIA_VERIFYING`, and `FP_MEDIA_BEFORE_READY`, guarded by `PACIFICDB_TEST_MODE=media_upload_recovery` and the existing confirmation phrase.
- Produces: `PACIFICDB_P0_MEDIA_100MB=1` to include the 100 MiB case when resources allow; release gates set it to 1.

- [x] **Step 1: Add guarded media crash points**

Place failpoints immediately after durable manifest creation, after durable chunk insertion but before progress update, after transition to `verifying`, and after successful verification but before `ready`. Production builds compile them to no-ops.

- [x] **Step 2: Write the boundary/filename matrix and verify RED**

Generate deterministic byte buffers at 0, 1, `chunk-1`, `chunk`, `chunk+1`, `2*chunk-1`, `2*chunk`, `2*chunk+1`, 1 MiB, 4 MiB, 10 MiB, and conditionally 100 MiB. Use filenames:

```js
['space name.png', 'తెలుగు-video.mp4', 'clip (final).mp4',
 `${'a'.repeat(96)}.bin`, 'archive.part.001.media.bin']
```

Assert zero bytes returns `media_file_empty` without a manifest. For accepted inputs, upload, list, download, compare exact bytes and SHA-256, restart, and repeat the comparison. On unmodified Windows beta.12, preserve evidence that the 4 MiB request can end before a response.

- [x] **Step 3: Write crash/resume and cleanup cases and verify RED**

Cover transport interruption before the begin response (the client has no trustworthy ID and reports a retryable begin failure), after begin, both before and after a chunk acknowledgement, and during finalize. For every engine failpoint, start an isolated engine, advance to that point, force exit, restart without deleting the root, inspect reconciliation results, resume the same ID when one was acknowledged, and verify one `ready` manifest and no orphan chunks. Assert fresh broad cleanup preserves uploads, expired cleanup removes only incomplete data, explicit cleanup is idempotent, and both cleanup routes refuse ready media.

- [x] **Step 4: Correct only state/recovery defects exposed by the tests**

Fix transition ordering, progress reconstruction, or cleanup selection in `CommunityCatalog` without weakening checksums or erasing failure records. A disk-full or permission error before durable chunk acknowledgement must return a structured resumable/failure response and must not advance progress.

- [x] **Step 5: Run GREEN media evidence**

Run:

```sh
node scripts/test-p0-media.mjs build
node scripts/test-p0-media-crash.mjs build
node scripts/test-community-disk-full.mjs build
node scripts/test-community-e2e.mjs build
node scripts/test-community-restart-matrix.mjs build
```

Expected: all exit 0; every accepted byte count round-trips; crash scenarios reuse one ID; no ready media is cleaned. Do not commit.

---

### Task 8: Cross-platform package gates and real Windows verification

**Files:**
- Create: `scripts/test-p0-installed.mjs`
- Create: `scripts/test-p0-windows.ps1`
- Modify: `.github/workflows/release.yml`
- Modify: `scripts/test-native-package.sh`
- Modify: `README.md`
- Modify: `cli/README.md`
- Modify: `docs/COMMUNITY_P0_CERTIFICATION.md`

**Interfaces:**
- Consumes: installed `pacificdb`/`db_engine`, P0 scripts, platform package paths, and SSH target `hites@10.0.0.9`.
- Produces: machine-readable P0 evidence JSON containing platform, architecture, edition/version, executable path, isolated root, test counts, exit codes, resource observations, and redacted log checks.
- Produces: release workflow artifacts only; publication remains gated on P1/P2.

- [x] **Step 1: Write the installed-package runner**

Resolve binaries from an explicit package root rather than ambient PATH. Create a Unicode/space test root, choose free engine/Raft ports, run version, startup/discovery, protocol, lifecycle 10,000/8, media through 10 MiB, restart, and log-redaction checks. The release gate sets `PACIFICDB_P0_MEDIA_100MB=1` and runs the soak separately.

- [x] **Step 2: Add platform workflow gates**

After installing each generated package in the existing Windows, macOS, and Debian jobs, invoke the installed-package runner with the actual install root. Windows must run the PowerShell wrapper, macOS both architecture jobs, and Debian the installed `/usr/bin` binaries. Upload redacted evidence JSON and engine logs on failure.

- [ ] **Step 3: Run all local checks before remote mutation**

Run:

```sh
npm install --ignore-scripts --no-audit --no-fund
scripts/test-community.sh build
PACIFICDB_P0_WRITES=10000 PACIFICDB_P0_CLIENTS=8 node scripts/test-p0-load.mjs build
PACIFICDB_P0_SOAK=1 node scripts/test-p0-load.mjs build
PACIFICDB_P0_MEDIA_100MB=1 node scripts/test-p0-media.mjs build
git diff --check
git status --short
```

Expected: all exit 0. Inspect status and preserve every unrelated user change. Do not commit.

- [ ] **Step 4: Build package artifacts and verify CI results**

Trigger the non-publishing release workflow for the candidate version, wait for Windows, both macOS architectures, and Debian to finish, and download their exact artifacts plus evidence. A failed platform job blocks remote installation and all release work.

- [ ] **Step 5: Baseline and test the supplied Windows host safely**

Connect with `ssh hites@10.0.0.9`. Before copying or installing anything, record `Get-Command pacificdb -All`, running `db_engine` processes, current installation paths, and existing PacificDB data paths. Do not stop or overwrite an existing engine. Copy the candidate installer and test scripts to a new temporary directory, install to a separate candidate root, and use a unique `PACIFICDB_HOME` plus dynamic ports.

Run the Windows installed-package suite, 10,000/8 lifecycle gate, 100,000/16 soak, 100 MiB media round trip, abrupt restart/recovery, and PATH-independent explicit executable checks. Capture exit codes and redacted evidence. Remove only the temporary candidate root and isolated test data after successful evidence capture; preserve them on failure for diagnosis and report their exact location.

- [ ] **Step 6: Run final P0 verification and document evidence**

Update `docs/COMMUNITY_P0_CERTIFICATION.md` with commands, platforms, counts, results, and remaining limitations. Run the full local suite again after documentation changes. Review all logs for forbidden secret/payload fields and all diffs for unrelated changes.

Expected: P0 acceptance criteria are directly evidenced on Linux locally, macOS/Windows CI, and the supplied real Windows host. Do not commit or publish; proceed to the separately reviewed P1 design and plan.

---

## P0 Completion Matrix

Before moving to P1, record one status per row as Confirmed, Partial, Unverified, or Failed:

| Requirement | Changed seam | Focused evidence | Broad evidence |
|---|---|---|---|
| Client exit never stops engine | server lifecycle | client-lifecycle cycles | 10k/100k load and restart |
| Startup discovery is unambiguous | local supervisor | state and lock tests | discovery matrix and packages |
| `--no-start` is read-only | supervisor | state matrix | installed-package matrix |
| Handled errors are fully framed | socket/server | protocol error script | media and malformed E2E |
| Media is resumable and leak-free | catalog/SDK/shells | state/catalog tests | boundary/crash/restart matrix |
| Acknowledged writes recover | WAL/engine lifecycle | restart matrix | load crash/restart |
| Logs are actionable and safe | structured events | redaction test | package/remote log audit |
| Windows/macOS/Linux packages work | package workflows | installed smoke | platform P0 suites |

Any Failed row blocks P1 execution. Any Partial or Unverified row must be explicitly accepted by the user before moving forward; it cannot be silently treated as passing.
