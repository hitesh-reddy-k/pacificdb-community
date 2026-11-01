# RC publication follow-through

> Execute the existing user-approved v1.2 release design using code-work,
> systematic debugging, TDD and independent review. Latest user instruction
> explicitly authorizes fixing release blockers, publishing applicable channels,
> then benchmarking the published artifacts. No further ordinary approval needed.

Goal: publish Pre-release v1.2 as v1.2.0-rc.1 with truthful scoped qualification,
then generate a new paired benchmark from downloaded release bytes.

Constraints: preserve unified data/WAL/LSM/RF3/document/media flows and prior work;
no waived failing tests, advisory suppression, force pushes, stable/latest
promotion, fabricated credentials or production-readiness claims. Use existing
branch per user. Existing staged versions remain immutable historical attempts.

- [x] Reproduce standalone commit/checkpoint and later rejected-tail failures.
- [x] Add early publication, syscall-error, late-publication and rollback tests.
- [ ] Serialize standalone durable publication using existing Raft log ownership;
      rollback unpublished tails; fence uncertain rename/directory sync failures.
- [ ] Confirm ordinary, group-sync, crash, ENOSPC, RF3 and full Community regressions.
- [ ] Repair dependency compatibility against patched minimatch without exceptions;
      upstream glob tests, package tests and actual Workbench package execution.
- [ ] Resolve review findings; update storage/compatibility docs with measured scope.
- [ ] Provide executable qualification from exact receipts; reject stale/tampered
      evidence or incomplete mandatory gates. Keep stable gates separate.
- [ ] Commit/rebuild once integration is frozen; stage/install/test final artifact
      bytes. Bind receipts and SHA256SUMS to the exact release revision.
- [ ] Create/push immutable RC tag only after scoped prerelease qualification.
- [ ] Publish GitHub prerelease with verified artifacts; download and independently
      verify identity, checksums, installation and examples.
- [ ] Publish npm next and approved GHCR RC when credentials permit; attempt PyPI
      OIDC and Maven only with valid owner publisher/signing setup. Keep missing
      credentials/mappings Blocked with exact owner actions, never bypass ownership.
- [ ] Benchmark downloaded native/SDK release bytes against same-settings local
      PostgreSQL18/pgvector; retain failures, raw data, hashes, build costs, recall,
      exact/filter/hybrid/resource/recovery and performance limitations.
- [ ] Attach benchmark evidence only after it completes; provide exact public URLs,
      distribution status and outstanding external blockers.
