# Production release procedure

## Required repository controls

- Pull requests required for `main`. The owner-selected sole-maintainer policy
  requires zero external approvals and resolved review discussions; existing
  approvals are dismissed when new commits are pushed.
- Required checks: `source-linux` and the reusable cross-platform `packages` workflow.
- Force pushes and branch deletion disabled.
- CodeQL, Dependabot alerts, secret scanning, and push protection enabled.

Verify without changing settings:

```sh
scripts/check-repository-controls.sh
```

The checker discovers the default branch and reads its effective rules, including
rulesets. An obsolete branch name, the presence of a ruleset alone, or a successful
API response cannot produce PASS. It reads the explicit review policy from
`.github/repository-controls.json` (independent approval is the default if the
file is absent) and verifies that policy alongside
`source-linux`, all four native package check names, strict status checks,
CodeQL's high-severity gate, deletion/force-push protection and both secret
protections. It is read-only and reports `BLOCKED` for missing controls or access.

The sole-maintainer profile changes the external review count only. Required
pull requests, strict platform checks, security scanning, discussion resolution,
linear history and deletion/force-push protections remain enforced. Independent
security assessment and artifact qualification remain separate release gates.
Re-run the checker for current effective settings.

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

## Native signing prerequisites

Platform signing is required by default. The release owner explicitly permits
unsigned native installers for `v1.0.0`, `v1.1.1` and `v1.2.0`, and unsigned
Workbench installers for `workbench-v1.1.1` and `workbench-v1.2.0`.
These are exact-tag exceptions; later tags still require signing credentials.
Unsigned evidence records `NOT_APPLICABLE`, never a successful signature check.
Checksums, install/runtime tests and all other release qualification requirements
remain enforced. Non-tag package validation also remains unsigned.

Windows may show an unknown-publisher/SmartScreen warning. macOS packages are
not Apple-notarized and may be blocked by Gatekeeper. State this on the download
page and release notes, and provide checksum verification instructions. Never
instruct users to disable OS protection globally. Maven Central publication is
separate and deferred; downloadable Java JAR/POM packages remain supported.

Windows requires these GitHub Actions secrets:

- `WINDOWS_CERTIFICATE_BASE64`: base64-encoded Authenticode PFX containing the
  code-signing certificate and private key.
- `WINDOWS_CERTIFICATE_PASSWORD`: PFX password.

The workflow signs and timestamps `db_engine.exe`, `pacificdb.exe`, and the
NSIS installer. It verifies each signature with SignTool and verifies both
installed executables with `Get-AuthenticodeSignature`. The temporary PFX is
deleted in a `finally` block.

macOS requires these GitHub Actions secrets:

- `MACOS_INSTALLER_CERTIFICATE_BASE64`: base64-encoded Developer ID Installer
  PKCS#12 certificate and private key.
- `MACOS_INSTALLER_CERTIFICATE_PASSWORD`: PKCS#12 password.
- `MACOS_INSTALLER_IDENTITY`: the exact Developer ID Installer identity.
- `MACOS_KEYCHAIN_PASSWORD`: password for the temporary CI keychain.
- `APPLE_NOTARY_ID`, `APPLE_TEAM_ID`, and `APPLE_APP_SPECIFIC_PASSWORD`:
  notarization credentials.

The workflow imports the certificate into an ephemeral keychain, signs each
package with `productsign`, waits for Apple notarization, staples and validates
the ticket, and requires Gatekeeper acceptance. An `EXIT` trap deletes the
temporary certificate and keychain. Never store credential values in the
repository or release evidence.

Successful static workflow checks do not prove that hosted signing works. The
release owner must configure the secrets, authorize the tag workflow, and
retain its exact-commit evidence before the signing gate can be marked `PASS`.

## External writes

Repository controls, credentials, public tags, packages, images, and releases
must be previewed and explicitly approved before mutation.
