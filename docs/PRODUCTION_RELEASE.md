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

Run `scripts/test-community.sh build`, all installed P0 package jobs, the OCI
digest smoke test, mixed-version upgrade test, restore drill, and external
certification probes. Every result must be attached to the release commit.

## Tag publication

Create an annotated release tag only from a clean, reviewed commit. The tag
workflow must remain the sole stable publication path. Do not publish or promote
a draft when any required job is failed, cancelled, skipped, or blocked.

## External writes

Repository controls, credentials, public tags, packages, images, and releases
must be previewed and explicitly approved before mutation.
