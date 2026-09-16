# Critical Production Gates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement every repository-owned critical-production gate except long-duration load testing, and produce fail-closed evidence for the external power-loss and independent-security-review gates.

**Architecture:** Repair crash qualification first, then add independent Python/Node evidence tools around the existing engine and RF3 harness. Replica comparison uses a bounded read-only engine digest endpoint and an external monitor so the observer does not share the engine failure domain. Physical power and external review remain explicit evidence consumers rather than self-certified results.

**Tech Stack:** C++17/nlohmann JSON/OpenSSL SHA-256, Node.js 18+ ESM, Python 3.10+ standard library, Bash, CMake, GitHub Actions, Kubernetes/Helm, systemd.

**Spec:** `docs/superpowers/specs/2026-09-16-critical-production-gates-design.md`

## Global Constraints

- Long-duration load and capacity testing is excluded and must be reported as `EXCLUDED`, never `PASS`.
- Physical power-loss evidence is `BLOCKED` until a dedicated run produces complete hardware, cache-policy, cut, reboot, ledger, and recovery evidence.
- Independent security review is `BLOCKED` until a named external reviewer supplies a dated report bound to the exact commit.
- No task pushes, tags, publishes, changes repository settings, deploys to a live cluster, or embeds credentials.
- Tests use disposable roots and never write the installed PacificDB data directory.
- Preserve all pre-existing worktree modifications and stage only task-owned files per commit.
- New behavior is introduced only after a focused test has failed for the expected reason.

---

### Task 1: Repair checkpoint crash qualification

**Files:**
- Modify: `engine/test/lsm_checkpoint_crash_driver.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- Consumes: `WAL::logPutBatch()` physical LSN range and six `FP_LSM_CHECKPOINT_*` failpoints.
- Produces: `db_engine_lsm_checkpoint_crash_driver --run-one NAME` and CTest entries named `lsm_checkpoint_crash_<boundary>`.

- [ ] **Step 1: Capture the current deterministic failure and trace the LSN**

Run:

```bash
./build-release-integrity/db_engine_lsm_checkpoint_crash_driver \
  --run-one FP_LSM_CHECKPOINT_BEFORE_WAL_RECLAIM
```

Expected: FAIL with `failpoint was not reached`. Inspect the generated WAL result and `markWalApplied()` path to determine the actual contiguous LSN passed to `LSM::flush()`.

- [ ] **Step 2: Make the driver expose the fixture's actual crash index**

Replace the hard-coded `kCrashLsn` with a fixture file written from the result returned by the final `LSM::putMany()` batch. The fixture must reject a missing/zero `last_lsn`; `runOne()` reads that literal value before setting `PACIFICDB_TEST_FAILPOINT_INDEX`.

The failing regression is: changing batch physical encoding from one LSN per document to one LSN per batch must not make the crash boundary unreachable.

- [ ] **Step 3: Build and run one boundary green**

```bash
cmake --build build-release-integrity -j2 --target db_engine_lsm_checkpoint_crash_driver
./build-release-integrity/db_engine_lsm_checkpoint_crash_driver \
  --run-one FP_LSM_CHECKPOINT_BEFORE_WAL_RECLAIM
```

Expected: `CHECKPOINT_CRASH_PASS` with 20,000 documents and identical consecutive verification results.

- [ ] **Step 4: Register and run all six boundaries**

Add six CTest registrations which invoke `--run-one` for:

```text
FP_LSM_CHECKPOINT_AFTER_SST_SYNC
FP_LSM_CHECKPOINT_AFTER_ARTIFACT_RENAME
FP_LSM_CHECKPOINT_AFTER_MANIFEST_SYNC
FP_LSM_CHECKPOINT_AFTER_MANIFEST_RENAME
FP_LSM_CHECKPOINT_BEFORE_WAL_RECLAIM
FP_LSM_CHECKPOINT_AFTER_WAL_RECLAIM
```

Add the same six invocations to `scripts/test-community.sh`, then run them from the current build.

- [ ] **Step 5: Commit the isolated repair**

```bash
git add engine/test/lsm_checkpoint_crash_driver.cpp engine/CMakeLists.txt scripts/test-community.sh
git commit -m "test: restore checkpoint crash qualification"
```

---

### Task 2: Add exact-commit qualification evidence

**Files:**
- Create: `scripts/release_qualification.py`
- Create: `scripts/test_release_qualification.py`
- Create: `docs/RELEASE_EVIDENCE.md`
- Modify: `docs/PRODUCTION_RELEASE.md`

**Interfaces:**
- Produces: `release_qualification.py --version VERSION --output PATH [--allow-dirty-development] [--run]`.
- JSON fields: `schema_version`, `version`, `revision`, `dirty`, `release_eligible`, `started_at`, `finished_at`, `platform`, `gates`, `artifacts`, `decision`.

- [ ] **Step 1: Write failing unit tests against controlled repositories**

Tests create a temporary Git repository and assert literal outcomes:

```python
def test_dirty_tree_is_not_release_eligible():
    evidence = qualification.collect_repository_state(repo)
    assert evidence["dirty"] is True
    assert evidence["release_eligible"] is False

def test_required_blocked_gate_prevents_stable_release():
    assert qualification.decide([
        {"name": "source", "status": "PASS", "required": True},
        {"name": "physical_power", "status": "BLOCKED", "required": True},
    ], stable=True) == "BLOCKED"

def test_excluded_soak_allows_only_controlled_candidate():
    assert qualification.decide([
        {"name": "long_duration_load", "status": "EXCLUDED", "required": False},
    ], stable=False) == "CONTROLLED_CANDIDATE"
```

Run `python3 -m unittest scripts/test_release_qualification.py -v`; expected failure is missing module/API.

- [ ] **Step 2: Implement state, command, artifact, and decision collection**

Use `subprocess.run(..., shell=False)`, monotonic duration measurement, UTC RFC3339 timestamps, SHA-256 streaming, and fixed status validation. Never include environment values or command output containing secrets; evidence paths are relative to the repository.

The default gate list contains focused source, SDK, package/container/Helm, upgrade, power, security-review, operations, and replica-integrity gates. `long_duration_load` is always emitted as `EXCLUDED` with reason `release_owner_excluded_2026-09-16`.

- [ ] **Step 3: Verify tests and dirty-tree behavior**

```bash
python3 -m unittest scripts/test_release_qualification.py -v
python3 scripts/release_qualification.py --version 0.1.0-beta.14 \
  --output /tmp/pacificdb-development-evidence.json --allow-dirty-development
```

Expected: tests pass; development evidence records `dirty: true` and `release_eligible: false`.

- [ ] **Step 4: Document evidence semantics and commit**

```bash
git add scripts/release_qualification.py scripts/test_release_qualification.py \
  docs/RELEASE_EVIDENCE.md docs/PRODUCTION_RELEASE.md
git commit -m "feat: bind release evidence to exact source state"
```

---

### Task 3: Implement mixed-version RF3 upgrade and rollback qualification

**Files:**
- Modify: `scripts/lib/raft-test-cluster.mjs`
- Create: `scripts/test-community-rf3-upgrade.mjs`
- Create: `scripts/test-rf3-upgrade-contract.mjs`
- Create: `docs/COMPATIBILITY.md`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- `RaftTestCluster` accepts `binaries: [path, path, path]` and `restartNode(index, binary)`.
- Upgrade command: `node scripts/test-community-rf3-upgrade.mjs --old-build PATH --candidate-build PATH --evidence PATH`.

- [ ] **Step 1: Write a failing cluster binary-swap contract test**

Create a deterministic fake-engine fixture that records its executable path. Assert `restartNode(2, candidateBinary)` stops node 2, retains its data root and ports, and starts exactly the candidate path. Expected RED: the cluster always uses `this.binary`.

- [ ] **Step 2: Add per-node binary selection and condition-based restart**

Store `this.binaries`; make `startNode()` spawn `this.binaries[index]`; implement `restartNode()` without deleting the node data directory. Preserve existing callers through the current single `build` option.

- [ ] **Step 3: Write the real upgrade harness**

The harness validates executable paths, creates a pre-upgrade backup, records acknowledged IDs, upgrades followers then leader, verifies health/catch-up after each step, performs full restart, checks every acknowledged ID, and restores the backup under a disposable isolated root. It writes evidence even on failure and cleans all processes.

If the exact beta.14 and candidate artifacts are the same binary, report `BLOCKED` with `distinct_artifacts_required`; do not mislabel a same-binary restart as mixed-version PASS.

- [ ] **Step 4: Add interruption, incompatibility, partition, disk-full, and rollback decisions**

The contract test uses fixtures to assert:

```text
pre-format-transition interruption -> RESUMABLE
unsupported protocol/storage version -> REJECTED_SAFE
post-format-transition binary rollback -> RESTORE_REQUIRED
missing old artifact -> BLOCKED
```

Real partition and disk-full cases reuse the existing proxy and ENOSPC harnesses, recording their evidence links rather than copying their implementation.

- [ ] **Step 5: Document exact compatibility and commit**

`docs/COMPATIBILITY.md` lists beta.14 to the candidate as the only initially supported rolling path and explicitly disallows arbitrary downgrades.

```bash
git add scripts/lib/raft-test-cluster.mjs scripts/test-community-rf3-upgrade.mjs \
  scripts/test-rf3-upgrade-contract.mjs docs/COMPATIBILITY.md scripts/test-community.sh
git commit -m "test: qualify mixed-version RF3 upgrades"
```

---

### Task 4: Add the physical power-cut evidence harness

**Files:**
- Create: `scripts/power_loss_harness.py`
- Create: `scripts/test_power_loss_harness.py`
- Create: `docs/schemas/physical-power-result.schema.json`
- Create: `docs/PHYSICAL_POWER_TEST.md`
- Modify: `scripts/probe-external-certification.sh`

**Interfaces:**
- `prepare`: writes immutable run metadata and an acknowledged-ID ledger under an explicit disposable absolute root.
- `arm`: launches one selected phase and prints `POWER_CUT_NOW run_id=... phase=...` only after the ledger and marker are persisted.
- `verify`: after reboot, performs read-only recovery and writes `PASS`, `FAIL`, or `BLOCKED` evidence.
- The script never invokes shutdown, reboot, IPMI, PDU, filesystem destruction, or power control itself.

- [ ] **Step 1: Write failing validation tests**

Use hand-written JSON fixtures to prove that missing hardware identity, unknown cache mode, absent cut timestamps, fewer iterations than required, digest mismatch, and VM/process-only cuts cannot produce `PASS`. Expected RED: validator module absent.

- [ ] **Step 2: Implement fail-closed schema and validator**

Require run ID, exact revision, dedicated-root path, filesystem/device/controller/drive/firmware/cache policy, operator, phase, iteration ledger digest, marker timestamp, power-restored timestamp, boot ID change, recovery result, and canonical digest match.

- [ ] **Step 3: Implement prepare/arm/verify state machine**

Reject `/`, `$HOME`, repository roots, relative paths, mounted roots containing the worktree, and non-empty roots without the generated ownership marker. `arm` flushes only harness metadata before printing the marker. `verify` refuses to run before observing a changed boot ID.

- [ ] **Step 4: Run a non-destructive dry run**

```bash
python3 scripts/power_loss_harness.py prepare --root /tmp/pacificdb-power-test \
  --phase wal_sync --revision "$(git rev-parse HEAD)" --iterations 1
python3 scripts/power_loss_harness.py validate --evidence \
  /tmp/pacificdb-power-test/evidence.json
```

Expected: preparation succeeds; validation remains `BLOCKED` because no physical cut/reboot evidence exists.

- [ ] **Step 5: Document operator safety and commit**

```bash
git add scripts/power_loss_harness.py scripts/test_power_loss_harness.py \
  docs/schemas/physical-power-result.schema.json docs/PHYSICAL_POWER_TEST.md \
  scripts/probe-external-certification.sh
git commit -m "test: add physical power-loss evidence harness"
```

---

### Task 5: Add security automation and independent-review validation

**Files:**
- Create: `.github/workflows/codeql.yml`
- Create: `.github/dependabot.yml`
- Create: `scripts/security_review.py`
- Create: `scripts/test_security_review.py`
- Create: `docs/SECURITY_REVIEW.md`
- Create: `docs/schemas/security-review-result.schema.json`
- Modify: `.github/workflows/ci.yml`
- Modify: `SECURITY.md`

**Interfaces:**
- `security_review.py bundle --output DIR --revision REVISION` creates a redacted manifest and SBOM inputs.
- `security_review.py validate --bundle DIR --review-result FILE` returns `PASS`, `FAIL`, or `BLOCKED`.

- [ ] **Step 1: Write failing review-evidence tests**

Fixtures assert that a missing reviewer, mismatched commit/report digest, unresolved critical/high finding, missing artifact digest, or self-review marker blocks acceptance. A complete external fixture returns `PASS`.

- [ ] **Step 2: Implement bundle and validator**

Use standard-library JSON and hashing. Inventory CMake/vcpkg, npm lockfile, Maven POM, Python metadata, GitHub Actions, and release artifacts without recording environment secrets. Generate a CycloneDX-compatible component document and a bundle manifest tied to the commit.

- [ ] **Step 3: Add repository security workflows**

CodeQL builds C++ and analyzes JavaScript on PR/main/schedule. Dependency review runs on pull requests. Dependabot covers npm, Maven, pip, GitHub Actions, and Docker monthly. Workflow permissions remain least-privilege and third-party actions are pinned to reviewed major or immutable revisions according to the repository policy test.

- [ ] **Step 4: Run the local security gate and commit**

```bash
python3 -m unittest scripts/test_security_review.py -v
python3 scripts/security_review.py bundle --output /tmp/pacificdb-security-review \
  --revision "$(git rev-parse HEAD)"
python3 scripts/security_review.py validate --bundle /tmp/pacificdb-security-review
```

Expected: tests and bundle pass; validation reports `BLOCKED` without an external review result.

```bash
git add .github/workflows/codeql.yml .github/dependabot.yml .github/workflows/ci.yml \
  scripts/security_review.py scripts/test_security_review.py docs/SECURITY_REVIEW.md \
  docs/schemas/security-review-result.schema.json SECURITY.md
git commit -m "ci: add security assurance evidence gates"
```

---

### Task 6: Publish the operational contract

**Files:**
- Create: `docs/OPERATIONS.md`
- Create: `docs/runbooks/BACKUP_RESTORE.md`
- Create: `docs/runbooks/CERTIFICATE_ROTATION.md`
- Create: `docs/runbooks/LOSS_OF_QUORUM.md`
- Create: `docs/runbooks/NODE_REPLACEMENT.md`
- Create: `docs/runbooks/UPGRADE_ROLLBACK.md`
- Create: `deploy/monitoring/pacificdb-alerts.yaml`
- Create: `scripts/validate_operations.py`
- Create: `scripts/test_validate_operations.py`
- Modify: `README.md`

**Interfaces:**
- `validate_operations.py` resolves every claimed numeric certification to an evidence JSON pointer and validates every required alert/runbook link.

- [ ] **Step 1: Write failing contract tests**

Test a temporary operations manifest with an unsupported numeric maximum and assert rejection. Test a complete fixture whose numeric observations reference the certification evidence and assert acceptance.

- [ ] **Step 2: Write the operating guide and runbooks**

Separate `guarantee`, `observed`, `operator_target`, and `blocked` labels. Include RPO/RTO, sizing formulas, backup frequency, restore drills, alert response, evidence preservation, and unsupported conditions without inventing an untested maximum database size or SLA.

- [ ] **Step 3: Add Prometheus alert rules and validation**

Cover WAL latency/errors, write stalls, flush/compaction backlog, disk/memory/FD pressure, connection saturation, Raft lag/quorum, integrity freshness/failure, backup age/failure, and certificate expiry. Every alert links to one runbook section.

- [ ] **Step 4: Verify and commit**

```bash
python3 -m unittest scripts/test_validate_operations.py -v
python3 scripts/validate_operations.py --docs docs/OPERATIONS.md \
  --alerts deploy/monitoring/pacificdb-alerts.yaml
git add docs/OPERATIONS.md docs/runbooks deploy/monitoring/pacificdb-alerts.yaml \
  scripts/validate_operations.py scripts/test_validate_operations.py README.md
git commit -m "docs: define production operating contract"
```

---

### Task 7: Add canonical replica digests and the external integrity monitor

**Files:**
- Modify: `engine/include/lsm.hpp`
- Modify: `engine/src/lsm.cpp`
- Modify: `engine/src/server.cpp`
- Modify: `engine/src/metrics_exporter.cpp`
- Create: `engine/test/replica_digest_test.cpp`
- Modify: `engine/CMakeLists.txt`
- Create: `scripts/replica-integrity-monitor.mjs`
- Create: `scripts/test-replica-integrity-monitor.mjs`
- Create: `deploy/kubernetes/replica-integrity-cronjob.yaml`
- Create: `deploy/systemd/pacificdb-integrity.service`
- Create: `deploy/systemd/pacificdb-integrity.timer`
- Modify: `engine/.env.example`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- C++: `LSM::logicalDigest(userId, dbName, collection, uint64_t fence, size_t maxDocs)` returns status, schema `pacificdb-logical-v1`, SHA-256, count, fence, and bounded/truncated flags.
- Protocol: `admin_replica_digest` requires `READ`, explicit database/collection/fence, and `maxDocs` within the configured cap.
- Monitor: `node scripts/replica-integrity-monitor.mjs --config FILE --output FILE [--prometheus FILE]`.

- [ ] **Step 1: Write the failing C++ digest test**

Insert the same logical documents in different orders/storage layouts and assert the literal same digest; mutate one value and assert different digest; set a visibility fence before a later MVCC write and assert the earlier digest; exceed `maxDocs` and assert fail-closed truncation rather than a partial comparable digest.

- [ ] **Step 2: Implement a bounded canonical digest**

Visit newest live documents visible at the requested fence, canonicalize object keys through nlohmann JSON's ordered object representation, encode length-prefixed IDs and MessagePack rows into SHA-256, sort by ID, and include the schema/fence/count in the final domain-separated digest. Do not return document contents.

- [ ] **Step 3: Expose the authenticated read-only endpoint**

Add `admin_replica_digest` to `permissionForAction()` as `READ`. Reject a fence above `lastApplied`, invalid identifiers, zero/oversized `maxDocs`, recovery-blocked state, and truncated digest. Return node/cluster identity, term, commit/applied indexes, digest schema/value/count, and local base/index verification status.

- [ ] **Step 4: Write failing monitor state tests**

Start three small fake TCP servers returning complete protocol responses. Assert literal outcomes and exit codes for `CONSISTENT`, `DIVERGENT`, `LAGGING`, `UNAVAILABLE`, `INCOMPATIBLE`, authentication failure, and response redaction.

- [ ] **Step 5: Implement monitor and deployment schedules**

Probe status first, select the minimum healthy `lastApplied`, poll lagging replicas until timeout, request matching digests, write atomic JSON/Prometheus outputs, and never log tokens, certificate bytes, or documents. Kubernetes and systemd examples load configuration/credentials from mounted files with a disabled-by-default schedule in development.

- [ ] **Step 6: Run focused and RF3 integration tests**

```bash
cmake --build build-release-integrity -j2 --target db_engine_replica_digest_test db_engine
./build-release-integrity/db_engine_replica_digest_test
node scripts/test-replica-integrity-monitor.mjs build-release-integrity
```

The RF3 case inserts replicated data, checks `CONSISTENT`, isolates a follower until it reports `LAGGING`, heals it, and returns to `CONSISTENT`.

- [ ] **Step 7: Commit the monitor**

```bash
git add engine/include/lsm.hpp engine/src/lsm.cpp engine/src/server.cpp \
  engine/src/metrics_exporter.cpp engine/test/replica_digest_test.cpp engine/CMakeLists.txt \
  scripts/replica-integrity-monitor.mjs scripts/test-replica-integrity-monitor.mjs \
  deploy/kubernetes/replica-integrity-cronjob.yaml deploy/systemd \
  engine/.env.example scripts/test-community.sh
git commit -m "feat: monitor replica logical integrity"
```

---

### Task 8: Integrate, qualify, and prepare the physical run

**Files:**
- Modify: `COMMUNITY_SCOPE.md`
- Modify: `docs/COMMUNITY_P0_CERTIFICATION.md`
- Modify: `docs/PRODUCTION_RELEASE.md`
- Modify: `scripts/test-community.sh`

**Interfaces:**
- Produces a clean local candidate commit and development qualification JSON.
- Physical execution remains a separate operator-confirmed phase.

- [ ] **Step 1: Run narrow checks**

```bash
git diff --check
python3 -m unittest scripts/test_release_qualification.py \
  scripts/test_power_loss_harness.py scripts/test_security_review.py \
  scripts/test_validate_operations.py -v
node scripts/test-rf3-upgrade-contract.mjs
node scripts/test-replica-integrity-monitor.mjs build-release-integrity
```

- [ ] **Step 2: Run engine durability and retained checks**

```bash
cmake -S engine -B build-release-integrity -DCMAKE_BUILD_TYPE=Release
cmake --build build-release-integrity -j2
scripts/test-community.sh build-release-integrity
scripts/test-helm-deployment.sh
```

If Node, npm, Java, Docker, Helm, credentials, platforms, or hardware are unavailable, record the exact gate as `BLOCKED`; do not install or substitute dependencies without approval.

- [ ] **Step 3: Update certification from fresh evidence**

Record exact commit candidates, commands, results, external blockers, and the explicit excluded soak gate. Do not preserve stale PASS language for changed engine paths.

- [ ] **Step 4: Commit the integrated candidate state**

Stage only in-scope files, inspect `git diff --cached`, and create a local candidate commit. Confirm `git status --short` is empty. Do not tag or push.

- [ ] **Step 5: Prepare—not execute—the physical cut**

Require the user to confirm a dedicated disposable data device/root, a complete backup of unrelated data, the exact power mechanism, and willingness to risk filesystem/OS damage. Run `prepare`, inspect evidence, run `arm`, and wait for `POWER_CUT_NOW`. Only then instruct the user to remove power. After reboot, resume with `verify`; repeat each selected phase and iteration independently.

- [ ] **Step 6: Produce the final decision**

Run the qualification orchestrator on the clean commit. The expected honest result before external review is `CONTROLLED_CANDIDATE` or `BLOCKED`, never universal stable production. Report physical evidence separately from the independent security-review blocker.
