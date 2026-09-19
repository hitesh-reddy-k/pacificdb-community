# Production release procedure

## Required repository controls

- Pull requests required for `main`, with one approval and stale approvals dismissed.
- Required checks: `source-linux` and the reusable cross-platform `packages` workflow.
- Force pushes and branch deletion disabled.
- CodeQL, Dependabot alerts, secret scanning, and push protection enabled.

Verify without changing settings:

```sh
scripts/check-repository-controls.sh
```

## Candidate qualification

Run `scripts/test-community.sh build`, `scripts/test-helm-deployment.sh`, all
installed P0 package jobs, the OCI digest smoke test, mixed-version upgrade
test, restore drill, and external certification probes. Every result must be
attached to the release commit. Production Helm values must render the exact
candidate image digest; a tag such as `latest` or `beta` is not release
evidence.

After producing the exact candidate artifacts, generate the machine-readable
report described in [RELEASE_EVIDENCE.md](RELEASE_EVIDENCE.md):

```sh
python3 scripts/release_qualification.py \
  --version "$PACIFICDB_RELEASE_VERSION" \
  --output build/release-evidence.json \
  --artifact build/db_engine \
  --run
```

Do not use `--allow-dirty-development` for release evidence. Verify that
`revision` is the reviewed commit, `dirty` is false, `release_eligible` is true,
and every artifact digest matches the object submitted for publication. A
required `FAIL` or `BLOCKED` result stops publication. `EXCLUDED` is a visible
scope decision, not a passing test.

## Tag publication

Create an annotated release tag only from a clean, reviewed commit. The tag
workflow must remain the sole stable publication path. Do not publish or promote
a draft when any required job is failed, cancelled, skipped, or blocked.

The container workflow pushes the Linux AMD64 image by digest first, validates
that exact digest with a read-only-root, non-root runtime smoke test, and records
its version and source revision. The final release job checks that evidence
alongside every native package before creating the version alias in GHCR and
undrafting the GitHub release.

## External writes

Repository controls, credentials, public tags, packages, images, and releases
must be previewed and explicitly approved before mutation.
