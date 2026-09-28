# Workbench production readiness — 28 September 2026

**Verdict: local Linux checks pass; production release qualification remains blocked.**
This review fixes the confirmed code blockers below. It does not certify every
possible workload or platform, and no release was published or deployed.

## Confirmed blockers fixed

| Problem | Change | Fresh evidence |
| --- | --- | --- |
| Low-cardinality index creation timed out and blocked schema application. | Replace quadratic duplicate scans with hash sets in the shared index builder used by create, rebuild, and validation. Preserve index format and posting order. | Old 50k fixture failed with `write_not_committed`; fixed 50k fixture passed create/rebuild/validate in 1.4–1.9s. Additional 500k compact-document fixture passed in 10.5s / 8.8s / 13.1s. |
| Malformed HTTP request target stopped the Workbench process. | Include URL parsing and asset reads in the request error boundary. | Child-process regression returns HTTP 400 and subsequently serves the page; all 3 Workbench unit tests pass. |
| Terminal engine configuration could prevent desktop startup or redirect its storage. | Remove inherited engine configuration before setting owned desktop paths and settings; match Windows environment names case-insensitively. | Poisoned inherited paths no longer prevent startup or create external storage. |
| Windows SIGINT forcefully terminated the desktop engine. | Use the bundled native CLI's identity-checked graceful shutdown event, retaining bounded forced cleanup. | Linux shutdown and packaged quit write the clean marker. Windows source reviewed; native Windows execution remains required. |
| The installed Linux app could close immediately when Chromium tried to launch a helper from `/opt/PacificDB Workbench`. | Install under `/opt/PacificDB-Workbench` while keeping the visible launcher name unchanged. | The rebuilt Debian package was installed and passed package verification and a sandboxed installed-app launch with a temporary profile. |
| The release runner could invoke the RF3 upgrade test without required arguments, causing its evidence fallback to overwrite the Node executable. | Require an explicit previous build and only write evidence to an explicit destination. | A regression reproduced the overwrite using a disposable sentinel, then passed after the fix. |
| The release runner invoked operations validation without required paths. | Pass the operations document and alert rules paths. | Operations validation and release-runner unit tests pass. |
| The first Raft health-rate sample could use time since the clock epoch and miss a startup error burst. | Start the sampling clock when Raft starts. | The previously failing health-recovery test passed four consecutive local runs; hosted CI rerun remains required. |

Regression checks are included in the desktop CI workflow. The Community branding
check now distinguishes synthetic customer tier labels from product editions.

## Executed verification

- Full `scripts/test-community.sh` matrix passed against the fresh native build:
  storage, WAL/crash recovery, backups/restores, protocol errors, authenticated
  engine boundaries, media, restart persistence, disk-full recovery, RF3
  replication/partition/integrity, SDKs, deployment contracts, and site checks.
- The same full matrix passed again after the Raft health-clock fix. The exact
  published v1.0.0 engine then passed the mixed-version RF3 qualification
  against the rebuilt 1.0.1 engine: nine steps and 29 acknowledged IDs.
- Core suite: 58 tests passed. npm: 15 SDK and 18 CLI tests passed.
- Release tooling unit tests and contracts passed. Six additional native checks passed
  for engine state, Unicode paths, storage ownership, log redaction, media state,
  and startup delay.
- Browser workflow and real-engine HTTP workflow passed.
- Linux unpacked application and extracted desktop Debian package passed native
  window, sandbox, CRUD, media, clean shutdown, preferences, and restart checks.
- The 28 September desktop rebuild includes PacificDB app icons at standard Linux
  sizes. The Debian launcher resolves the same icon as the window; the existing
  PacificDB wordmark remains on the loading screen. The icon also converts to
  Windows ICO and macOS ICNS. Native Windows/macOS packaging still needs CI.
- Native Debian package smoke test passed; desktop Debian and tar.gz candidates
  were built with checksums. npm audit reported zero advisories.
- The rebuilt Linux desktop Debian and tar.gz checksums verify. Both its
  unpacked app and extracted Debian payload passed the desktop workflow; their
  bundled engine matches the engine used in the mixed-version test byte for byte.
- A local OCI image passed the non-root, read-only-root health and write/read
  smoke test. It is labeled as a dirty development build and has no registry
  digest, so it is not release image evidence.
- RF3 paced smoke test: 64/128 clients, 3,264 operations, zero reported errors.
  This does not certify sustained production capacity.
- Your existing live customer collection still contains exactly 500,000 records.
  Test fixtures used separate temporary databases.

The first optional 500k check exceeded a 15-second assertion intended for 50k;
validation completed in 16.2 seconds. The larger fixture now permits linear
growth, while keeping the SDK's 30-second request timeout. Its repeated full
check passed. The strict 50k regression budget remains unchanged.

[Machine-readable verification and artifact hashes](workbench-production-verification.json)
record the executed checks. The separately generated
[release-policy report](workbench-production-release-gates.json) was generated
without `--run`; its policy gates are not a substitute for these development runs.

## Remaining release requirements

1. Review the committed source and bind release evidence to the final clean
   revision. Two unrelated historical site plans remain untracked in this local
   workspace; they are not part of the Workbench candidate.
2. Run Windows/macOS native package jobs and retain signing/notarization evidence.
   They cannot be certified from this Linux host.
3. Bind the passing mixed-version RF3 evidence to the clean release revision.
   The published v1.0.0 engine was downloaded and its checksums verified, and
   the local upgrade test passed against the rebuilt candidate. Hosted release
   evidence must still identify the exact final artifact and revision.
4. Qualify a registry-pushed candidate OCI digest and Helm/live deployment for
   the intended production environment. Static deployment contracts and a local
   OCI smoke test passed; live deployment qualification was not performed here.
5. Attach the external physical-power and independent-security evidence required
   by [the repository release policy](RELEASE_EVIDENCE.md). The user reports
   these results are on another laptop; they are not present for validation here.
6. Configure Windows code-signing and macOS signing/notarization secrets. The
   repository currently lists only `NPM_TOKEN`; no signing credentials are
   configured, so the release-tag jobs would fail closed.
7. Complete the long-duration load policy gate and bind external/package/container
   results into a clean release report. The current release runner still emits
   those gates as blocked or excluded.

Workbench is a local desktop workspace. These results do not certify exposing
its loopback bridge or unauthenticated desktop engine as a public service.

## Recovered workspace and run command

The USB workspace returned I/O and read-only-filesystem errors during validation.
The complete source and dependencies were recovered to the internal disk:

```sh
cd /home/hitesh/pacificdb-production-recovery-20260927/workspace
npm run workbench:desktop
```

Quit the existing Workbench before launching the recovered copy. Verified native
binaries are installed in its `build/` directory. The app uses your existing
`~/.config/PacificDB Workbench` data; the 500k dataset was not moved or deleted.
Do not use the failing USB filesystem as the authoritative production workspace.

Linux candidate installers and checksums are in `dist/desktop/`; the native engine
package is in `dist/native/`. These are tested local candidates, not published
production releases.
