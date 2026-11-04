# Cold filtered exact vector queries

Status: PASS for the measured Linux x64 development workload and checkpoint scope.
Production readiness: INCOMPLETE. This is an unreleased follow-up to the warm
metadata-index execution optimization. Published RC.2 packages are unchanged.

## Implementation and compatibility

Filtered merged scans evaluate tenant, modality and filter fields on the resolved
latest row before decoding the full visibility projection. Version resolution
and tombstone selection precede predicate evaluation. Eligible rows still pass
commit visibility, the request's read context, the full predicate and exact
ranking. OR/NOT/dotted filters continue to scan the complete eligible source;
there is no arbitrary candidate prefix.

A warm, ready metadata index can still provide a bounded complete equality
candidate set. Candidate records certified by the same merged-read contract,
mutation revision, observed commit index and file-publication generation are
reused from the existing bounded row cache. All misses are resolved in one
ordered storage merge, replacing repeated per-ID SST lookup. Payloads enter
that cache only while their observed mutation and publication proofs still
match. Each API requires its own independent certificate. Revalidating an identical
payload preserves the other API's existing proof; writes and changed payloads
clear it. Ordinary document revalidation therefore does not evict a still-valid
vector proof or force an avoidable full rescan.
Final generation, commit and collection checks reject concurrent source changes
with an explicit freshness error. Snapshot/floor and bounded contexts retain
the existing merged plan.

Optional traversal checks poll cancellation/deadlines during memory snapshot
preparation, SST opening/merging, tombstones, candidate selection and completion.
Existing document/media callers omit the optional callbacks and retain their
previous iterator behavior. No WAL/SST format, write acknowledgement, fsync,
replication or authoritative storage change was introduced.

Candidate count and ID-byte limits remain explicit, with ID storage charged
before allocation. The indexed scan limit continues to count selected candidates;
internal SST traversal is reported separately as `candidate_storage_scanned_rows`
and remains subject to deadline/cancellation. Additional execution fields report
candidate cache hits and storage rows. In the unindexed prefilter path, scan
accounting conservatively includes resolved rows touched before visibility
rejection; a resource-limited query may therefore fail earlier, never return a
partial exact result. Inherited in-memory snapshot and row-cache size behavior
is not a new strict byte-budget guarantee.

## Matched recovered 1M diagnostic

Same recovered GloVe25/cosine/top10 fixture: 1M original rows, 999,995 survivors,
9,999 rows with group_id=1. The OR filter is group_id=1 OR group_id=101; the
second branch is empty. Both runtimes have the same complete metadata index,
but OR cannot narrow to a mandatory equality. It exercises the same merged
prefilter path used without an eligible metadata index. It is not a separate
collection with physically absent index files.

Three read-only processes run in order: frozen prior executable, candidate,
same frozen prior executable. Each verifies 45 surviving updates, five deletions
and count. Each first issues an equality query with a cold metadata cache, then
two OR warmups and two rounds over ten unique queries, warms the metadata index
through the same public document read, and issues two indexed warmups followed
by two ten-query rounds. Every one of 135 query results, including warmups and
first queries, matches strict and tie-aware independent ground truth.

| Case | Baseline control | Candidate | Observed speedup |
| --- | ---: | ---: | ---: |
| Cold metadata-cache equality (single observation) | 6,061.32 ms | 3,906.40 ms | 1.55x |
| First payload-cold indexed equality (single observation) | 6,247.39 ms | 4,188.93 ms | 1.49x |
| Non-index-eligible OR filter, warm median | 6,144.08 ms | 3,859.61 ms | 1.59x |
| Indexed equality, warm median | 118.82 ms | 70.19 ms | 1.69x |

Warm controls combine the four baseline round medians. First-query controls use
the median of two individual observations, one per baseline process. These cold
samples describe application caches; count/verification and the OS page cache
have already touched storage. They do not simulate a physically cold disk or
establish p95/p99. Four server/two client logical CPUs, the same temporary
performance-profile hold and unchanged native durability/query-budget settings
were used. The host is an Intel Core i7-10850H with 15.23 GiB RAM running Linux x86_64.
Affinity selects logical CPUs and does not guarantee physical-core/SMT isolation.
Original host power selection is restored after each process. Local
TCP benchmark authentication/TLS is disabled; authenticated SDK checks run
separately. Shared host activity, dynamic clocks, fixed order, repeated queries,
25 dimensions and one fixture limit generalization.

This is not a fresh pgvector/Qdrant, ingestion, ANN build, recovery or mixed-write
comparison. Earlier three-database measurements remain tied to their earlier
executable hashes and cannot be relabeled as measurements of this runtime.

## Verification and remaining limits

The new real-storage test covers version/tombstone ordering, cache/miss parity,
point-cache contamination by an invisible newer SST version, flush-driven
visibility changes, mid-traversal SST and memory cancellation, candidate byte limits, and cross-API document revalidation without losing a
valid vector proof. The pending-version test failed on the earlier implementation
before its correction. A further author-added regression reproduced document
revalidation discarding a valid merged proof; independent proof retention fixes
that interaction. Existing metadata tests continue to cover exact metric
scores, filters, tenant/read contexts and paused mutation/publication races.

All 82 checkpoint checks passed, including CTest, document/media/auth, RF3, restart, request-isolation and crash scenarios. Separate production and failpoint builds passed. Python/Java integration through the Node fixture passed authenticated TCP/TLS and SIGKILL recovery. ASan/UBSan and ThreadSanitizer passed with LSM, query, storage-format and cold-filter test translation units instrumented; linked dependencies do not constitute a fully sanitized engine.


Independent static review found cache-contract, cancellation and file-publication
proof gaps. They have targeted regressions and author-verified corrections.
The reviewer's final pass was interrupted after its third finding; a complete
independent clean re-review remains unexecuted. No clean-review PASS is claimed.

The large synchronous metadata-index creation ACK failure retained in the prior
checkpoint remains unresolved. Cold loading of that metadata-index cache remains
ineligible, and exact fallback still scans storage. Broader high-dimensional,
large-payload, sustained multi-tenant, platform and distributed qualification
remain INCOMPLETE. No general production-readiness or pgvector win is claimed.

[Measurements, sources, checksums and receipts](README.md).
