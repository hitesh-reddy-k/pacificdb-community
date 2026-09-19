# Native Signing and Platform Distribution Implementation Plan

> Execute from the dedicated release-preparation worktree. Never create, upload, print, or configure signing credentials. Hosted signing/notarization remains BLOCKED until the release owner supplies repository secrets and authorizes workflow execution.

**Goal:** Make release-tag publication fail closed unless Windows binaries/installer have valid Authenticode signatures and both macOS packages are Developer ID signed, notarized, stapled, and Gatekeeper accepted.

**Architecture:** Non-tag package validation remains unsigned and exercises install/runtime behavior. Release tags require platform secrets, use ephemeral credential files/keychains, clean them in unconditional handlers, verify installed artifacts, and emit machine-readable signing evidence. The final release verifier rejects missing, stale, or non-passing native signing evidence.

---

## Task 1: Add native-signing policy tests

**Files:**

- Create: `scripts/test-native-signing-contract.py`
- Modify: `scripts/test-community.sh`
- Modify: `scripts/test-verify-release-artifacts.py`

1. Assert the release workflow requires Windows secrets on tags, signs both binaries and NSIS installer, verifies installed signatures, and cleans the PFX unconditionally.
2. Assert both macOS jobs require the certificate/notary secrets on tags, use an ephemeral keychain, `productsign`, `notarytool --wait`, `stapler`, and `spctl`, and clean credentials via `trap`.
3. Add failing release-verifier fixtures for missing or non-passing platform signature evidence.
4. Capture RED before workflow/verifier implementation.

## Task 2: Complete Windows Authenticode evidence

**Files:**

- Modify: `.github/workflows/release.yml`

1. Move PFX materialization into a `try/finally` scope that always deletes it.
2. Require certificate/password secrets for tag builds while keeping non-tag construction unsigned.
3. Sign and timestamp `db_engine.exe`, `pacificdb.exe`, and the NSIS installer; verify each with SignTool.
4. After silent installation, verify both installed executable signatures are `Valid`.
5. Add version, revision, installer signature, and installed binary signature results to the Windows evidence JSON.

## Task 3: Add macOS signing, notarization, and stapling

**Files:**

- Modify: `.github/workflows/release.yml`

1. Require the installer certificate, certificate password, installer identity, keychain password, Apple ID, team ID, and app-specific password for release tags.
2. Import the certificate into a temporary keychain and restrict key access to signing tools.
3. Build an unsigned package, sign it with `productsign`, verify it with `pkgutil`, submit it with `notarytool --wait`, require `Accepted`, staple/validate the ticket, and require `spctl --type install` acceptance.
4. Delete the P12 and keychain through an EXIT trap even when any step fails.
5. Record signature, notarization, stapling, Gatekeeper, architecture, version, and revision in each macOS evidence file. Non-tag validation records `NOT_APPLICABLE` and is never accepted by the release verifier.

## Task 4: Enforce signing evidence before release publication

**Files:**

- Modify: `scripts/verify-release-artifacts.py`
- Modify: `scripts/test-verify-release-artifacts.py`
- Modify: `docs/PRODUCTION_RELEASE.md`

1. Parse Windows and both macOS evidence files and require matching version/revision.
2. Require every Windows signature field to be `PASS`; require macOS signing, notarization, stapling, and Gatekeeper fields to be `PASS`.
3. Include signature/notarization summaries in `RELEASE-MANIFEST.json`.
4. Document the exact secret names, certificate types, cleanup behavior, and external prerequisites without including values.

## Task 5: Verify locally without credentials

1. Run native-signing, workflow, release-consistency, and artifact-verifier tests.
2. Run `actionlint` over all workflows and the retained source suite.
3. Confirm workflow files contain no credential values and the worktree is clean.
4. Report Windows hosted signing and macOS signing/notarization as BLOCKED until secrets and explicitly authorized tag execution exist; never infer PASS from source inspection.
