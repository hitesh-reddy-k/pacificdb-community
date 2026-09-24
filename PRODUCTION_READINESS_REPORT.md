# PacificDB v1.0.1 source readiness report

**Status:** The fixes pass the local test matrix. This source checkout has not
been published as a v1.0.1 npm package, installer, or container image. Test
results establish the behavior below; they cannot prove that every possible
bug is absent.

## What changed

| Area | Fix or addition |
| --- | --- |
| Storage and recovery | Prevented an acknowledged update or deletion from losing to an older LSM version after restart; serialized batch WAL and memtable writes; recovered a torn, uncommitted Raft log tail while still rejecting committed corruption. |
| Catalog and media | Enforced project ownership when mapping or dropping databases, kept media files within their collection, reported media progress, and paged orphan-chunk reconciliation so it can finish under a small result limit. |
| Server security | Made role checks fail closed, restricted leader-term observation, redacted configured secrets from diagnostics, and rejected invalid bind hosts at startup. |
| Clients | Fixed split UTF-8 response decoding and connection reuse in Node, made media downloads replace the destination only after verification, sent the project ID when creating a database, and enabled Java TLS hostname verification. |
| Deployment | Enabled authentication in the Helm and Kubernetes defaults and pinned the existing published image by digest. |

## Verification performed

| Check | Result |
| --- | --- |
| `BUILD_JOBS=4 scripts/test-community.sh build-release-integrity` | **Passed.** Covers native engine tests, npm CLI and SDK tests, Python and Java tests, project and document contracts, restart and disk-full recovery, RF3 replication and partitions, authentication boundaries, deployment contracts, and site checks. |
| `PACIFICDB_P0_MEDIA_100MB=1 node scripts/test-p0-media.mjs build-release-integrity` | **Passed:** 100 MiB maximum case and abrupt restart. |
| `node scripts/test-p0-media-crash.mjs build-p0-failpoints` | **Passed:** four media finalization crash points in a separate failpoint-enabled build. |
| `scripts/test-helm-deployment.sh` | **Passed:** Helm lint/render and Kubernetes schema validation. |
| RF3 sustained fixture in the main suite | **Passed:** 64 and 128 paced clients for 10 seconds each, 3,264 total operations, zero reported errors. This is a smoke test, not a maximum-capacity benchmark. |

## Remaining flaws and limits

1. **Published artifacts are behind this checkout.** The pinned container digest
   is the previously published v1.0.0 image and does not contain these fixes.
   This work did not publish npm packages, create installers or images, or deploy
   the new source. Release users will not get the fixes until new
   artifacts are built and published.
2. **Some validation remains.** The test matrix did not run a live Kubernetes
   deployment, Windows or macOS runtime tests,
   or an RF3 mixed-version upgrade against an exact prior build. The main suite
   explicitly skipped the mixed-version run because `PACIFICDB_OLD_BUILD` was
   not supplied.
3. **Capacity is not certified by the smoke test.** The 64/128-client RF3 runs
   lasted 10 seconds and used paced traffic. Longer load, storage growth, and
   failure-injection runs are needed before claiming a production capacity
   limit.

## Run and test this checkout

With Node.js and the engine's build dependencies installed (including OpenSSL
development files; `sudo apt install libssl-dev` on Debian or Ubuntu):

```sh
npm install --ignore-scripts
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.0.1
cmake --build build -j2
./build/pacificdb --version
./build/db_engine --version
node cli/bin/pacificdb.js --version
```

Each version command should report `1.0.1`. Start the native terminal CLI with
`./build/pacificdb`, or the npm terminal CLI with
`PATH="$PWD/build:$PATH" node cli/bin/pacificdb.js shell`. In either shell:

```text
create project demo
use project project_...
create database app
use app
create collection users
insert users {"id":"1","name":"Ada"}
find users {"id":"1"}
```

Replace `project_...` with the ID printed by `create project`. Stop the local
engine with `./build/pacificdb stop`.

Run `npm test --workspace @pacificdb/cli` for focused CLI tests, or
`scripts/test-community.sh build` for the full repository matrix. The published
packages were still at 1.0.0 when this report was checked; testing the 1.0.1
source requires this checkout.
