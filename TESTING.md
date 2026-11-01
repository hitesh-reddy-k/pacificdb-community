# Test Pre-release v1.2

Tests must run against a fresh build and isolated data, never production roots.
Every release artifact needs its own identity/hash/install evidence. Historical
1.2.0-dev benchmark/checkpoint logs are retained but do not count as RC installation.
See [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) for current outcomes.

## Core and vector qualification

```sh
cmake -S engine -B build-rc -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.2.0
cmake --build build-rc -j2
ctest --test-dir build-rc --output-on-failure
cmake -S engine -B build-rc-failpoints -DCMAKE_BUILD_TYPE=Release \
  -DPACIFICDB_ENGINE_VERSION=1.2.0 -DPACIFICDB_ENABLE_TEST_FAILPOINTS=ON
cmake --build build-rc-failpoints --target db_engine -j2
python3 scripts/test-vector-checkpoint.py --build build-rc \
  --failpoint-build build-rc-failpoints --expected-version 1.2.0 \
  --evidence /absolute/path/to/empty-rc-evidence
```

The checkpoint runner records each attempted test, failure/timeout, binary hashes
and summary. It covers exercised document/media/auth/RF3/request isolation,
vector/schema/incremental/freshness/typed storage and destructive crash failpoints.
Do not install the failpoint binary as a release engine. A successful build or
component test alone does not establish full production readiness.

## SDK, CLI and Workbench

```sh
npm run test:npm
PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests
mvn -B -q -f sdk/java/pom.xml test
python3 scripts/test-sdk-capability-matrix.py
PACIFICDB_EXPECTED_ENGINE_VERSION=1.2.0 \
PACIFICDB_VECTOR_PREVIEW_ENABLED=1 PACIFICDB_TEST_NATIVE_VECTOR_PREVIEW=1 \
PACIFICDB_TEST_VECTOR_SCHEMA=1 PACIFICDB_TEST_TYPED_VECTORS=1 \
  node scripts/test-cross-sdk-e2e.mjs build-rc
node scripts/test-workbench-e2e.mjs build-rc
node scripts/test-desktop-engine.mjs build-rc
node scripts/test-workbench-desktop-vector-preview.mjs build-rc
npm run test:workbench:browser -- build-rc
PACIFICDB_WORKBENCH_ENGINE="$PWD/build-rc/db_engine" npm run test:workbench:desktop
```

Browser/Electron checks need the appropriate installed browser/runtime/GUI surface.
Tests failing for a missing environment are Blocked, not Passed. The desktop
preview bridge is tested separately from default isolation. The Workbench does
not claim graphical controls for every lifecycle/schema/hybrid SDK operation.

Build/install actual npm tarballs, wheel/sdist and Java JAR/POM before declaring
SDK distribution passed. `scripts/test-staged-sdk.sh STAGING EVIDENCE` verifies
package contents/hashes, installs clean consumers and runs installed tests. Set
`PACIFICDB_TEST_ENGINE_BUILD` to the absolute fresh native build to include live
TCP/TLS/SIGKILL integration. Clean consumers must not resolve source SDK modules.

## Release and security checks

```sh
python3 scripts/test_release_identity.py
python3 scripts/test_prerelease_qualification.py
python3 scripts/test_prerelease_workflow.py
python3 scripts/test-release-consistency.py
python3 scripts/test-build-identity.py
python3 scripts/test-sdk-packages.py
python3 scripts/test-sdk-workflows.py
npm audit --omit=dev
```

The native tenant-auth test exercises bounded persisted lock expiry, authorized
reset, legacy locked rows and limited database access without self-deadlock.
The auth-boundary integration also verifies accepted/rejected tenant password
operations do not disclose credentials through deterministic logs. Keep logs
private and sanitized; security findings belong in private reporting.

Run ASan/UBSan and TSAN in separate supported builds. Actual sanitizer outcomes
are scoped to binaries/tests/hashes, not to all code or other platforms. The
release checklist identifies current execution; previous checkpoint sanitizer
passes are not silently substituted for a newly compiled candidate.

## Interpreting results

Use Passed, Failed, Blocked, Not applicable or Not tested. Record command, platform,
revision/artifact hash, exit status, scope and retained evidence. A skipped test
is not Passed. Known benchmark failures (ANN availability/performance) remain
visible even when reference/recall checks pass. Process SIGKILL recovery is not
physical power-loss evidence. Cross-platform CI must execute installer/runtime
checks before support is marked verified; source targets or emulation are insufficient.
The strict stable-release audit remains separate from scoped preview staging.

## Exact prerelease receipts

`stage-prerelease.py` stages/tests packages and remains BLOCKED by default. After
reviewing the final core, disk-full, SDK, GUI and security receipts, the release
owner supplies a JSON receipt whose revision/version, artifact hashes and local
execution-evidence hashes match the staging directory. Seal it with:

```sh
python3 scripts/qualify-prerelease.py --directory STAGED_DIRECTORY \
  --receipts OWNER_RECEIPTS.json --revision EXACT_40_HEX_COMMIT
python3 scripts/verify-prerelease-staging.py --directory STAGED_DIRECTORY \
  --revision EXACT_40_HEX_COMMIT --require-qualified
```

The receipt format and tamper/missing/error checks are exercised by
`python3 scripts/test_prerelease_receipts.py`. Evidence hashes bind reviewed logs;
they do not replace reading test outcomes. The disk-full gate is mandatory. The
manual publisher downloads the qualified draft's exact assets and checks the tag,
manifest and SHA256SUMS before making it public. No stable/latest promotion occurs.
