# v1.2 exact vector search using metadata indexes

Status: PASS for the warm indexed execution path and stated qualification scope.
Metadata-index creation acknowledgement: FAIL in the 1M setup attempt.
Production readiness: INCOMPLETE. This follows the scalar metadata decoder
optimization and preserves the earlier unreleased changes.

## Behavior and compatibility

The exact vector query can use a complete, ready existing binary secondary index to enumerate candidates for a mandatory top-level scalar equality. A mandatory equality can occur within AND; OR/NOT and dotted paths alone retain a complete scan. Other predicates, modality and row visibility are evaluated normally on authoritative point-read records. Every candidate is ranked; the document output limit does not bound the ranked set. Numeric values such as integer 1 and float 1.0 can have distinct legacy index keys; the lookup includes all equivalent numeric buckets. String/key type collisions are safe supersets removed by the full predicate.

The index must have no pending updates and its existing cache must already be loaded. Cold or busy caches fall back to the existing scan: the legacy cold loader does not meet the vector request's cancellation/resource contract. Existing document/index operations can populate that cache. No implicit metadata index or new authoritative vector store is created. Users without a suitable warm metadata index keep the previous behavior.

Candidate retention has explicit count and byte limits. Enumeration and ranking poll deadline/cancellation. Candidate lookup and the completed ranking are bound to the same collection/commit certificate. A concurrent source change invalidates the result rather than silently omitting a newly matching row. Historical/floor and bounded read contexts retain the merged scan. Additive execution fields identify `candidate_source`, `metadata_index_field`, candidate count and scanned rows. Exact/ANN mode and fallback policies are unchanged.

## Shared mutation correction

Review identified a single-put publication gap: the mutation freshness guard previously ended when the memtable lock was released, before pending column-index work was registered. A new query could capture a stable new data revision against an older index. The guard now spans the existing put operation through registration or synchronous completion. Collection/index lock ordering, WAL format, write acknowledgement and fsync settings are unchanged. The guard is an atomic certificate-invalidation mechanism, not a global write lock.

A paused-write test at the existing `FP_APPLY_AFTER_BASE_BEFORE_INDEX` reproduces the omission with the old guard scope. The corrected test uses a distinct new ID to avoid confusing an older version with a newer source mutation. A second paused query after candidate enumeration verifies that moving an excluded row into the filter causes an explicit freshness failure. Production failpoints remain disabled.

## Verification and performance scope

New real-storage tests cover late best candidates, exact cosine/L2/dot scores, scalar numeric/string/null behavior, compound filters, OR/NOT/dotted fallback, candidate count/bytes, cancellation, updates/deletes, cross-tenant rows, floor/bounded contexts, compaction and mutation races. Existing suites must run on fresh production and separate failpoint builds.

The planned diagnostic uses the same recovered 1M GloVe25 fixture and an explicitly created `group_id` secondary index. Both the frozen prior executable and candidate see the same index and perform the same existing document-index warmup. Index preparation, disk and warmup costs are recorded separately. Two rounds repeat ten unique filtered queries, with independent ground truth and record/deletion checks. This is not a fresh three-database comparison and does not replace the prior unindexed benchmark cell.

Remaining limits include the inherited metadata-cache memory lifecycle, unsupported cold loading, point-read costs for large candidate sets, conservative failures during source changes, and unqualified platform/clustered/distributed behavior. Production readiness remains INCOMPLETE.


## Completed measurement and evidence

[Raw measurements, source identity and checksums](README.md).
The candidate executable SHA-256 is
`44e5004010c8b16564d54dc9e704572729a5dbe4155fb4913bbae550aa16553a`;
the unchanged scalar-decoder baseline is
`3fbe2f226b5d262aed4ce20faafafcb763b5ef52fc6ef295fb103fc87b77008e`.
Both ran against the same recovered fixture: 1M original GloVe25 records,
999,995 survivors, cosine/top10, group_id=1 with 9,999 eligible rows. Two warmups
preceded two rounds over ten unique queries per variant. Four server logical
CPUs, two client CPUs and the same temporary performance-profile hold were used;
the original power-saver selection was restored after each process. Local TCP
had authentication/TLS disabled, while separate authenticated SDK checks passed.

| Run order | Median of two round medians | Measured calls | Exact reference recall |
| --- | ---: | ---: | ---: |
| Prior executable before | 6,032.20 ms | 20 | 1.0 |
| New indexed execution | 120.50 ms | 20 | 1.0 |
| Same prior executable after | 6,127.38 ms | 20 | 1.0 |

The median of the four baseline round medians is 6,127.38 ms, approximately
50.85 times the candidate median (98.03% lower warm latency). Both strict and
tie-aware recall were 1.0 for every measured call. Each candidate query reported
`candidate_source=metadata_index`, 9,999 candidates and 9,999 scanned rows. It
ranked all of them, not an arbitrary top-K prefix. Each variant checked the
45 surviving changed records, five deletions and total record count.

Cache-state costs are material. Loading the existing metadata index through the
same document read took 0.69–0.72 s per process. The candidate's first
payload-cold vector query took **5,966.57 ms**; its next warmup took 111.77 ms.
The speedup table describes the warmed candidate payload cache, not a universal
first-query improvement. The secondary index occupies 18.87 MiB. Query-process
peak sampled PSS was 250.36 MiB / 228.20 MiB / 222.66 MiB in run order; these
samples are not memory-limit guarantees. Shared host activity, run order,
dynamic clocks and only ten unique inputs limit generalization. There is no
fresh pgvector/Qdrant, ANN or ingestion measurement for this executable, and the
previous unindexed three-database cells remain unchanged.

### Index preparation failure retained

The initial public `createIndex` request on the baseline returned
`write_outcome_unknown`. The process subsequently published a complete index
before its graceful shutdown. Independent inspection found `status=ready`,
`complete=true`, field group_id and 999,995 indexed-source documents. Catalog
creation-to-ready timestamps span 32.209 s; this is an observation, not a
successful client acknowledgement latency. Peak sampled PSS in that failed
preparation attempt was **5,463.52 MiB**.

No write was blindly retried and no durability setting or timeout assertion was
relaxed. The failed attempt and catalog observation are retained separately.
The matched read-only experiment then reused that same verified complete index
in both executables. Large synchronous metadata-index creation can leave clients
with an uncertain result; this is an unresolved external-testing/release concern
that the fast warm-query result does not fix.

## Validation results

| Gate | Status | Evidence/scope |
| --- | --- | --- |
| Original scan-budget regression | PASS | Old query exceeded maxScan=30 on 120 rows/20 matches; indexed route ranks every match and returns the independently ranked top three. |
| Publication-gap correction | PASS | Isolated old guard scope fails the distinct-new-ID paused-write test; corrected scope passes. |
| Complete-candidate mutation fencing | PASS | Paused query plus moving an excluded row into the filter returns explicit freshness failure. |
| Scalar/type/compound filters, limits and row visibility | PASS | Real-storage tests include numeric 1/1.0, string collisions, null/missing, OR/NOT/dotted fallback, cosine/L2/dot, foreign tenant, floor/bounded contexts, deletion and compaction. |
| Production and separate failpoint builds | PASS | Final builds exit 0 with production failpoints disabled. |
| Checkpoint regressions | PASS | 81 top-level checks, including CTest, document/media/auth, existing RF3/partition/restart, request isolation and crash scenarios. |
| ASan/UBSan and ThreadSanitizer | PASS | Instrumented LSM/query/test translation units, both query and publication tests. Linked dependencies are not a fully sanitized engine. |
| Python/Java integration through Node fixture | PASS | Authenticated TCP/TLS, document/media operations and SIGKILL recovery, actual exit 0. |
| Matched filtered-query diagnostic | PASS | All three measured subprocesses exit 0; all 60 query results match independent exact ground truth. |
| Public 1M metadata-index creation acknowledgement | FAIL | Initial request returned write_outcome_unknown despite eventual ready publication. |
| Cold metadata loading acceleration | INCOMPLETE | Deliberately ineligible; the existing loader lacks a bounded cancellable query contract. |
| Broader indexed named/sparse, platform, clustered/restore/upgrade and sustained-load qualification | INCOMPLETE | Existing storage/regression suites do not establish a full indexed-plan matrix. |
| Production release | INCOMPLETE | No new commit, tag, registry package or release was published. |

An independent read-only review found the publication gap and cold-loader issue;
both were addressed before the final tests. It also identified weak OR coverage;
the fixture now gives the OR-only branch the highest-scoring row and requires
the merged-scan plan. The review findings, dispositions, RED/GREEN logs, source
fingerprints and raw failures are retained. The earlier smoke failed because its
test fixture omitted the asynchronous index worker; startup/teardown now follows
the existing mixed-flow test. Initial compilation/setup errors are retained and
are not counted as meaningful regression failures or passing results.
