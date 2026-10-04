# Java/Python database-first SDK qualification — 3 October 2026

SDK file implementation: `643beaf`; authenticated integration harness: `deedd08`.
All code is on local branch `ux/database-first-20261003` in the recovered healthy
worktree. No package was published. Existing project APIs/data remain compatible;
ordinary create-database/collection/document flows require no project.

The shared URL fixtures contain eight valid and 39 invalid cases. Python 3.12.3
passed 125 tests, Java 17 passed 29 tests with declared Java 11 source floor,
and the matrix validated named bindings for all 135 Community dispatches.
Transport tests cover verified TLS/hostname/trust, authentication, frame corruption,
bounded pooling/deadlines, close races, captured scope and no sent-write retry.
File tests cover interruption/resume, changed sources, negotiated request bounds,
base64/checksum/progress/scope validation and verified atomic destination replacement.

`node scripts/test-cross-sdk-e2e.mjs build` passed both language clients against
an authenticated real engine over TCP, then after actual SIGKILL recovery, then
verified TLS. Each client checked every expected field of 160 acknowledged
documents and 700,000 media bytes. Both exercised database/document/index/vector,
media upload/ready-resume/download, backup creation/verification/streamed export/
restore, read-only and revoked API-key refusals, anonymous refusal, legacy project
operations, health and operation status. Temporary restore targets were outside
live data roots. The retained diagnostic root is `/tmp/pacificdb-cross-sdk-4lDvQ2`.

The full Community run executed its engine/SDK/API/storage/recovery/disk-full/RF3
checks and these cross-SDK cases. Its final branding scan initially matched an
inherited Workbench seed-data report, filters and query recipe containing customer tier values;
the scan now excludes those three exact artifacts while retaining product/source
checks. The corrected final-gate result is recorded with package readiness.

Evidence logs are retained under the ignored local
`.superpowers/sdd/2026-10-03-java-python-sdk-capabilities/` directory. The initial
integration assertion incorrectly expected a vector `results` field and was
corrected to the engine's existing `data` field; no protocol response was changed.

Limits: Linux runtime evidence only; Python 3.10/Java 11 floor execution is CI work.
Mixed-version RF3 was skipped because no exact prior released build was supplied.
No physical power-loss simulation, independent security certification, arbitrary
concurrent transaction proof or remote registry acceptance is implied.
