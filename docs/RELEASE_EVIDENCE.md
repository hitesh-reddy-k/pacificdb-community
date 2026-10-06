# Release evidence

PacificDB release decisions are bound to one Git revision and the artifacts
hashed from that revision. Generate a development report with:

```sh
python3 scripts/release_qualification.py \
  --version 1.0.1 \
  --output build/release-evidence.json \
  --allow-dirty-development
```

Add `--run` only when the host is intended to execute every locally available
gate. Command output is not copied into the report, which prevents credentials
or application data from entering release evidence. Referenced artifacts must
be regular files inside the repository and are recorded by repository-relative
path, byte length, and SHA-256 digest.

## Status meanings

- `PASS`: the named gate ran successfully against the recorded revision.
- `FAIL`: the gate ran and found an error. Any failure fails the decision.
- `BLOCKED`: required evidence or tooling is absent. A required blocked gate
  prevents a stable release.
- `EXCLUDED`: the release owner deliberately removed an optional gate from this
  program. The long-duration load gate is always emitted this way with reason
  `release_owner_excluded_2026-09-16`; it is never represented as passing.

`STABLE` requires a clean tree and every recorded gate to pass.
`CONTROLLED_CANDIDATE` may contain optional blocked or excluded evidence, but
never a failed or required blocked gate. A dirty-tree report is development
evidence only: `release_eligible` remains false even when
`--allow-dirty-development` permits the report to be written.

## Required external evidence

Physical power interruption and independent security review are required,
external gates. Repository automation can validate their evidence but cannot
self-certify either result. Until complete external records are attached to the
same revision, both gates remain `BLOCKED`.


## Database-first CLI, Workbench and SDK candidate

The [3 October database-first qualification](evidence/2026-10-03-database-first-package-readiness.md)
records isolated direct-database UX, Java/Python client and local distribution checks,
including independent review findings and their tested corrections. Its
[artifact manifest](evidence/2026-10-03-sdk-package-artifacts.json) identifies exact
clean-source candidate hashes and explicitly test-only signing.

These scoped checks do not waive the required external gates above. No package,
release tag or installer was published or deployed. Registry ownership/protected
publisher setup, supported-platform execution, and release-owner approval remain
separate prerequisites; see [publication instructions](SDK_PUBLISHING.md).

## Workbench installer qualification

Workbench releases use a separate fail-closed report. Each supported runner
records the installer it actually tested:

```sh
python3 scripts/workbench_release_qualification.py platform \
  --name linux-x64 \
  --version 1.1.2 \
  --artifact dist/desktop/PacificDB-Workbench-1.1.2-linux-amd64.deb \
  --output build/workbench-linux-x64.json
```

Valid platform names are `linux-x64`, `windows-x64`, `macos-arm64`, and
`macos-x64`. Aggregate all four results with the exact installer files and the
external evidence bound to the same clean Git revision:

```sh
python3 scripts/workbench_release_qualification.py aggregate \
  --version 1.1.2 \
  --artifact dist/desktop/PacificDB-Workbench-1.1.2-linux-amd64.deb \
  --artifact dist/desktop/PacificDB-Workbench-1.1.2-win-x64.exe \
  --artifact dist/desktop/PacificDB-Workbench-1.1.2-mac-arm64.dmg \
  --artifact dist/desktop/PacificDB-Workbench-1.1.2-mac-x64.dmg \
  --platform-result build/workbench-linux-x64.json \
  --platform-result build/workbench-windows-x64.json \
  --platform-result build/workbench-macos-arm64.json \
  --platform-result build/workbench-macos-x64.json \
  --physical-power-evidence build/physical-power-result.json \
  --security-bundle build/security-review \
  --security-review build/security-review/review-result.json \
  --load-evidence build/workbench-load-result.json \
  --audit-report build/npm-audit.json \
  --output build/workbench-release-evidence.json
```

The load record must satisfy
[`workbench-load-result.schema.json`](schemas/workbench-load-result.schema.json):
at least eight hours, 500,000 records, one operation, zero errors, and a
positive peak resident-memory measurement. The runtime audit report is the JSON
output of `npm audit --omit=dev --json` and must contain zero vulnerabilities.
The security bundle and review must include all four candidate installer
digests, not only the source revision.

`PASS` requires a clean source tree, all four exact installer digests, valid
physical power-loss and independent security-review evidence, the load
threshold, and a clean runtime dependency audit. Missing evidence is
`BLOCKED`; conflicting revisions, digests, failed external evidence, load
errors, or runtime advisories are `FAIL`. Neither status may be published as a
stable Workbench release.

If the first tag run blocks for missing external evidence, retain its workflow
artifacts, attach evidence for those exact digests, then dispatch the workflow
with `release_tag` set to that immutable tag. The retry skips package builds and
downloads the original tag-run installers; rebuilding signed installers would
change their bytes and invalidate the review.
