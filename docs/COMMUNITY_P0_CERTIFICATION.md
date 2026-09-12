# PacificDB Community durability and production-certification report

Date: 2026-09-12

Source branch: `feature/community-complete-cli`

Source/package version tested: `0.1.0-beta.8`

Source commit at test start: `5ed6bdf405f3be4ec586645d8387d08478a02aaf`

## Verdicts

| Scope | Verdict | Evidence |
|---|---|---|
| P0 acknowledged-write repair | **PASS** | The old failure was reproduced, repaired, and covered by abrupt cross-process recovery. |
| Linux source and SDK candidate | **PASS** | Complete retained suite: 55/55 test units passed. |
| Linux Debian artifact | **PASS** | Extracted-package smoke plus real Ubuntu 24.04 `apt` install, engine read/write, remove, and data-preservation checks. |
| RF3 correctness and sustained load | **PASS** | TCP partition/election campaign plus three 10-minute 64-client and three 10-minute 128-client rounds with zero operation errors. |
| Physical power/storage-controller durability | **BLOCKED** | No dedicated power-cut/storage-controller harness is configured. |
| Native Windows package and signing | **BLOCKED** | No authorized Windows host or signing identity is configured. |
| Native macOS package, signing, and notarization | **BLOCKED** | No authorized macOS host, signing identity, or notarization credentials are configured. |
| Global multi-platform production readiness | **BLOCKED** | The three external gates above have no valid evidence. |

`0.1.0-beta.8` is a Linux release candidate. This report does not label it a
globally production-ready release because physical durability and native signed
Windows/macOS installation cannot be certified from this Linux host.

The certification run itself did not upgrade the host package or modify
paid/private control-plane code. Release publication is tracked separately from
the test evidence in this report.

## Protected paths and artifact inventory

All destructive work used disposable roots below `/tmp`, rootless mount
namespaces, or disposable containers. No installed PacificDB data root was
opened for writing.

| Item | Observed value |
|---|---|
| Installed host CLI | `/usr/bin/pacificdb` |
| Installed host Debian package | `pacificdb-community 0.1.0~beta.3 amd64` |
| Tested source CLI | `build/pacificdb`, version `0.1.0-beta.8` |
| Tested engine | `build/db_engine` |
| Final Debian artifact | `build/pacificdb-community-0.1.0-beta.8-Linux.deb` |
| Final artifact SHA-256 | `ad9b7862772f3b163b3c67e71a00ecaaccedecdfada4f1e223c55c788d931e44` |
| Ubuntu package-manager test | Ubuntu 24.04 container, `apt install` then `apt remove` |
| Linux installed-data default | `${XDG_DATA_HOME:-$HOME/.local/share}/pacificdb` |
| macOS installed-data default | `~/Library/Application Support/PacificDB` |
| Data, backup, restore | `$PACIFICDB_HOME/data`, `$PACIFICDB_HOME/backup`, `$PACIFICDB_HOME/restore` |
| Linux CLI state | `${XDG_STATE_HOME:-$HOME/.local/state}/pacificdb` |
| macOS CLI state | `~/Library/Application Support/PacificDB` |
| Windows CLI state | `%LOCALAPPDATA%/PacificDB` |

The CLI is a JSON-over-TCP client. The native package installs the CLI and
engine as separate executables; plain `pacificdb` starts the loopback engine
automatically when needed.

## Original reproduction and repair

The original failure was reproduced in an isolated root:

| Step | Expected | Old result |
|---|---|---|
| `create project hi` | Canonical ID and durable write | Canonical ID returned |
| `use project hi` | Resolve an existing ID or fail | Unchecked text stored |
| `show project` | Selected project | `project_not_found` |
| Create another project, SIGKILL, restart | Acknowledged project remains | Project disappeared |

`pacificdb_meta.projects` already used the replicated document path. The
single-node leader returned success after fsync and apply but before persisting
its committed/applied watermark.

The repaired defects are:

1. The npm and native shells resolve project/database selections and store only
   canonical values. A lookup failure preserves the previous context.
2. `RaftCore::replicateAndApply` persists standalone `commitIndex` and
   `lastApplied` before acknowledgement. An `_Exit(0)` cross-process
   regression proves recovery.
3. LSM snapshot apply preserves engine-local `security`, `restores`, and
   `shard_map.json` state that is outside the snapshot payload.
4. Backup lookup, verification, and restore failures are explicit, and the Node
   SDK retains safe public error detail.
5. Native shell errors omit Raft, tracing, request, password, and API-key hash
   fields.
6. Vector queries require a finite non-empty numeric vector, positive integer
   `k`, and a supported metric.
7. Database/collection-dependent operations reject missing resources. Dotted
   equality does not select the top-level equality-index fast path.
8. Native CLI context/history writes check open, flush, and close failures.

## Expanded certification results

### Beta.8 installation and CLI experience

The native and npm CLIs display the Community beta banner and open the shell
when invoked as plain `pacificdb`. For loopback connections, the command starts
one background engine if needed, even when two clients make the first request
at the same time. The engine uses the documented platform data directory and
records its log and PID there. `--no-start` keeps remote/operator-managed
connections client-only.

The beta.8 native package contains `pacificdb`, `db_engine`, the exact supplied
PNG logo, licenses, and documentation. The obsolete `pacificdb-local` launcher
is no longer installed.

### Public contract matrix

| Contract area | Passed |
|---|---:|
| Database-name cases | 13 |
| Document-boundary cases | 15 |
| Aggregation cases | 16 |
| Explain cases | 4 |
| Role/authorization cases | 8 |
| Vector cases | 18 |
| Timeout command categories | 9 |
| Malformed-response command categories | 9 |

Unsupported aggregation stages, malformed values, invalid identities, missing
resources, invalid vectors, timeouts, truncated replies, and wrong response
shapes fail explicitly.

### Exact persistence lifecycle

The matrix passed for 13 persistent entities. It performed create/read, normal
restart/read, modify/read, a second normal restart/read, abrupt
termination/recovery/read, and three fresh-shell checks where applicable. The
exact `emergency-persistence-test` project survived the two additional normal
restarts and abrupt recovery.

### Genuine disk-full injection

A rootless mount namespace, 128 MiB tmpfs, and loopback request path produced
genuine kernel ENOSPC. Each case filled the filesystem to 134,217,728 bytes.
All 21 write categories returned failure rather than false acknowledgement:

`project-create`, `project-delete`, `database-create`, `database-drop`,
`collection-create`, `collection-drop`, `document-insert`,
`document-update`, `document-delete`, `index-create`, `index-drop`,
`api-key-create`, `api-key-revoke`, `backup-create`, `backup-restore`,
`media-begin`, `media-chunk`, `media-finalize`, `vector-put`,
`cli-history`, and `cli-context`.

After reserved space was released, the engine restarted and all earlier
acknowledged state was checked.

### Network partitions and elections

All six directed peer links used controllable TCP proxies. Isolation closed
existing sockets and dropped new connections. This is a real userspace TCP
transport partition.

| Round | Isolated leader | Majority-elected node | New term | Commit index after heal |
|---|---:|---:|---:|---:|
| 1 | 0 | 1 | 1 | 5 |
| 2 | 1 | 0 | 2 | 7 |

Minority writes acknowledged zero times. Both majority writes acknowledged.
After healing, all replicas converged to term 2, commit index 7, and identical
data.

### Sustained RF3 runs

Six release rounds ran sequentially for 10 minutes each with paced mixed CRUD,
unique IDs, immediate reads, updates, transient deletes, term/lag monitoring,
and full final-map comparison.

Those long rounds ran on the certified `5ed6bdf` engine core. Beta.8 changes
the CLI, packaging, branding, and documentation without changing that engine
core; a fresh beta.8 64/128-client smoke run repeated the RF3 path.

| Clients | Passing rounds | Operations | Final documents | Errors | Largest apply lag | Slowest final convergence |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 3 | 161,274 | 57,597 | 0 | 5 | below 1 second |
| 128 | 3 | 322,546 | 115,193 | 0 | 202 | 12.320 seconds |
| **Total** | **6** | **483,820** | **172,790** | **0** | **202** | **12.320 seconds** |

Terms stayed stable. Every successful round ended with equal commit/apply
indexes and identical document maps on all replicas.

One extra 128-client attempt did not qualify because the earlier harness used a
60-second final-convergence deadline and reported a replica mismatch at that
deadline. Its workload reported no operation errors. Diagnostics were improved
and the deadline raised to 300 seconds; two subsequent independent 128-client
rounds passed, including the measured 12.320-second catch-up. The nonqualifying
attempt remains recorded.

### Debian package installation

The final beta.8 artifact passed:

- isolated extraction followed by packaged CLI/engine document, media, and
  vector round trips;
- real `apt install` inside Ubuntu 24.04, package/PATH validation, packaged
  engine write/read, `apt remove`, executable removal, and preservation of
  created data.

The host beta.3 installation and host data remained unchanged.

### External probes

QEMU and `/dev/kvm` exist, but no dedicated disposable VM image is configured.
That cannot prove physical power or storage-controller cache behavior. No
authorized Windows/macOS hosts or signing/notarization identities are
configured. The probe emitted machine-readable `BLOCKED` results without
printing credential values.

## Test summary

| Harness | Run | Passed | Failed | Skipped |
|---|---:|---:|---:|---:|
| C++ retained executables | 29 | 29 | 0 | 0 |
| Natural-query JavaScript check | 1 | 1 | 0 | 0 |
| Node client tests | 5 | 5 | 0 | 0 |
| Node CLI tests | 9 | 9 | 0 | 0 |
| Native/npm automatic-start scenario | 1 | 1 | 0 | 0 |
| Real-engine Community E2E | 1 | 1 | 0 | 0 |
| Public contract matrix | 1 | 1 | 0 | 0 |
| Persistence/restart matrix | 1 | 1 | 0 | 0 |
| Genuine ENOSPC matrix | 1 | 1 | 0 | 0 |
| RF3 replication/catch-up | 1 | 1 | 0 | 0 |
| RF3 TCP partition/election | 1 | 1 | 0 | 0 |
| RF3 64/128-client smoke | 1 | 1 | 0 | 0 |
| Python SDK tests | 1 | 1 | 0 | 0 |
| Java SDK tests | 1 | 1 | 0 | 0 |
| YCSB binding | 1 | 1 | 0 | 0 |
| **Retained suite total** | **55** | **55** | **0** | **0** |
| Extracted Debian package smoke | 1 | 1 | 0 | 0 |
| Ubuntu `apt` lifecycle | 1 | 1 | 0 | 0 |
| **Linux total** | **57** | **57** | **0** | **0** |

The C++ comprehensive executable separately reported 58/58 internal
assertions. The E2E executed 78 shell commands and checked 24 concurrent
acknowledged writes, six concurrent projects, six concurrent API-key
create/revoke cycles, four concurrent media chunks, a graceful restart, and a
SIGKILL recovery.

Final commands:

```text
scripts/test-community.sh build
scripts/test-community-autostart.sh build
cmake --build build -j2 --target package
scripts/test-native-package.sh build/pacificdb-community-0.1.0-beta.8-Linux.deb
scripts/test-debian-container-install.sh build/pacificdb-community-0.1.0-beta.8-Linux.deb
scripts/probe-external-certification.sh
git diff --check
node --check cli/src/cli.js
node --check cli/src/shell.js
node --check sdk/node/src/index.js
node --check scripts/test-community-e2e.mjs
node --check scripts/test-community-rf3.mjs
node --check scripts/test-community-rf3-partition.mjs
node --check scripts/test-community-rf3-sustained.mjs
node --check scripts/test-community-contract-matrix.mjs
node --check scripts/test-community-restart-matrix.mjs
```

## Command certification

| Category | Certified behavior | Result |
|---|---|---|
| Authentication | `login`, `whoami`, `logout`; invalid/revoked credentials fail | PASS |
| Projects | create/list/use/show/delete; canonical IDs and restart paths | PASS |
| Databases | create/list/use/show/drop; reserved/system names hidden | PASS |
| Collections | create/list/drop; missing/reserved resources fail | PASS |
| Documents | insert/find/findOne/update/delete/count and boundary errors | PASS |
| Queries | bounded aggregate and honest explain; bad stages fail | PASS |
| Backups | create/list/show/verify/export/restore/list restores/delete | PASS |
| API keys | create/list/show/revoke; secret hiding and roles | PASS |
| Media | sequential upload/download/list/find/show/delete/cleanup | PASS |
| Vectors | put/query, ordering, metrics, bad vector/metric/`k` | PASS |
| Shell | help/context/status/history/clear/request/exit/quit and redaction | PASS |

## Durability matrix

| Persistent entity | Create/read | Two normal restarts | Modify/read | Abrupt recovery/read | Result |
|---|---|---|---|---|---|
| Project | Yes | Yes | Create/delete/map | Yes; RF3 catch-up | PASS |
| Database | Yes | Yes | Create/drop/select | Yes; RF3 create | PASS |
| Collection | Yes | Yes | Create/list/drop | Yes; RF3 create | PASS |
| Document | Yes | Yes | Update/delete/insert | Yes; concurrent writes | PASS |
| Secondary index | Yes | Yes | Indexed update/delete | WAL recovery | PASS |
| User/auth state | Bootstrap/login | Yes | Logout/relogin | Fresh login after SIGKILL | PASS |
| API-key metadata | Create/list/show | Yes | Create/revoke | Durable revocation | PASS |
| API-key revocation | Revoke/auth failure | Yes | Additional keys | Rejected after SIGKILL | PASS |
| Backup metadata | Create/list/show | Yes | Verify/delete | Read after SIGKILL | PASS |
| Restore journal | Success/failure | Yes | Additional attempts | Read after SIGKILL | PASS |
| Media manifest | Begin/finalize/get | Yes | Resume/delete/cleanup | SIGKILL and RF3 | PASS |
| Media chunks | Put/get/checksum | Yes | Resume/download | SIGKILL and RF3 | PASS |
| Vector data | Put/query | Yes | Multiple vectors | Query after SIGKILL | PASS |
| CLI context/history | Create/read | Fresh shells | Update/clear | ENOSPC explicit | PASS |

## Remaining production gates

- **BLOCKED — physical power/storage controller:** this requires a dedicated
  host, controllable power cut, known drive/controller cache policy, and
  post-reboot consistency inspection. Process or VM termination cannot prove it.
- **BLOCKED — native Windows:** no authorized Windows runner or signing
  certificate is configured, so native install, SmartScreen, service,
  signature, and uninstall behavior cannot be certified.
- **BLOCKED — native macOS:** no authorized macOS runner, Developer ID identity,
  or notarization credentials are configured, so Gatekeeper, notarization,
  service, and uninstall behavior cannot be certified.
- The TCP proxy campaign proves application transport partition behavior. A
  separate multi-host/kernel-firewall campaign remains useful infrastructure
  evidence.
- Real power loss, kernel panic, and controller-cache loss remain untested. The
  abrupt software tests cover process SIGKILL and `_Exit(0)`.

No test process, mount, test container, or temporary firewall rule remained
after the final cleanup audit. Package-smoke cleanup removes its disposable
extraction and data roots.
