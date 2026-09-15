# Secure Container and Orchestrator Deployment Implementation Plan

> Execute this plan in the existing `codex/production-readiness` worktree. Do not push images, tags, releases, secrets, or cluster resources without an exact preview and explicit approval.

**Goal:** Produce a non-root, immutable PacificDB OCI image contract and a fail-closed RF3 Helm production profile that satisfies the engine's production preflight without committing secrets.

**Architecture:** Development deployments remain explicitly non-production. Production is enabled only through a separate values file and requires an image digest, client/health and Raft TLS Secrets, bootstrap-credential Secret, encrypted-volume evidence ConfigMap, explicit storage classes, RF3 quorum, topology protection, separate backup storage, and application-aware mTLS probes. The release workflow builds and smoke-tests one Linux AMD64 image per commit; release aliases are created only in the final publication job from the verified digest.

**Tooling:** Docker/BuildKit, GitHub Actions, Helm 3, kubeconform, Python 3, Bash, Kubernetes apps/v1 and policy/v1 APIs.

---

## Task 1: Define executable deployment contracts

**Files:**

- Create: `scripts/test-deployment-contract.py`
- Create: `scripts/fixtures/helm-production-values.yaml`
- Modify: `scripts/test-community.sh`

1. Add failing assertions for the production values file, digest-only production image, required secret/config references, Pod-IP binding, RF3/quorum, PDB, topology spread, read-only root filesystem, separate data/backup claims, and application-aware probes.
2. Assert development defaults are labelled non-production and the raw manifest is explicitly development-only.
3. Add the check to `scripts/test-community.sh` next to the other release-policy checks.
4. Run the test and capture the expected RED result before implementation.

## Task 2: Implement the fail-closed production Helm profile

**Files:**

- Modify: `deploy/helm/pacificdb/values.yaml`
- Create: `deploy/helm/pacificdb/values-production.yaml`
- Create: `deploy/helm/pacificdb/templates/_helpers.tpl`
- Modify: `deploy/helm/pacificdb/templates/engine.yaml`
- Create: `deploy/helm/pacificdb/templates/poddisruptionbudget.yaml`
- Create: `deploy/helm/pacificdb/templates/networkpolicy.yaml`
- Create: `deploy/helm/pacificdb/README.md`

1. Keep the default profile usable only for local development and clearly label it as such.
2. In production mode, fail Helm rendering unless `replicaCount` is 3, the image has a `sha256:` digest, all required Secret/ConfigMap names are non-empty, and data/backup storage classes and sizes are explicit.
3. Render `PACIFICDB_ENVIRONMENT=production`, auth/RBAC/audit, client mTLS, Raft TLS, synchronous replication, quorum two, fsync-on-commit, Pod-IP client/Raft binds, and the absolute at-rest evidence path.
4. Mount client TLS (including health client credentials), Raft TLS, bootstrap credentials, root-owned evidence, separate persistent data and backup volumes, and writable restore/log/tmp volumes while keeping the root filesystem read-only.
5. Add mTLS `ping` exec probes, topology spread/anti-affinity, an RF3-safe PDB, termination grace, and a default-deny-plus-required-ports NetworkPolicy.
6. Render the fixture with Helm and validate all objects with kubeconform; prove missing digest/secret/storage inputs fail.

## Task 3: Remove ambiguity from raw Kubernetes examples

**Files:**

- Modify: `deploy/kubernetes/engine-statefulset.yaml`
- Create: `deploy/kubernetes/README.md`
- Modify: `README.md`

1. Mark the raw manifest development-only in both machine-visible annotations and documentation.
2. Replace the nonexistent Docker Hub image reference with the canonical GHCR repository while retaining a non-production convenience tag.
3. Direct production operators to the Helm production contract and list the externally supplied TLS, bootstrap, encrypted-volume evidence, storage, and digest prerequisites.
4. Extend deployment-contract tests so a raw manifest cannot silently become a second production path.

## Task 4: Harden and smoke-test the OCI image

**Files:**

- Modify: `deploy/docker/Dockerfile`
- Create: `deploy/docker/healthcheck.sh`
- Create: `scripts/test-container-image.sh`
- Modify: `.dockerignore`

1. Add OCI source, revision, version, license, and created labels; build both `db_engine` and the native `pacificdb` client.
2. Retain UID/GID 10001, drop to non-root, expose only declared ports, create declared writable directories, and make the image compatible with a read-only root filesystem.
3. Add an mTLS-capable health helper that performs an application `ping`; development mode may use plaintext, production mode must use CA/client certificate verification.
4. Build locally with immutable version/revision inputs and run a smoke script that checks labels, non-root identity, read-only-root compatibility, startup, ping, write/read, and clean shutdown.
5. If the Docker daemon is unavailable, report the runtime check as BLOCKED and rely only on Dockerfile/static contract checks until CI runs it.

## Task 5: Add digest-first image CI and release integration

**Files:**

- Modify: `.github/workflows/release.yml`
- Modify: `scripts/test-workflow-contract.py`
- Modify: `scripts/verify-release-artifacts.py`
- Modify: `scripts/test-verify-release-artifacts.py`
- Modify: `docs/PRODUCTION_RELEASE.md`

1. Add a Linux AMD64 container job to the reusable workflow. PR/main calls build and smoke-test without publication.
2. On a release tag, authenticate to GHCR, build with SBOM and provenance, push by digest without a release alias, smoke-test the exact digest, and upload `p0-evidence-linux-amd64-container.json`.
3. Make the final release job depend on the container job, verify the evidence file with native artifacts, then create the version image alias from the recorded digest immediately before undrafting the GitHub release.
4. Grant only job-scoped `packages: write`; never forward repository secrets to PR/main reusable calls.
5. Extend local workflow and artifact-verifier tests for digest-first publication, evidence shape, and the new required file.

## Task 6: Run the integrated container/deployment gate

**Files:** Verify only unless a failing check identifies an in-scope defect and a regression test is added first.

1. Run release, workflow, artifact, and deployment contract tests.
2. Run `helm lint` and render both default and production fixtures; validate rendered YAML with kubeconform.
3. Build and smoke-test the OCI image when a permitted daemon is available; otherwise record the exact daemon blocker.
4. Run `scripts/test-community.sh build-secure-deployment` with Node 22 and JDK 21.
5. Inspect the complete branch diff and clean-tree state. Keep live registry publication, cluster deployment, and GitHub workflow execution explicitly deferred until approved.

## Acceptance boundary

Local completion proves source contracts, rendering, schema validation, and any available local image runtime smoke. It does not prove GHCR publication, hosted-runner execution, or a live RF3 Kubernetes deployment. Those stay `BLOCKED` until their external evidence exists.
