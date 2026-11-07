# v1.2 scalar metadata decoding follow-up

Status: PASS for this reader optimization and its stated validation scope.
Production readiness remains INCOMPLETE.
This continues the completed ANN optimization checkpoint (separate earlier evidence bundle).
The published packages and completed three-database report retain their original
runtime identities and results.

## Confirmed path and change

Filtered exact search already projects only required fields and evaluates the
existing filter before vector distance calculation. Repeating that optimization
would not address the remaining cost. The shared MessagePack field accessor
still constructs a generic binary reader for each scalar metadata field used
by visibility, version resolution and predicates during the complete scan.

`decodeMsgpackField` in `engine/src/storage_format_v2.cpp` now decodes bounded
scalar spans directly: null, booleans, signed/unsigned integers, float32/float64
and strings. Arrays, objects, binary and extension values use the existing
reference decoder. Both indexed and unindexed field lookup/projection use the
same helper. Field-span validation, duplicate-key handling, iterator merge order,
tenant checks, version/tombstone resolution and filter evaluation order are
unchanged. The WAL/SST encoding and numeric precision are unchanged.

This modifies a shared reader, so regression evidence must cover document,
media, storage/recovery, authentication and existing RF3 flows. It does not add a
metadata index, reduce the number of eligible rows ranked, change ANN recall
targets or turn a truncated scan into a valid exact result.

## Verification and measurement contract

`engine/test/storage_format_v2_test.cpp` compares field and projection values
and JSON types against the independent full MessagePack decoder. Cases include
all positive/negative fixints; noncanonical integer widths and boundary values;
float32/float64 signed zero, subnormals, extrema and nonfinite values; each string
length encoding; embedded NUL/Unicode; container/binary fallback; and truncated
encodings. Existing duplicate-field, malformed-row, wide-map, concurrent-reader,
SST version/tombstone and dense-scoring tests remain in place.

The previous measured executable was copied and hash-verified before rebuilding:
`8c293646b8a9ddb54c286268f5e021b660c95e0c2255bb8da1251539b7b1338f`.
The focused performance test uses the same disposable, recovered 1M fixture for
baseline, candidate and a baseline recheck. Each variant uses two warmups and two
rounds of ten unique queries, filter `group_id=1`, cosine/top10, four server CPUs,
two client CPUs and a temporary performance-profile hold. The fixture has
999,995 surviving records and 9,999 eligible filtered rows after the earlier
documented mutations/deletions. Every measured result must match independently
computed filtered ground truth. The same 45 changed records, five deletions and
total count are verified. No query-mode, filter, durability or resource limit is
relaxed for the candidate.

This is a baseline/candidate reader experiment, not a fresh three-database
benchmark. It must not overwrite the published comparison or substitute its
timings into that report. Twenty calls repeat ten queries; tail latency and
independent-build confidence are not established. CPU clocks, background load
and run order remain limitations even under the same power profile.

## Completed results

[Raw receipts, source fingerprints and checksums](README.md).

| Variant, in execution order | Median of two round medians | Measured calls | Exact reference recall |
| --- | ---: | ---: | ---: |
| Unchanged baseline before | 7,712.05 ms | 20 | 1.0 |
| Candidate scalar decoder | 5,745.14 ms | 20 | 1.0 |
| Unchanged baseline after | 7,247.24 ms | 20 | 1.0 |

Combining all four baseline round medians gives 7,432.28 ms versus the
candidate's 5,745.14 ms: **22.7% lower measured median latency**. Compared with
the second baseline control alone, latency is 20.7% lower; compared with the
first, 25.5% lower. The control drift is retained, not hidden. Both strict and
tie-aware recall were 1.0 for every measured call. No meaningful tail-latency
claim is made from this sample.

Baseline SHA-256:
`8c293646b8a9ddb54c286268f5e021b660c95e0c2255bb8da1251539b7b1338f`.
Candidate SHA-256:
`3fbe2f226b5d262aed4ce20faafafcb763b5ef52fc6ef295fb103fc87b77008e`.
The unchanged executable was used again after the candidate on the same fixture;
there is no new persistent format or migration. The original power-saver
selection was restored after all timed runs.

| Validation | Status | Scope |
| --- | --- | --- |
| Independent scalar/reference decoder checks | PASS | Release test plus sanitizer variants; numeric types, boundaries, special floats, strings, fallback and truncation. |
| Production and separate failpoint builds | PASS | Existing v1.2 configuration, version checks and failpoint separation retained. |
| Complete checkpoint runner | PASS | All 80 top-level checks, including CTest, document/media/auth, request isolation, existing RF3 and recovery scenarios. |
| AddressSanitizer/UBSan | PASS | Storage-format and test translation units instrumented; linked dependencies do not constitute a fully sanitized engine. |
| ThreadSanitizer | PASS | Same instrumented units, including concurrent immutable field/projection reads. |
| Python/Java integration through Node fixture | PASS | Authenticated TCP/TLS, document/media checks and SIGKILL recovery on the candidate executable. |
| Baseline/candidate/baseline experiment | PASS | All three subprocesses exited 0; all 60 measured calls passed independent reference checks. |
| Fresh three-database ANN/ingestion comparison for this candidate | INCOMPLETE | Not run in this follow-up; the earlier full comparison belongs to executable 8c293646. |
| Windows/macOS/ARM and full production qualification | INCOMPLETE | This follow-up is verified on Linux x64 only. |

Raw local logs remain under
`build-v1.2-release/filter-fast-scalar-20261103/`; the distributed evidence omits
private runtime logs and machine-specific paths while retaining their hashes.

## Remaining work

The query still visits every authoritative row. Integrating complete, current
metadata indexes with vector ranking is needed for a much larger gain; this
scalar-reader change adds no metadata-index execution plan. Selective ANN
filtering must retain an exact fallback or explicit failure when coverage cannot
be guaranteed. Incremental graph checkpoint persistence, automatic compaction,
distributed completeness and broader release qualification also remain open.

The 5.75 s result is a focused recovered-fixture measurement. It must not replace
the earlier 6.29 s fresh-workload cell or be represented as a new comparison with
pgvector's 59.63 ms. No new package, tag or release was published here.
