# PacificDB P0 Lifecycle and Media Reliability Design

**Date:** 2026-09-13
**Status:** Approved by user; implementation planning complete
**Target:** PacificDB Community after `0.1.0-beta.12`

## Purpose

Make the local PacificDB engine and chunked media pipeline reliable across
Windows, macOS, and Linux. Client exit, disconnect, timeout, or termination must
not stop the engine. Every handled request must produce one complete protocol
response. Media uploads must end in a durable ready, resumable, failed, or
aborted state without leaking unowned chunks.

This is the first of three ordered delivery stages:

1. P0 engine lifecycle, discovery, protocol, and media reliability.
2. P1 vector schemas, dotted paths, and backup integrity.
3. P2 resource-create contracts, CLI diagnostics, and installer/PATH UX.

The final release is permitted only after all three stages and their
cross-platform acceptance checks pass.

## Established Evidence and Working Root Causes

### Windows socket timeouts

The server currently supplies a POSIX `timeval` to `SO_RCVTIMEO` and
`SO_SNDTIMEO` on every platform. Winsock expects a `DWORD` millisecond value.
This makes the timeout invalid or far shorter than intended and is the primary
working root cause for why large media requests can be truncated intermittently
while smaller requests sometimes pass. The Windows regression must reproduce
the old behavior before this cause is considered fully validated.

### Incomplete error framing

Some receive and frame-limit paths close a socket without returning a complete
JSON or binary-wire error. An empty receive timeout and an oversized binary
frame can therefore appear at the client as only "closed before response."

### Ambiguous local-engine startup

The native launcher polls for a fixed 30 seconds, immediately closes the
Windows child-process handle, and does not redirect Windows child output. It
cannot distinguish slow startup, early exit, or a live unhealthy process. On
the observed Windows system it reported failure while the spawned process later
became healthy and continued holding the real DATA_ROOT lock.

### Incomplete media lifecycle

Media manifests currently distinguish only `uploading` and `ready`. They do not
persist acknowledged progress or a lease, broad cleanup removes all incomplete
uploads regardless of age, and startup does not reconcile manifests with
chunks. A transport error after `begin` leaves an upload whose ID is not
surfaced by the CLI, and a retry commonly creates another manifest.

## Scope

P0 includes:

- engine lifetime independent of normal clients;
- graceful shutdown and crash-safe restart verification;
- PID, process, port, protocol, and DATA_ROOT lock discovery;
- race-safe concurrent local startup;
- complete engine logging during startup and runtime;
- platform-correct socket timeouts and complete error framing;
- a persistent, resumable media state machine;
- stale-upload and orphan-chunk reconciliation;
- lifecycle, malformed-protocol, media-boundary, crash, and load regressions;
- installed-package smoke tests on Windows, macOS, and Linux;
- real Windows verification over `ssh hites@10.0.0.9` after isolated and CI
  checks pass.

P0 excludes vector schema changes, dotted updates, backup contracts,
resource-create semantics, the final `doctor` command, and final PATH naming
policy. Those remain P1 or P2. P0 may add internal diagnostics consumed by P2.

## Compatibility and Safety Invariants

- Existing numeric `engine.pid` files remain compatible.
- Existing shell commands and newline-JSON clients remain supported.
- `--no-start` never spawns, stops, signals, or adopts a process.
- No path deletes a lock owned by a live process.
- Connection close, reset, timeout, or abrupt client termination never requests
  server shutdown.
- Every acknowledged write survives graceful restart and crash recovery.
- Media is not `READY` until its bytes and whole-file checksum are durable and
  verified.
- Cleanup never deletes `READY` media.
- Logs exclude passwords, tokens, API-key secrets, and payload contents.
- Tests and remote verification use isolated roots and preserve existing data.

## Architecture

### Local engine supervisor

Extract the native launch/discovery logic from `shell.cpp` into a focused,
testable supervisor and keep equivalent state semantics in the npm CLI. It:

1. probes the configured host and port with the PacificDB protocol;
2. reads the compatible numeric PID and richer process metadata;
3. validates liveness and executable identity;
4. matches protocol instance identity to process and storage metadata;
5. classifies the port as unused, PacificDB-owned, or unrelated;
6. joins an in-progress startup or spawns exactly one engine under the existing
   inter-process startup lock.

Observable states are `healthy_existing`, `starting`, `spawned`, `exited`,
`unhealthy`, `stale_pid`, `pid_reused`, `port_conflict`, and
`data_root_in_use`. Every state has a stable public error/result code.

Use a configurable monotonic startup deadline, default 120 seconds, while also
monitoring process exit. The longer deadline is not the root fix: validated
identity, progress, exit detection, and logs make the result unambiguous. A
still-live engine is not killed merely because the deadline expires.

Keep `engine.pid` numeric. Atomically write an owner-readable process metadata
document containing PID, executable identity, instance ID, DATA_ROOT
fingerprint, a cryptographically random discovery nonce, engine port, start
time, and format version. Ordinary unauthenticated ping exposes only a safe
edition marker. PID, instance, and DATA_ROOT identity are returned only when a
local discovery request proves possession of that nonce. The nonce and raw
paths are never returned or logged.

### Platform boundary

Behavior is shared; only OS adapters vary:

| Concern | Windows | macOS | Linux |
|---|---|---|---|
| Process identity | `OpenProcess`, `QueryFullProcessImageNameW` | `proc_pidpath` | `/proc/<pid>/exe` |
| Spawn isolation | detached `CreateProcessW` | `fork`, `setsid`, `exec` | `fork`, `setsid`, `exec` |
| Socket timeouts | `DWORD` milliseconds | `timeval` | `timeval` |
| Root lock | `CreateFileW`, exclusive writer, readable owner metadata | `flock` | `flock` |
| Paths | wide-character Win32 APIs | native filesystem paths | native filesystem paths |
| Logs | inherited append handles | duplicated append descriptors | duplicated append descriptors |

Timeouts use `std::chrono` internally and convert only in the adapter. Every
socket-option call is checked. Platform tests include spaces and Unicode paths.

### Engine lifecycle

The engine owns its lifetime. Launchers and clients hold no lifetime reference,
and the engine does not depend on their handles, pipes, or consoles. It stops
only on a supported explicit termination event or fatal engine failure.

Graceful termination stops admission, interrupts tracked sockets, drains work,
flushes durability state, stops Raft and storage workers, writes the clean
marker only after a successful drain, and releases the root lock last. Fatal
paths log phase, safe error context, and exit reason before returning nonzero.
The supervisor captures all engine output in `engine.log` on every platform.

### Protocol contract

Socket timeouts measure an idle interval without byte progress, not a total
request deadline. For every request that can still be answered, emit exactly one
response in its original framing:

- malformed JSON: `invalid_json`;
- incomplete request timeout: `request_incomplete`;
- request/frame limit: `payload_too_large` with the maximum;
- execution failure: stable code and safe message;
- admission failure: structured retry metadata.

If the peer already disconnected, delivery is impossible; log the connection
and request IDs without changing engine lifetime. Retry partial sends for
interrupt and bounded writable-wait conditions, then record a delivery failure.

## Media State and Data Model

Persist these states:

```text
UPLOADING --all chunks--> VERIFYING --valid size/hash--> READY
    |                         |
    +--explicit cleanup-----> ABORTED
    +--terminal corruption--> FAILED
```

The diagram uses uppercase conceptual names. Persisted and public JSON values
remain lowercase (`uploading`, `verifying`, `ready`, `failed`, `aborted`) for
compatibility with the existing Community API.

`CREATED` is not separate because a durable zero-progress manifest is already
`UPLOADING`. Each manifest contains a stable ID; database and collection; safe
basename and content type; expected size, chunk count, and SHA-256; state and
state version; created, updated, and lease timestamps; durable chunk/byte
progress; and a safe last error code.

The default lease is 24 hours and configurable. Each acknowledged chunk renews
it. Leases never authorize deletion of `READY` media.

### Begin and resume

Begin creates one `UPLOADING` manifest. Resume validates immutable metadata and
returns the same ID, durable received chunks/bytes, next missing index, and
lease. A mismatch rejects the request without modifying the existing upload.

### Chunk write

Validate index, decoded size, per-chunk checksum, and limits before persistence.
Persist deterministic `(media_id, index)` content first, then update progress.
Identical repeats are idempotent. A crash between chunk and progress updates is
repaired by reconciliation. Invalid uncommitted content is rejected while the
upload remains resumable. A conflicting durable chunk is terminal corruption:
transition to `FAILED`, clean owned chunks recoverably, and retain safe failure
metadata.

### Finalize

Confirm all chunks, transition to `VERIFYING`, and stream chunks in index order
through size and whole-file hash verification. Publish `READY` only after
verification and durability complete. Missing chunks return resumable progress.
A stored-data mismatch transitions to `FAILED` and invokes recoverable cleanup.

### Client interruption

After begin returns, the Node SDK and both shells retain the upload ID. Any
ambiguous transport failure reports a structured resumable error with that ID
and last acknowledged progress. `--resume <id>` reuses the upload and
idempotently retransmits uncertain chunks; it does not create a new manifest.

### Cleanup and reconciliation

Explicit cleanup of non-ready media transitions it to `ABORTED`, removes owned
chunks, and is idempotent. Cleanup of `READY` is refused; explicit media deletion
remains the ready-object deletion path.

Startup and requested cleanup reconcile chunk/progress disagreement, orphan
chunks, interrupted `VERIFYING` states, and expired incomplete records. Broad
cleanup removes only expired incomplete records. Orphan chunks are removed only
after proving no manifest owns them. Recovery is bounded and restart-safe.

Zero-byte media is rejected before manifest creation. Engine metadata stores
only safe basenames; local source/destination paths are never engine paths.

## Observability

Use a common structured event helper. Records include timestamp, severity,
subsystem, event/error code, operation/request ID, and safe context. Cover:

- discovery, spawn, readiness, deadline, process exit, lock acquire/release;
- explicit and fatal shutdown reason and phase;
- client connect/disconnect/reset and response delivery failure;
- media begin, chunk acknowledgement, resume, verify, ready, fail, abort,
  cleanup, and reconciliation.

Credentials, payload bodies, and unnecessary hashes are excluded.

## Verification Design

### Focused checks

- timeout conversion and socket-option failures;
- process metadata serialization and atomic replacement;
- dead/live/reused PID and executable identity decisions;
- protocol instance matching;
- media transition legality, progress reconstruction, and lease decisions;
- exact JSON and binary-wire error framing;
- safe basenames with spaces, Unicode, parentheses, long names, and dots.

### Lifecycle and discovery matrices

For several cycles, start one engine; connect 16 clients; insert concurrently;
disconnect them; verify the same PID and health; reconnect; and validate every
acknowledged record. Repeat with abrupt clients and partial requests.

Test `--no-start` and normal discovery against healthy, absent, starting,
unhealthy, stale-PID, reused-PID, early-exit, deadline, unrelated-port, and
valid-root-lock states. No test passes by deleting a valid lock.

### Media matrix

Exercise 0, 1, `chunk_size - 1`, `chunk_size`, `chunk_size + 1`,
`2 * chunk_size - 1`, `2 * chunk_size`, `2 * chunk_size + 1`, 1 MiB, 4 MiB,
10 MiB, and 100 MiB inputs. Zero bytes must produce the structured rejection;
accepted sizes must round-trip exactly.

Run image, video, and generic content with every filename category. Interrupt
before begin response, after begin, around chunk acknowledgement, and during
finalize. Kill the engine at several offsets, restart, resume, and verify exact
bytes/checksum. Cleanup must be idempotent and preserve ready media.

### Durability and load gates

- 10,000 inserts through 8 clients on Windows, macOS, and Linux;
- disconnect all clients, verify engine health, reconnect, and verify all
  acknowledged writes;
- restart and verify exact IDs and count;
- 100,000 inserts through 16 clients with reads every 100 writes and updates
  every 250 writes as an extended release gate;
- report memory, handles/descriptors, threads, and storage growth without a
  throughput correctness threshold.

### Installed packages and real Windows host

CI installs and tests the actual Windows installer, macOS Intel/Apple-silicon
packages, and Debian package. It invokes installed binaries from paths with
spaces and Unicode.

After isolated checks pass, test the Windows installer and full workflow via
passwordless SSH at `hites@10.0.0.9`. Resolve the executable explicitly, use a
new isolated `PACIFICDB_HOME` and dynamic ports, and preserve the machine's
existing installation and data. Capture version, package path, commands, exit
codes, logs, and results.

## Acceptance Criteria

P0 is complete only when:

1. Each confirmed failure has an old-behavior regression.
2. Focused tests pass after root-cause changes.
3. Existing Community and SDK tests pass.
4. Cross-platform installed-package tests pass.
5. Lifecycle, discovery, protocol, media, restart, and load matrices pass.
6. Acknowledged writes and ready media survive restart.
7. The real Windows host test passes.
8. Logs contain no credentials or payloads.
9. Remaining limitations are stated explicitly.

P0 success does not authorize publication by itself. A release is published
only after P1 and P2 pass equivalent review and final end-to-end checks.
