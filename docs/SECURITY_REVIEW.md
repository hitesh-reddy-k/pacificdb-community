# Independent security review gate

Repository automation creates a reproducible review bundle and validates a
third-party result. It does not perform or certify the independent assessment.
Until an external reviewer signs off on the exact candidate revision, the
release gate remains `BLOCKED`.

## Create the bundle

From a clean candidate worktree:

```sh
python3 scripts/security_review.py bundle \
  --output build/security-review \
  --revision "$(git rev-parse HEAD)"
```

Use an empty output directory; earlier evidence is never overwritten. The bundle
contains a Git archive of that revision, SHA-256 digests, dependency and workflow
inputs read from that archive, a CycloneDX-compatible declared-dependency inventory,
and redacted secret-pattern scan results. Current checkout changes cannot alter
the archived revision's dependency evidence. Secret matches record only the file,
line, and pattern name; matched values are never written to evidence.

This is a source-review bundle. Unrelated or stale files in `dist/` are not copied
or qualified. Binary/package review needs a separate exact-artifact manifest and
assessment; this source bundle cannot establish executable production readiness.
The dependency inventory describes declarations, not a complete resolved native
runtime dependency audit.

Provide the reviewer with the threat boundaries and invariants in
`SECURITY.md`, production configuration, compiler settings, test evidence,
unsupported features, disclosure path, and remediation policy. The review must
cover authentication and authorization bypass, tenant isolation, TLS/mTLS,
path containment, parser/message bounds, backup and restore authorization,
secret handling, dependencies, release workflows, and storage/recovery trust
boundaries.

## External result

Place the final report inside the bundle and create a result matching
`schemas/security-review-result.schema.json`. It must name the reviewer and
organization, explicitly attest independence, identify the exact 40-character
revision, hash the report and every artifact, enumerate all findings, and give
every critical/high finding a disposition. `accepted_risk` also requires an
owner and rationale.

Validate it with:

```sh
python3 scripts/security_review.py validate \
  --bundle build/security-review \
  --review-result build/security-review/review-result.json
```

Missing external evidence, self-review, or unresolved critical/high findings
returns `BLOCKED`. Revision, report, or artifact digest mismatches return
`FAIL`. Only a complete independent result tied to the candidate returns
`PASS`.
