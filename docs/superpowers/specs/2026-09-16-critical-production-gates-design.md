# PacificDB Critical Production Gates

**Date:** 2026-09-16

**Status:** Approved design, pending implementation plan

## Goal

Close the repository-implementable blockers for controlled critical-data
deployments and broad self-service release qualification, while preserving
honest evidence boundaries. Long-duration load and capacity testing is
explicitly excluded from this program at the release owner's request.

This program extends
`docs/superpowers/specs/2026-09-15-production-readiness-program-design.md`.
Where the two documents overlap, this document narrows the current execution
scope but does not weaken the earlier fail-closed release requirements.

## Scope and evidence boundary

The implementation covers:

1. checkpoint crash-test correctness and complete checkpoint failpoint runs;
2. exact-commit release-candidate qualification and evidence aggregation;
3. mixed-version RF3 upgrades, interrupted upgrade recovery, rollback limits,
   and backup/restore verification;
4. a physical power-loss harness contract and post-reboot verifier;
5. automated security gates and an independent-review evidence package;
6. production operating limits, RPO/RTO definitions, alerts, recovery, backup,
   restore, certificate rotation, node replacement, and upgrade runbooks;
7. periodic read-only replica-integrity monitoring; and
8. a clean, locally committed release candidate after all locally executable
   required checks pass.

The implementation does not run or claim completion of a 24–72-hour load
test. It also does not fabricate external evidence. Physical power-loss status
remains `BLOCKED` until a dedicated host with a documented storage-controller
cache policy and controllable power source produces a valid signed result.
Independent security-review status remains `BLOCKED` until a named external
reviewer supplies a dated report and the release owner records the disposition
of every critical and high finding.

No task publishes a release, pushes commits, creates a tag, changes GitHub
repository settings, deploys to a live cluster, buys or installs signing
credentials, or modifies customer data.

## 1. Checkpoint crash qualification

The checkpoint crash driver must derive its selected failpoint index from the
actual contiguous WAL range produced by the fixture. It must not hard-code an
LSN that becomes stale when WAL physical-record encoding changes.

The retained crash suite runs these boundaries independently:

- after SST synchronization;
- after artifact rename;
- after manifest synchronization;
- after manifest rename;
- before WAL reclamation; and
- after WAL reclamation.

For every boundary, two consecutive post-crash recoveries must produce the
same document count, canonical digest, published SST inventory, and retained
WAL byte count. A failpoint that is not reached is a test failure, never a
skip. The suite must retain durable `insertMany` coverage for one physical WAL
record and shared group-commit synchronization.

## 2. Exact-commit release qualification

A dependency-light qualification orchestrator runs the locally available
release checks and writes one JSON evidence document. The document records:

- schema version;
- requested release version;
- Git commit and dirty-tree status;
- UTC start/end timestamps;
- platform and tool versions;
- every gate name, command, status, duration, and evidence path;
- explicit `PASS`, `FAIL`, `BLOCKED`, or `EXCLUDED` status;
- SHA-256 digests for referenced release artifacts; and
- the overall release decision.

The orchestrator fails closed. A required `FAIL` or `BLOCKED` gate prevents a
stable decision. The excluded long-duration load gate appears as `EXCLUDED`
with the release-owner decision recorded and prevents universal scale claims,
but it does not prevent creation of a controlled production candidate.

Qualification refuses stable evidence from a dirty tree. Development runs may
inspect a dirty tree, but their output is labelled non-release evidence. The
final local candidate is created only after focused and retained tests pass;
commits are never pushed or tagged automatically.

## 3. Mixed-version upgrade and rollback

The supported initial path is the latest available beta.14 artifact to the new
candidate. The harness accepts explicit old and candidate engine/CLI artifact
paths rather than downloading mutable tags.

The RF3 sequence is:

1. create and verify a pre-upgrade backup;
2. begin bounded acknowledged traffic with unique operation identifiers;
3. upgrade one follower and wait for health and catch-up;
4. upgrade the second follower and wait for health and catch-up;
5. transfer or re-elect leadership away from the old leader;
6. upgrade the final node;
7. verify every acknowledged identifier and replica convergence;
8. restart every node and repeat convergence and data verification;
9. restore the pre-upgrade backup into an isolated data root; and
10. verify the restored canonical data digest.

Separate cases interrupt an upgrade between process stop and binary
replacement, reject unsupported protocol/storage versions, exercise a network
partition during the supported mixed-version window, and exercise disk-full
handling on the upgrading node.

Rollback is allowed only while the candidate has not committed an irreversible
storage-format transition. Once such a transition is published, the harness
must reject binary rollback and direct the operator to restore the verified
pre-upgrade backup. The compatibility policy lists exact supported source and
target versions rather than promising arbitrary downgrade support.

## 4. Physical power-loss harness contract

The repository supplies a controller-neutral command contract, JSON Schema,
fixture validator, and post-reboot inspection command. A hardware adapter must
provide these operations:

- report host, filesystem, block device, controller, drive model, firmware,
  write-cache mode, and power-cut mechanism;
- start the selected deterministic mutation phase;
- confirm the target failpoint/evidence marker without treating it as durable;
- remove power without a graceful guest shutdown;
- restore power and wait for the host;
- collect boot, filesystem, engine, and storage diagnostics; and
- run read-only recovery verification.

Supported phases are WAL append/sync, SST creation, manifest publication, WAL
reclamation, compaction, backup, and restore. Every phase must be repeated by
an externally configured count. The result is `PASS` only when every required
iteration survives and the recovered canonical digest matches the acknowledged
operation ledger. Missing hardware identity, cache policy, controller action,
iteration evidence, or post-reboot verification yields `BLOCKED` or `FAIL`.

A VM reset or process kill may be stored as supplemental evidence but can
never satisfy the physical gate.

## 5. Security assurance

Repository automation adds or verifies:

- CodeQL analysis for C++ and JavaScript;
- pull-request dependency review;
- Dependabot configuration for supported package ecosystems;
- secret-pattern checks that redact matching values;
- dependency inventory and CycloneDX-compatible SBOM production;
- compiler hardening and release-build metadata;
- pinned or policy-validated GitHub Action references; and
- negative security tests for authentication, authorization, tenant
  isolation, TLS/mTLS, backup/restore authorization, parser bounds, path
  containment, and resource safeguards.

An independent-review bundle contains the exact commit, source archive digest,
artifact digests, SBOMs, threat boundaries, production invariants, supported
configuration, excluded features, compiler options, test evidence, disclosure
path, and remediation policy. A machine-readable acceptance record requires
the reviewer identity, organization, report date, reviewed commit, report
digest, finding counts by severity, and disposition of every critical/high
finding.

Automation validates review evidence but does not create the independent
review. Until valid external evidence is supplied, the gate remains `BLOCKED`.

## 6. Operational contract

The production operations guide distinguishes guarantees from targets and
untested assumptions. It defines:

- acknowledged-write RPO for tested failure classes;
- recovery and failover RTO observations, without turning a single observation
  into a universal SLA;
- the currently certified platform, topology, concurrency, and data-volume
  envelope;
- memory, storage, file-descriptor, and network sizing procedures;
- backup frequency from the operator's chosen RPO;
- verification, export, retention, and isolated restore drills;
- WAL, memtable, flush, compaction, disk, memory, connection, replication-lag,
  integrity, backup-age, and certificate-expiry alerts;
- emergency recovery and evidence-preservation steps;
- loss-of-quorum, node replacement, certificate rotation, upgrade, interrupted
  upgrade, rollback, and restore runbooks; and
- unsupported configurations and escalation conditions.

No maximum database size, RPO, RTO, or throughput is invented. A numeric value
may be called certified only when linked to release-specific evidence. Values
without sufficient evidence remain operator-configured targets or explicit
release blockers.

## 7. Replica-integrity monitor

Replica comparison runs outside the database process so that the observer does
not share the failure domain of the engine being checked. The repository ships
a read-only monitor command plus Kubernetes CronJob and systemd timer examples.

Each configured replica exposes or returns:

- cluster and node identity;
- term, commit index, and applied index;
- a canonical logical digest for each requested collection at an explicitly
  selected common applied index;
- document count and digest algorithm/version; and
- local base/index integrity status.

The monitor first chooses the minimum applied index shared by healthy members.
It requests canonical digests at that fence, compares only responses with the
same digest schema and fence, and reports:

- `CONSISTENT` when every required replica agrees;
- `DIVERGENT` when comparable digests differ;
- `LAGGING` when a replica cannot reach the fence before the deadline;
- `UNAVAILABLE` when quorum evidence cannot be collected; and
- `INCOMPATIBLE` for digest/protocol schema mismatch.

It emits JSON without document contents or credentials, returns nonzero for all
states except `CONSISTENT`, and exposes counters/gauges suitable for Prometheus.
Checks are bounded by configurable collections, interval, timeout, concurrency,
and maximum scan work. They never repair, compact, delete, or rewrite data.

Authentication uses existing TLS/mTLS and admin credentials supplied through
files or environment references; examples never embed secrets. Operators can
disable the schedule, but production documentation treats an absent successful
check within the configured interval as an alert.

## 8. Testing strategy

Behavior changes follow test-first development. Each new executable contract
has focused deterministic tests that are observed failing before the minimal
implementation is added.

The final locally executable gate includes:

- checkpoint crash boundaries;
- grouped WAL and batch recovery tests;
- release evidence and dirty-tree policy tests;
- mixed-version harness contract tests plus an available real-artifact run;
- power-result schema and fail-closed validator tests;
- security workflow, SBOM, secret-check, and review-evidence validator tests;
- operational-document contract tests;
- replica-monitor agreement, divergence, lag, unavailable, incompatible,
  authentication-failure, and redaction tests;
- full retained C++ and available SDK suites;
- package, container, and Helm validation available on the host; and
- `git diff --check`.

Unavailable platforms, credentials, physical hardware, external reviewers, and
repository settings are reported as `BLOCKED`, never silently skipped or
converted to passing evidence.

## Acceptance criteria

The implementation is complete when:

1. every repository-owned implementation and focused test above exists and
   passes in its supported local environment;
2. checkpoint crash failpoints are reached and recover stable data;
3. the qualification report binds evidence to one commit and fails closed;
4. upgrade, rollback, power, security-review, operations, and replica-monitor
   contracts are executable and documented;
5. all external evidence is accurately `PASS`, `FAIL`, or `BLOCKED`;
6. the long-duration load gate is visibly `EXCLUDED`, not falsely passed;
7. no unrelated user-owned change is overwritten; and
8. the final handoff names all unresolved external blockers and does not call a
   controlled candidate universally production-ready.
