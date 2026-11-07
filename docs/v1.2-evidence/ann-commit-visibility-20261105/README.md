# Incremental ANN commit-visibility development evidence

The checks passed for the exact code fingerprints in `source-sha256.json`. The
local working-tree binaries are development artifacts, not a clean stable release.
`summary.json` distinguishes passed checks, expected baseline failures and pending
release gates. Earlier 1M runs do not qualify this new runtime.

The original failed mixed load, baseline API failure, first TSAN fixture warning
and temporary dependency failure are retained locally. Their raw digests are
recorded without publishing host paths, credentials or test payloads. The two
selected sanitizer transcripts are sanitized only for the repository path. No
sanitizer suppression or test assertion was removed.

Reproduce with normal development dependencies installed:

```sh
cmake -S engine -B build-check -DCMAKE_BUILD_TYPE=Release
cmake --build build-check -j2
cmake -S engine -B build-failpoints -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENABLE_TEST_FAILPOINTS=ON
cmake --build build-failpoints --target db_engine -j2
python3 scripts/test-vector-checkpoint.py --build build-check --failpoint-build build-failpoints --expected-version 1.2.0 --evidence /tmp/ann-checkpoint-empty
```

Use an unused evidence directory. For the full Community/SDK paths, enable
`PACIFICDB_VECTOR_PREVIEW_ENABLED=1`, `PACIFICDB_TEST_NATIVE_VECTOR_PREVIEW=1`,
`PACIFICDB_TEST_VECTOR_SCHEMA=1`, and `PACIFICDB_TEST_TYPED_VECTORS=1` when running
`scripts/test-community.sh build-check`. Set `PACIFICDB_OLD_BUILD` to a verified
prior artifact to exercise the mixed-version RF3 gate.
