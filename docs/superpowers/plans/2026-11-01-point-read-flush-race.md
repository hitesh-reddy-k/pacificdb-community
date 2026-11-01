# Point lookup during flush: implementation plan

> **For agentic workers:** Use superpowers:executing-plans; execute sequentially in the existing `v1.2` working tree. The user has already authorized implementation and continuation. Do not commit, push or replace existing work.

**Goal:** A point read must not miss a committed value or resurrect a deletion when a flush moves rows between memory and SSTs.

**Architecture:** Reuse the existing authoritative LSM layers and per-collection locks. Reproduce the suspected gap using a controlled flush between read layers, then capture memory before reading SSTs, following `LSM::visitLatest`'s existing safe order. Do not change WAL, Raft acknowledgement or stored formats.

**Tech Stack:** C++17, CMake/CTest, real LSM/SST/WAL fixtures, existing no-op production failpoint interface.

**Spec:** `docs/V1.2_COMMUNITY_SCOPE.md`; original user requirements for correct document/vector version and tombstone visibility.

## Global constraints

- Preserve all uncommitted changes and the frozen measured candidate binary/source.
- Use fresh isolated build directories and owned disposable data roots.
- No global lock, durability relaxation, result truncation or storage migration.
- No claim that the benchmark mismatch is fully explained until a reproduction and corrected recovery check support it.

## Review focus

- A newer committed value moves to a new SST during an ID-cache miss.
- A committed tombstone moves during lookup; the prior row must stay deleted.
- A newly inserted ID has no older SST row; lookup must still find it.
- A repaired point lookup must not poison the next cached read.
- Other tenants/collections with the same ID must retain their independent values.

## Task 1: Reproduce and repair the read-layer gap

Files: `engine/test/lsm_point_read_flush_test.cpp`, `engine/src/lsm.cpp`, `engine/CMakeLists.txt`.
Interface: existing `LSM::put`, `LSM::flush`, `LSM::invalidateIdCache`, `LSM::findByField`; a test-owned failpoint runtime schedules a real flush at the layer boundary.

- [x] Add the boundary observation and real-storage regression: persisted version 1, committed memory version 2, clear cache, flush between layers; require version 2 and its vector in both the first and subsequent point reads.
- [x] Build the focused test in `build-v1.2-point-read`; retain the expected pre-fix failure.
- [x] If that failure confirms the gap, capture the selected ID from active/immutable memory under the existing collection lock before scanning SSTs. Retain and compare visible versions across all layers; keep cache/public result behavior compatible.
- [x] Additional confirmed gap: late cache refill overwrites a concurrent update/delete, even after eviction. Reproduce, then guard both point APIs with existing mutation certificates under the existing collection lock. All 14 focused cases pass.
- [ ] Run the focused cases for replacement, insert, tombstone and tenant separation; then existing storage/exact/native index tests and complete CTest.

## Task 2: Check recovery and benchmark evidence

Files: benchmark helpers, v1.2 architecture/validation/release documentation, new revision-bound evidence.

- [ ] Assert actual `updated` responses and save per-phase/changed-record diagnostics before crash so a future failure cannot erase mutation evidence.
- [ ] Run targeted public dirty-restart and mixed-read checks, then the existing full checkpoint with the separate failpoint build and appropriate sanitizers.
- [ ] Keep original benchmark FAIL, mixed-read availability FAIL, and measured binary hashes explicit. Only rerun timings after correctness qualification; never attribute old timings to a new binary.
- [ ] Publish local review artifacts and an explicit remaining-gates report; do not claim production readiness or publish a release.
