# Company evaluation — Pre-release v1.2, RC.2

Release: [v1.2.0-rc.2](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/v1.2.0-rc.2).
Engine source: `e3d70761f03341acc13130d17b75fed2bc758a29`.
Base engine version 1.2.0; npm/Java 1.2.0-rc.2; Python 1.2.0rc2.
This is a company-testing prerelease. Production readiness remains INCOMPLETE.

## Choose an installer

Download the matching file plus `SHA256SUMS` and `RELEASE-MANIFEST.json` from the
release. No C++ build is needed. Each desktop installer includes the engine and
native CLI; installing an SDK alone does not install those executables.

| Verified package target | Release asset |
| --- | --- |
| Ubuntu24.04 Linux x64 desktop | `PacificDB-Workbench-1.2.0-rc.2-linux-amd64.deb` |
| Windows x64 desktop | `PacificDB-Workbench-1.2.0-rc.2-win-x64.exe` |
| macOS15 Apple Silicon | `PacificDB-Workbench-1.2.0-rc.2-mac-arm64.dmg` |
| macOS15 Intel | `PacificDB-Workbench-1.2.0-rc.2-mac-x64.dmg` |
| Ubuntu24.04 Linux x64 engine/native CLI | `pacificdb-community-1.2.0-rc.2-linux-x64.deb` or matching archive |

Other Linux distributions/architectures and OS versions are not certified by
this matrix. The Ubuntu native packages require Ubuntu's runtime ABI; do not
substitute them as Debian13/RHEL packages. The Linux x64 container provides a
separate installation route on compatible container hosts. Its verified digest is
`ghcr.io/hitesh-reddy-k/pacificdb-community@sha256:bda3cc787d33ff93d34f5545d9c4bd77dcc3f25d051fd8eda4c954733438ad26`.

Verify the selected file's SHA-256 against its exact filename in `SHA256SUMS`.
On Linux use `sha256sum FILENAME`; on macOS use `shasum -a 256 FILENAME`; on
Windows use PowerShell `Get-FileHash -Algorithm SHA256 .\FILENAME`. Checking a
complete checksum inventory with only one file downloaded also reports missing
files; that is different from a selected-file hash mismatch.

On Ubuntu install with
`sudo apt install ./PacificDB-Workbench-1.2.0-rc.2-linux-amd64.deb`.
On Windows run the NSIS installer, and on macOS mount the DMG and copy
`PacificDB Workbench.app` to Applications. Start the installed Workbench. Use a
graphical session and a new, disposable evaluation data directory.

Stable Windows signing and macOS Developer ID/notarization/browser quarantine
qualification are not asserted. Managed-device endpoint policy may require IT
approval. CI download/install tests do not prove every organization's browser
download or endpoint-security policy will permit the app.

## First document/media test

1. Confirm the displayed prerelease identity includes 1.2.0-rc.2 and that the
   engine is Connected.
2. Create a database, then a collection; insert a document with a chosen ID and
   retrieve/update/delete it.
3. Upload a small non-sensitive file, download it and compare its SHA-256.
4. Quit the app completely, reopen it and verify the remaining records/media.
5. In the File menu select **Copy CLI connection command**. This gives the
   correct bundled CLI path and the current local engine port. Run that command
   while the Workbench is open. The native shell uses PacificDB commands, not SQL.

RC.2 fixes RC.1's collection-button overlap in short windows. The sidebar should
scroll and keep the database tree reachable. Test this on a small display too.

Use the connection options in Workbench for the actual SDK URL. Do not assume
the desktop engine uses port 9000. Local default-off authentication is a
development/evaluation configuration; do not expose it to an untrusted network.
Authentication/TLS configuration has separate documentation and test evidence.

## Install language packages

Download the exact SDK archives from this release and verify checksums. These
commands use packages, not repository source imports:

```sh
npm install ./pacificdb-client-1.2.0-rc.2.tgz ./pacificdb-cli-1.2.0-rc.2.tgz
./node_modules/.bin/pacificdb --version
python3 -m venv .venv
.venv/bin/python -m pip install ./pacificdb-1.2.0rc2-py3-none-any.whl
mvn org.apache.maven.plugins:maven-install-plugin:3.1.2:install-file \
  -Dfile=pacificdb-client-1.2.0-rc.2.jar \
  -DpomFile=pacificdb-client-1.2.0-rc.2.pom
```

Windows uses `.venv\Scripts\python.exe`. Java requires Java11+ and Maven;
its POM resolves Jackson dependencies from Maven Central. Local JAR installation
does not publish the candidate to Central. The SDK guide contains the tested
Node/Python/Java CRUD examples, TLS and error-handling instructions.

The npm client/CLI are now public and independently installed/tested. You can use
`npm install @pacificdb/client@1.2.0-rc.2 @pacificdb/cli@1.2.0-rc.2` instead of
the local tarballs. Avoid `@latest` for this prerelease. The initial check caught
a registry propagation delay for the CLI; its successful retry and original
failure are retained in run41324080656.
**PyPI and Maven Central are BLOCKED pending owner publisher/namespace/signing
setup**, as confirmed by the owner. GitHub wheel/JAR/POM installation remains
available; do not substitute older registry versions. Owner instructions are in
`PUBLISHING_OWNER_GUIDE.md`.

## Evaluate vectors deliberately

Native vector/index/hybrid features remain an explicit standalone preview.
Quit the desktop app before setting `PACIFICDB_WORKBENCH_VECTOR_PREVIEW=1`
and launching its actual executable. For a separately managed engine use
`PACIFICDB_VECTOR_PREVIEW_ENABLED=1` before startup. Existing running processes
do not acquire environment changes from a new terminal.

Follow `docs/VECTORS.md` from the current documentation archive for
dimension/metric schemas, dense/sparse/named CRUD, explicit exact/ANN modes,
index creation/status/rebuild and hybrid requests. Schema/index administration
and hybrid operations use the SDK/raw operations; every function does not have a
graphical control. Reject malformed/mismatched vectors and verify that the
expected authorization scope is enforced.

Wait for index READY before requiring ANN. Index construction is a setup cost,
not every query. Index maintenance is asynchronous; strict ANN searches can
return explicit unavailability during mutations. After restart graph definitions
remain, but graph reconstruction is explicit and can take many minutes at 1M.
Authoritative records stay in WAL/LSM. Do not interpret ANN unavailability as
lost records or silently treat incomplete ANN results as exact results.

Distributed vector completeness, persisted graph snapshots, advanced compression/
disk ANN/automatic maintenance and full production qualification remain incomplete.
Use the native vector readiness documents for exact supported scopes.

## Benchmark evidence and feedback

The published RC.2 project-run comparison completed at10K/100K/1M against
PostgreSQL18.6 + pgvector0.8.7, plus smaller dimensional/metric and sparse/named/
hybrid diagnostics. [Final report](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/BENCHMARK_REPORT_1.2.0-rc.2.txt) and
[per-query evidence](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/pacificdb-benchmarks-1.2.0-rc.2-20261101.tar.gz) include all numerical results,
failed strict ANN reads, build/recovery costs and comparison limits. At1M,
strict ANN mixed reads succeeded0/360 versus360/360. Timings vary by host/workload;
execution PASS is not a high-availability or production-readiness PASS.

Report candidate/build-info, artifact hash, OS/CPU/RAM, settings, minimal steps,
expected/actual result and sanitized logs. Include errors and unsuccessful
requests when reporting throughput/latency. Reproduction and feedback formats
are in `COMPANY_TESTING_GUIDE.md`; use private security reporting for sensitive
issues and never attach credentials or customer records to a public issue.
