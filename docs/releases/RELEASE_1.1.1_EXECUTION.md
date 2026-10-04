# PacificDB 1.1.1 execution contract

The release owner supplied the complete release specification on 3 October 2026
and instructed execution, including local builds, validation, publication of
qualified packages, website deployment and a PR only after its gates pass.

## Scope and sequencing

- [x] Discover public engine/package/Workbench baselines and distribution workflows.
- [x] Create an isolated release checkout rooted in the public default branch.
- [x] Import the previously qualified database-first CLI/Workbench/SDK changes without deleting unrelated public files.
- [x] Align current component metadata to 1.1.1; preserve historical evidence and actual public download versions.
- [x] Map engine/storage/transport/replication APIs and audit retained optimization changes.
- [x] Build a candidate and execute relevant Community, SDK, browser and desktop checks.
- [x] Acquire the exact v1.0.0 baseline artifact; validate identity and perform repeated, matched benchmarks.
- [x] Retain raw measurements, configuration, resource counters, integrity results, and summarize variance/latency/regressions.
- [x] Build, inspect and install local npm, Python, Java, engine and Workbench artifacts.
- [x] Update website examples, Workbench guide, changelog and both required release documents from executed evidence.
- [x] Run final local checks and hosted Workbench/native-installer matrices; retain exact run and P0 evidence.
- [x] Causally profile the repeatable batch-10 regression with an engine-only concurrency/lock-wait diagnostic.
- [ ] Obtain required independent power/security review and resolve the batch-10 regression before stable publication.
- [ ] Publish only qualified, signed artifacts through existing configured workflows.
- [ ] Verify public registry versions and installed artifacts after publication; create the requested PR only when its stated gates pass.

## Binding invariants

Storage format 2 and Raft protocol 2 remain unchanged. Do not import format-3
one-sync, rejected batch/anchored/publication experiments or unproven durability
shortcuts. Preserve document/vector/media semantics, auth/RBAC, legacy project
APIs/data, and quorum acknowledgements. Ordinary UX remains database-first,
as explicitly approved earlier. No automatic merge. Every result is tied to
an exact source revision or artifact hash. Historical benchmarks are not 1.1.1
evidence. Unmeasured or unavailable results are labeled as such.

## Review focus

1. Runtime version vs package/installer/workflow identities must agree.
2. Artifact installation must use the inspected bytes, not an older registry package.
3. Missing namespace/authentication and failed writes must retain established semantics.
4. Mixed-version Raft, restart/crash, index, media and vector checks must precede release claims.
5. Publication must not bypass required evidence or announce unuploaded versions as stable.

## Initial findings and rulings

- Public stable engine baseline: `v1.0.0`; public Linux Workbench baseline:
  `workbench-linux-v1.0.1` (prerelease). Public npm SDK/CLI versions: 1.0.0.
- GitHub API found no PyPI/Maven protected environments or corresponding secrets;
  local PyPI/Maven credentials are absent. npm has an existing repository secret,
  but the local registry login is unauthorized. Never print secret values.
- Public Python `pacificdb` and Maven `io.pacificdb:pacificdb-client` metadata
  endpoints returned 404 during discovery; ownership remains unverified.
- The repository requires independent security and physical power interruption
  evidence for stable publication. These gates cannot be self-certified.
- Ruling: start from public branch `2bc7b99` and import only added/modified
  qualified snapshot files. The recovered snapshot's shallow history lacks a
  common ancestor, and its incidental deletions must not remove public docs/icons.
- Ruling: preserve public 1.0.0 download links and label 1.1.1 as a candidate
  until publication has succeeded. The request to show 1.1.1 as stable is
  conditional on its own publication/verification requirements.
- Ruling: execute the supplied detailed spec directly without repeated design
  approvals. Previously authorized database-first design remains binding.

## Evidence log

Record fresh commands and material outcomes under `benchmarks/results/v1.1.1/`
and the two release documents. No failed/slow completed benchmark is excluded.
