# Verified point-cache implementation plan

> **For agentic workers:** Execute sequentially with superpowers:executing-plans. Existing user authorization covers implementation; preserve `v1.2`, uncommitted work, frozen candidates and all attempts. No commit or publication.

**Goal:** Reuse authoritative cached rows safely, reducing native candidate lookup overhead.

**Architecture:** Optional root/collection proofs live in existing striped cache entries. Only complete stable storage reads publish proofs; writer entries remain unverified. Native queries retain authorization, scoring and final source checks.

**Tech Stack:** C++17, existing LSM/cache/freshness primitives, CMake/CTest and public Python benchmark.

**Spec:** `docs/V1.2_VERIFIED_POINT_CACHE.md`.

## Global constraints and review focus

- Durable bytes, WAL/Raft acknowledgements and public signatures stay compatible.
- Bound cache entry count and include proof memory in existing process measurements.
- Verify older replay rows, tombstones, mutations, restore and namespace recreation.
- Preserve strict ANN freshness, tenant and result-validation behavior.
- Keep source/binary identities and partial benchmarks separate; no forced PASS.

## Task 1: Qualify and implement cached authoritative reads

Files: `engine/src/lsm.cpp`, `engine/test/lsm_verified_point_cache_test.cpp`, `engine/CMakeLists.txt`.

- [x] Observe the focused failure against the frozen `ac9f879...` core: writer cache returns an older row over a newer SST.
- [x] Profile forced versus cached point lookups on a stable owned fixture before changing the lookup behavior.
- [x] Add optional proofs to existing cache entries; require a matching proof on authoritative lookup and publish row/proof atomically after existing certificate checks.
- [x] Retain the SST file view under the existing collection read lock while authoritatively validating a cache miss.
- [x] Pass replay, mutation, tombstone, restore/recreate and namespace tests plus the existing 18 point-read interleavings.

## Task 2: Integrate native query reuse and qualify

Files: `engine/src/database_engine_vector_native.cpp`, native cache integration test, checkpoint runner, benchmark and reports.

- [x] Demonstrate a native warm-query reuse gap while checking exact IDs/scores, then remove forced invalidations only where authoritative proof checks remain.
- [x] Expose additive per-query authoritative fetch/cache-hit counts for benchmark transparency.
- [x] Fresh full regression/failpoint/sanitizer/SDK qualification and 1M dirty recovery.
- [x] Run the complete new paired comparison with identical source data/durability, retaining cold/warm round effects, all failures and prior attempts.
- [x] Review combined changes and update documentation/evidence without claiming full release readiness from this checkpoint.
