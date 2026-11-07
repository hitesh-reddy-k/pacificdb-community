# Native ANN lifecycle and short mixed-load evidence

Engine source `337b9ef43300ff27a5c3c663f45b672b8ac3a6b1` passed the stated software checks and the complete corrected short 1M qualification. `summary.json` separates software PASS from production readiness INCOMPLETE. The changed SDKs are identified separately under the idle-sdk evidence folder; the tracked engine tree is identical.

`LOAD_REPORT.json` contains workload, hardware, source/binary identity, timings, resources, independent recall and recovery receipts. It is a single-node paced mixed-load result, not a new PostgreSQL/Qdrant comparison or maximum-throughput result. Original failures, interrupted attempts and authority snapshots remain retained. `failure-chronology.json` discloses relevant failures without private credentials, database payloads or TLS material.

Reproduce normal/checkpoint and dedicated failpoint builds through the development guide. See ../../V1.2_MIXED_LOAD_GUIDE.md for the parameterized disposable Linux profile. The measured latest run reused a backed-up owned corpus; it is not a fresh ingestion benchmark. The initial 32-second index readiness may reuse a derived checkpoint and warm pages and must not be described as fresh graph construction.

ASan/UBSan and TSan logs cover six selected graph/journal/generation targets and linked libraries, not a fully sanitized engine. Native-platform receipts cover engine/CLI/Workbench installer tests, not stable signing/notarization. Thirty minutes cannot establish eight-hour stability, independent security assessment, physical-power qualification or distributed completeness. Published RC.2 packages are unchanged.
