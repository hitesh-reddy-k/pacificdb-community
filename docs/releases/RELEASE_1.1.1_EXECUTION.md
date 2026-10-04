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
- [x] Record the release owner's explicit 4 October 2026 override accepting the missing independent power/security review and documented batch-10 regression for v1.1.1.
- [x] Restrict the unsigned-artifact exception to the v1.1.1 engine and Workbench tags; keep the existing signing requirement for later releases.
- [x] Authorize publication through the existing configured workflows, followed by public artifact verification and a PR with actual workflow outcomes.

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
5. Publication status must be verified against each public registry or release after the authorized workflows finish.

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
- Superseding release-owner ruling on 4 October 2026: publish v1.1.1 despite the
  missing independent review, documented batch-10 regression, and unavailable
  platform signing. The unsigned exception is exact-version only. Publication
  failures must still be reported rather than represented as successful.
- Ruling: execute the supplied detailed spec directly without repeated design
  approvals. Previously authorized database-first design remains binding.

## Evidence log

Record fresh commands and material outcomes under `benchmarks/results/v1.1.1/`
and the two release documents. No failed/slow completed benchmark is excluded.

## Publication outcomes

- [x] Created and pushed branch `v-1.1.1`; annotated `v1.1.1` and
  `workbench-v1.1.1` both peel to release commit `9800c9abb5891f974d0d413c6be6fac92649f912`.
- [x] Published the stable native release and GHCR image in run `37181644375`;
  downloaded checksums, embedded versions, manifest revision and image digest pass.
- [x] Published `@pacificdb/client@1.1.1` and `@pacificdb/cli@1.1.1` in run
  `37181644291`; both are npm `latest`, public bytes match inspected tarballs, and
  a clean install passed real CRUD against the downloaded engine.
- [ ] Publish `pacificdb==1.1.1` to PyPI. Run `37191728049` passed every build,
  matrix and exact-artifact check, then PyPI rejected OIDC with
  `invalid-publisher`. Tagged-source installation is the verified fallback.
- [ ] Validate/publish `io.pacificdb:pacificdb-client:1.1.1` through Central.
  Run `37191730662` passed every build, matrix and artifact check, then stopped
  because the owner GPG identity and Central token are not configured.
- [x] Deployed the final 1.1.1 website in Pages run `37192873112`; all three public
  pages return HTTP 200 and the repository/site comparison-removal scan passes.
- [x] Published Workbench in run `37191983730`; all platform jobs passed, every
  public asset matches `SHA256SUMS`, and the extracted Debian package plus bundled
  engine/CLI report 1.1.1.
- [x] Responded to the PR dependency-review failure by updating the unpublished
  Java client to Jackson 2.18.11 at commit
  `25fb81d413973b6779eaf71a26bedb42f6d79be3`; 29 tests, the dependency tree,
  installed consumer, authenticated TCP/TLS recovery and release examples pass.
- [x] Created review PR
  [#31](https://github.com/hitesh-reddy-k/pacificdb-community/pull/31). The PR is
  intentionally not merged; its live CI conclusion is recorded in the final report.
