# Release evidence

PacificDB release decisions are bound to one Git revision and the artifacts
hashed from that revision. Generate a development report with:

```sh
python3 scripts/release_qualification.py \
  --version 1.0.0 \
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
