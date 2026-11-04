# Cold filtered exact queries: recovered 1M diagnostic

PASS for the stated development workload and checkpoint scope.
Production readiness: INCOMPLETE. Published packages unchanged.

See [implementation, measurements, compatibility and limitations](IMPLEMENTATION.md).
comparison.json contains cold/OR/warm timings for baseline/candidate/baseline.
All 135 queries including warmups match strict and tie-aware exact ground truth.
Both executables use the same recovered 1M GloVe25 fixture and complete metadata
index. The OR predicate is not index-eligible; the indexed phase uses the same
public document read to warm the metadata cache. Application payload-cold timing
is not a physically cold disk measurement. No p95/p99 conclusion is supported.

82 checkpoint checks, separate production/failpoint builds, authenticated SDK
TCP/TLS/SIGKILL and scoped ASan/UBSan/TSAN checks passed. Independent static
review found three material gaps; targeted regressions and corrections address
them, but an account-limited final re-review was not completed. No independent
clean-review PASS is claimed. Large-index creation ACK failure retained in the
previous checkpoint is still unresolved. No new index creation was attempted.

Prior pgvector/Qdrant/ANN/ingestion/recovery numbers belong to prior executables;
they are not a comparison of this runtime. Shared host activity, dynamic clocks,
fixed execution order, repeated ten-query rounds and low dimension limit claims.

Compressed raw results include per-query IDs/recall, execution metadata, warmups,
resource samples and process receipts. Sources, input manifest, code fingerprints
and provenance identify the tested revision. Private raw logs/failures are kept
locally with hashes; no credentials, customer records or security-review reports
are distributed. No commit, tag or package was published by this checkpoint.

To reproduce, apply a source patch to provenance.json's base commit, build in
separate production/failpoint directories, and run the checkpoint runner with
expected version 1.2.0. Measurement scripts require a disposable recovered fixture
matching the prior trial's documented mutations/deletions and existing complete
metadata index. Replace $REPOSITORY/$PUBLICATION/$NODE_PREFIX with your isolated
paths and retain containment checks. Never point these fixtures at live data.
Verify the distributed files with SHA256SUMS.
