# PacificDB Community Production-Certification Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expand PacificDB Community's release evidence with safe disk-full, network-partition, sustained RF3, contract-edge, restart-matrix, and real package-install certification.

**Architecture:** Extend the existing isolated Node/C++ certification harnesses. Use rootless mount namespaces for ENOSPC, directed TCP proxies for Raft partitions, disposable processes and containers for all destructive operations, and keep native platform/signing gates separate.

**Tech Stack:** C++17, Node.js 18+ standard library, Linux mount namespaces/tmpfs, Docker, CMake/CTest, npm test.

**Spec:** `docs/superpowers/specs/2026-09-12-production-certification-expansion-design.md`

## Global Constraints

- Never read or write the installed PacificDB data root.
- Never install over the host beta.3 package.
- Do not push, publish, tag, or create a release during certification; repository
  publication requires separate explicit authorization after verification.
- Never claim physical or native-platform evidence from a simulation.
- Keep all destructive roots below disposable `/tmp`, mount namespace, container, or VM storage.
- Fix production code only after a new regression demonstrates a real defect.

---

### Task 1: Exhaustive public-contract edge matrix

**Files:**
- Create: `scripts/test-community-contract-matrix.mjs`
- Modify: `scripts/test-community.sh`

- [x] Enumerate the prompt's database-name, document-boundary, aggregation,
  explain, role, and vector cases as named assertions.
- [x] Add timeout and malformed-response fake servers and exercise one real
  engine-backed command from every shell category.
- [x] Run the new scenario and fix only demonstrated contract defects with
  focused regressions.
- [x] Add the passing scenario to `scripts/test-community.sh`.

### Task 2: Complete persistence lifecycle and original reproduction

**Files:**
- Create: `scripts/test-community-restart-matrix.mjs`
- Modify: `scripts/test-community.sh`

- [x] Exercise create/read, normal restart/read, modify/read, normal
  restart/read, abrupt termination/recovery/read for every applicable
  persistent entity.
- [x] Run `emergency-persistence-test` through two additional normal restarts
  and fresh shells, then verify an unknown project cannot alter context.
- [x] Record per-entity results and add the scenario to the retained suite.

### Task 3: Genuine isolated disk-full failures

**Files:**
- Create: `scripts/test-community-disk-full.sh`
- Create: `scripts/test-community-disk-full.mjs`
- Modify: `scripts/test-community.sh`

- [x] Create a rootless mount namespace containing a bounded tmpfs and prove
  it returns ENOSPC without changing the host mount table.
- [x] Exercise each durable Community write category in a fresh bounded root,
  verify failures are explicit, release reserved space, restart, and check all
  earlier acknowledgements.
- [x] Cover CLI context/history failure separately with a bounded state root.
- [x] Add the scenario to the retained suite.

### Task 4: RF3 network partition and election campaign

**Files:**
- Create: `scripts/lib/raft-test-cluster.mjs`
- Create: `scripts/test-community-rf3-partition.mjs`
- Modify: `scripts/test-community-rf3.mjs`
- Modify: `scripts/test-community.sh`

- [x] Extract reusable RF3 process/port/convergence management.
- [x] Add six directed TCP proxies and a control mechanism that closes active
  sockets and drops new connections.
- [x] Partition the current leader, prove minority writes cannot acknowledge,
  prove majority leadership/write progress, heal, and require term/index/data
  convergence across all three nodes.
- [x] Repeat leader isolation/recovery enough to detect stale-leader behavior.

### Task 5: Sustained 64/128-client RF3 certification

**Files:**
- Create: `scripts/test-community-rf3-sustained.mjs`
- Modify: `scripts/test-community.sh`

- [x] Implement deterministic mixed CRUD workload with unique logical IDs,
  acknowledged-write history, retry classification, periodic term/lag samples,
  and end-state duplicate/loss verification.
- [x] Add configurable duration/repeat values for smoke execution.
- [x] Run the release gate as three 10-minute rounds at 64 clients and three at
  128 clients, sequentially.
- [x] Require zero errors, stable terms absent an intentional campaign event,
  and commit/apply convergence.

### Task 6: Real isolated Debian package installation

**Files:**
- Create: `scripts/test-debian-container-install.sh`
- Modify: `scripts/test-community.sh`

- [x] Build beta.7 and copy it into a disposable Ubuntu container.
- [x] Install with `apt`, verify package metadata and PATH, start the packaged
  engine under a disposable root, execute the packaged CLI, remove the package,
  and verify cleanup behavior.
- [x] Preserve the host beta.3 installation and data unchanged.

### Task 7: Hard-power and native-platform probes

**Files:**
- Create: `scripts/probe-external-certification.sh`

- [x] Probe for a disposable VM/hypervisor and, if available, perform a VM
  hard-power recovery test against a dedicated virtual disk.
- [x] Probe authorized Windows/macOS hosts and signing identities without
  printing secret material.
- [x] Run native package install/CLI suites and signing/notarization only where
  the required host and identity exist.
- [x] Emit machine-readable BLOCKED evidence for unavailable physical hardware,
  hosts, certificates, or notarization credentials.

### Task 8: Full rerun and evidence report

**Files:**
- Modify: `docs/COMMUNITY_P0_CERTIFICATION.md`

- [x] Run static checks and all focused scenarios.
- [x] Run the complete retained suite once after the last code change.
- [x] Rebuild and hash the package.
- [x] Update exact counts, commands, results, limitations, and separate Linux,
  hardware, Windows, and macOS verdicts.
- [x] Verify no test process, mount, container, or temporary firewall rule
  remains.
