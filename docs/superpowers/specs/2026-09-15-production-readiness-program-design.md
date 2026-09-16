# PacificDB Cross-Platform Production Readiness Program

**Date:** 2026-09-15

**Status:** Approved design, pending implementation plans

## Goal

Produce a defensible stable PacificDB Community release for the currently
supported native platform matrix:

- Linux AMD64 Debian package;
- Windows x64 NSIS installer;
- macOS 15 Apple silicon package;
- macOS 15 Intel package;
- Linux AMD64 OCI image for Docker, Kubernetes, and Helm deployments.

Every published artifact must come from one immutable Git commit and must pass
the release checks defined here. The project must not use “production-ready” or
equivalent language for a platform whose required evidence is missing.

## Program decomposition

The work is divided into five independently reviewable subprojects. Each gets
its own implementation plan and verification gate.

1. **Release integrity and continuous integration** repairs current packaging,
   adds PR/main validation, and makes tag publication atomic and traceable.
2. **Secure container and orchestrator deployment** publishes an immutable OCI
   image and supplies a production configuration that satisfies the engine's
   production preflight.
3. **Native signing and platform distribution** signs Windows artifacts and
   signs, notarizes, and staples macOS artifacts before publication.
4. **Upgrade and durability qualification** proves supported rolling upgrades,
   backup restoration, partition recovery, disk-full handling, and abrupt
   failure behavior.
5. **Security assurance and certification** enables automated security gates,
   prepares an independent-review package, and generates a release-specific
   certification report.

Subprojects may proceed concurrently only when they do not edit the same files
or publish mutable external state. Final release certification is serialized
and runs against the integrated release commit.

## Current release blockers addressed

The program must close these confirmed gaps:

- `site/pacificdb-logo.png` was removed while packaging, tests, and README files
  still require it, causing CPack to fail.
- Kubernetes and Helm examples run with authentication disabled and omit the
  production environment, TLS/mTLS, Raft TLS, encrypted-storage evidence, and
  secret mounts required by the engine.
- Deployment manifests reference `pacificdb/community:beta`, but the repository
  has no public container publication workflow.
- Backend checks do not run on pull requests or ordinary `main` changes, and
  `main` currently has no branch protection or repository ruleset.
- The production report primarily certifies beta.11 while beta.13 and later
  source states need release-specific evidence.
- Physical power/storage-controller tests, mixed-version upgrades, independent
  security review, Windows signing, and macOS signing/notarization remain open.

## Release integrity and CI design

### Artifact identity

A release version is supplied only by an annotated `vMAJOR.MINOR.PATCH` or
explicit prerelease tag. All native packages, the OCI image, SBOMs, checksums,
attestations, and certification evidence embed the same version and Git commit.
Publication must fail if generated metadata disagrees.

The packaging logo will use one canonical source under `site/assets/`. The
installed filename remains `pacificdb-logo.png` for compatibility. Repository
README links and package assertions will reference the canonical source or the
stable installed filename as appropriate.

### CI tiers

- **Pull request:** source build, retained unit tests, SDK tests, static release
  consistency checks, Helm rendering, Kubernetes schema checks, container
  build, and package construction without publication.
- **Main:** the pull-request tier plus installed-package smoke tests and an OCI
  image smoke test. Artifacts are retained for diagnosis but not published as a
  stable release.
- **Scheduled/manual qualification:** RF3 load, TCP partition, disk-full,
  restart, backup/restore, and mixed-version upgrade suites.
- **Release tag:** all supported native platform builds and installed P0 suites,
  required signatures/notarization, OCI publication by immutable digest,
  checksums, SBOMs, attestations, and release-specific certification. Any
  failed, cancelled, skipped, or unavailable required job prevents publication.

GitHub configuration outside the repository must require the pull-request
workflow on `main`, require review, dismiss stale approvals, block force pushes,
and prevent direct deletion. Repository rules, Dependabot alerts, secret
scanning, push protection, and CodeQL must be enabled when supported by the
repository plan.

## Container and deployment design

### OCI publication

The canonical image repository is
`ghcr.io/hitesh-reddy-k/pacificdb-community`. Stable and prerelease tags are
immutable release aliases backed by a recorded digest. Production examples pin
the digest; `latest` and `beta` are convenience aliases only and never appear in
production manifests.

The runtime image remains non-root, has a read-only root filesystem where the
platform permits it, and writes only to declared data, backup, restore, log, and
temporary mounts. The image receives OCI source, revision, version, license,
SBOM, and provenance metadata.

### Production Helm profile

Development defaults remain safe for local-only use and are labelled
non-production. A separate production values file must:

- set `PACIFICDB_ENVIRONMENT=production`;
- enable client TLS, client-certificate verification, authentication, RBAC,
  audit logging, synchronous Raft, and Raft TLS;
- mount client and Raft certificates from Kubernetes Secrets;
- mount a reviewed at-rest-encryption evidence document matching `DATA_ROOT`;
- bind client and Raft listeners to the Pod IP rather than a wildcard address;
- require three replicas with quorum two;
- use pod anti-affinity or topology spread constraints;
- define a PodDisruptionBudget that preserves quorum;
- use application-aware startup, readiness, and liveness checks;
- define explicit CPU, memory, storage class, and volume sizes;
- keep backups on a separately managed destination or document the required
  external backup export workflow.

Templates fail rendering when required production secret names, image digest,
storage class, or encryption-evidence configuration is absent. Secret material
is never committed or printed by tests.

The raw Kubernetes example either consumes the same production contract or is
clearly marked as development-only to avoid two divergent production paths.

## Native signing and distribution design

### Windows

The tag workflow requires an Authenticode certificate and password supplied by
GitHub Actions secrets. It signs and verifies `db_engine.exe`, `pacificdb.exe`,
and the final NSIS installer with a trusted timestamp. Publication fails when
credentials are absent, signature verification fails, or the installed files
do not retain valid signatures.

### macOS

The tag workflow imports an ephemeral Developer ID Installer identity, builds
the `.pkg`, signs it with `productsign`, submits it using `notarytool`, waits for
acceptance, staples the ticket, and verifies Gatekeeper assessment. Secrets and
temporary keychains are deleted in an unconditional cleanup step. Publication
fails if credentials are absent or any signing, notarization, stapling, or
assessment step fails.

### Linux

The Debian package, checksum manifest, SBOM, and provenance are attached to the
release. The release workflow verifies the package through `apt`, checks data
preservation on removal, and verifies artifact checksums after downloading the
published release.

## Upgrade and durability design

The supported first stable upgrade path is the latest published beta.14 to the
new stable candidate. An RF3 harness upgrades one follower at a time, waits for
catch-up and health, transfers leadership away from the old leader, upgrades
the final node, and validates acknowledged writes and replica convergence
throughout.

The harness must check:

- old and new nodes reject incompatible protocol or storage versions safely;
- supported mixed-version members preserve quorum and continue bounded traffic;
- every acknowledged identifier exists after full convergence and restart;
- backup creation before upgrade, verification, export, and isolated restore;
- documented rollback behavior before and after an irreversible storage-format
  transition;
- interrupted upgrade recovery and resumption;
- disk-full and network-partition behavior during the supported mixed-version
  window.

Production runbooks document preflight, backup, upgrade, rollback limits,
certificate rotation, node replacement, loss of quorum, restore, and evidence
collection.

Physical power-loss certification remains an external hardware gate. The
repository supplies a harness contract and machine-readable result format but
must report `BLOCKED`, never `PASS`, when a dedicated host, known cache policy,
controllable power source, and post-reboot inspection are unavailable.

## Security assurance design

Repository CI adds CodeQL for C++ and JavaScript, dependency review for pull
requests, pinned GitHub Action revisions, SBOM generation, and secret-pattern
checks that do not expose matches. GitHub-native Dependabot, secret scanning,
and push protection are enabled as repository settings.

The independent review package contains:

- threat boundaries and production invariants;
- supported configurations and excluded features;
- authentication, authorization, tenant-isolation, TLS, Raft TLS, backup,
  parser, path, and resource-exhaustion test evidence;
- exact source commit, dependency inventory, compiler settings, and artifacts;
- a private disclosure path and remediation SLA.

Independent review is complete only when a named reviewer provides a dated
report, all critical/high findings are resolved or explicitly accepted by the
release owner, and fixes have regression evidence.

## Certification and release decision

Certification is generated per release and contains direct links or digests for
every required result. Each gate is `PASS`, `FAIL`, or `BLOCKED`; missing
evidence cannot default to pass. A stable release requires:

- package and installed P0 success on Linux AMD64, Windows x64, macOS arm64,
  and macOS x86_64;
- valid required native signatures and notarization;
- published OCI image smoke-tested by digest;
- secure production Helm rendering and an RF3 deployment smoke test;
- successful supported mixed-version upgrade and restore drill;
- current independent security-review acceptance;
- no unresolved critical or high release finding;
- an explicit disposition for physical power/storage-controller evidence.

If physical testing is still blocked, the release may be described only as a
controlled production candidate with that durability limitation prominently
documented. A universal durability claim requires the physical gate to pass.

## Failure handling and recoverability

Release publication uses a draft release until every artifact and evidence file
has been uploaded and verified. Failed runs do not overwrite stable tags or
mutable artifacts. Signing credentials are ephemeral and cleanup runs even on
failure. Deployment changes retain development compatibility while production
values fail closed.

Every migration and release task documents rollback. Source changes are made on
an isolated branch with small commits; no task modifies user data, production
infrastructure, GitHub protection settings, repository secrets, or public
releases without a separate preview and explicit authorization at the point of
the external write.

## Non-goals

- Implementing features explicitly excluded by `COMMUNITY_SCOPE.md`.
- Claiming support for additional CPU architectures in this program.
- Creating or purchasing signing identities on the user's behalf.
- Self-certifying an “independent” security review.
- Simulating physical power loss and reporting it as equivalent evidence.
- Deploying to or modifying a live customer or production cluster.

## Acceptance evidence

The final handoff includes:

- the exact release commit and clean-tree status;
- required workflow URLs and job conclusions;
- artifact names, sizes, SHA-256 digests, signatures, SBOMs, and attestations;
- installed smoke-test evidence for every supported native platform;
- OCI digest and Kubernetes/Helm deployment evidence;
- mixed-version upgrade and backup/restore evidence;
- security-review disposition;
- physical durability result or explicit blocker;
- a release-specific production-certification report with no stale candidate
  language.
