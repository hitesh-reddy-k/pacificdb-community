#!/usr/bin/env python3
"""Finalize a self-run candidate report only from retained machine-readable evidence."""
import argparse,hashlib,json,statistics,tarfile,zipfile,subprocess,re
from pathlib import Path

def cache_reuse(records):
 """Count verified row reuse, preserving missing metadata as unavailable."""
 if not records:return None
 total=hits=0
 for record in records:
  execution=record.get('execution',{})
  if 'payload_fetches' not in execution or 'payload_cache_hits' not in execution:return None
  fetched,cached=execution['payload_fetches'],execution['payload_cache_hits']
  assert isinstance(fetched,int) and isinstance(cached,int) and 0<=cached<=fetched
  total+=fetched;hits+=cached
 return {'payload_fetches':total,'payload_cache_hits':hits,'hit_fraction':hits/total if total else None}

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);args=p.parse_args()
 root=Path(__file__).resolve().parents[1];out=args.evidence.resolve();load=lambda name:json.loads((out/name).read_text())
 main=load('final-primary/summary.json');supp=load('final-supplement/summary.json');qual=load('regressions/summary.json');versions=load('version-provenance.json')
 continuation=load('recovery-continuation/summary.json') if (out/'recovery-continuation/summary.json').is_file() else None
 assert main['status'] in ('PASS','FAIL') and supp['status']=='PASS' and qual['status']=='PASS'
 if main['status']=='FAIL':
  assert continuation and continuation['status']=='PASS'
  assert continuation['candidate_sha256']==main['candidate_sha256']
  failed=[d for d in main['datasets'] if d['status']!='PASS']
  assert len(failed)==1 and failed[0]['rows']==1000000 and 'connection failed' in failed[0]['error']
 assert main['candidate_sha256']==supp['candidate_sha256']==qual['engine_sha256']==versions['pacific_binary']['sha256']
 assert main['postgres_sha256']==versions['postgres_binary']['sha256'] and main['pgvector_sha256']==versions['pgvector_library']['sha256']
 tsan=load('tsan-results.json');assert all(r['status']=='PASS' for r in tsan)
 asan=load('asan-ubsan-results.json');assert all(r['status']=='PASS' for r in asan)
 ctest_log=(out/'regressions/ctest.log').read_text()
 ctest_count=int(re.search(r'100% tests passed, 0 tests failed out of (\d+)',ctest_log).group(1))
 median=statistics.median;number=lambda x:f'{x:,.2f}'
 queries=[];costs=[];operational=[];derived=[]
 for d in main['datasets']:
  assert (d['status']=='PASS' or d['rows']==1000000 and continuation) and len(d['rounds'])==3
  for system in ['pacific','postgres']:
   s=d['systems'][system]
   for mode in ['exact','ann']:
    selected=[r[system][mode] for r in d['rounds']]
    assert all(r['samples']==main['queries'] and r['recall_at_10']>=(1 if mode=='exact' else .95) for r in selected)
    row={'rows':d['rows'],'system':system,'mode':mode,'p50_ms':median(r['p50_ms'] for r in selected),
      'p95_ms':median(r['p95_ms'] for r in selected),'p99_ms':median(r['p99_ms'] for r in selected),
      'serial_qps':median(r['serial_qps'] for r in selected),'recall_at_10':median(r['recall_at_10'] for r in selected),
      'strict_recall_at_10':median(r['strict_recall'] for r in selected),
      'round_p50_ms':[r['p50_ms'] for r in selected],'samples_per_round':main['queries'],
      'round_cache_reuse':[cache_reuse(r['records']) for r in selected]}
    queries.append(row)
   exact=s['indexes']['exact'];ann=s['indexes']['ann'];extended=s.get('extended')
   if extended is None:
    assert d['rows']==1000000 and continuation
    extended=continuation['systems'][system];assert extended['status']=='PASS'
   costs.append({'rows':d['rows'],'system':system,'ingestion_rows_s':s['load']['rows_per_second'],
     'ingestion_seconds':s['load']['seconds_including_preparation_and_final_flush'],'batch_p95_ms':s['load']['batch_latency']['p95_ms'],
     'flat_build_seconds':exact.get('source_and_build_seconds',0),'ann_build_seconds':ann.get('source_and_build_seconds',ann.get('build_seconds')),
     'peak_pss_mib':max(s['peak_sampled_pss_bytes'],continuation['systems'][system]['peak_sampled_pss_bytes'] if continuation and d['rows']==1000000 else 0)/1024**2,'storage_after_ann_mib':s['storage_after_build']['ann']['bytes']/1024**2,
     'wal_after_ann_mib':s['storage_after_build']['ann']['wal_bytes']/1024**2})
   mixed=extended['mixed_90_read_10_write'];recovery=extended['crash_recovery']
   operational.append({'rows':d['rows'],'system':system,'filter_1percent_p50_ms':s['filter_1_percent_exact']['p50_ms'],
     'concurrency4_qps':s['concurrency_4_ann']['wall_qps'],'concurrency4_p95_ms':s['concurrency_4_ann']['p95_ms'],
     'update_commit_p50_ms':extended['updates']['commit_latency']['p50_ms'],
     'post_ack_index_wait_p50_ms':extended['updates']['post_ack_ann_ready_wait']['p50_ms'],
     'delete_commit_p50_ms':extended['deletes']['commit_latency']['p50_ms'],'mixed_reads':mixed['reads_attempted'],
     'mixed_read_successes':mixed['read_successes'],'mixed_read_success_fraction':mixed['successful_read_fraction'],
     'mixed_read_error_count':len(mixed['read_errors']),'mixed_write_p50_ms':mixed['write_latency']['p50_ms'],
     'restart_to_connection_s':recovery['restart_to_connection_seconds'],'restart_ann_rebuild_s':recovery['ann_reconstruction_seconds'],
     'recovery_count':recovery['count'],'recovery_ann_recall':recovery['ann_recall_at_10']})
  a=next(r for r in queries if r['rows']==d['rows'] and r['system']=='pacific' and r['mode']=='exact')
  b=next(r for r in queries if r['rows']==d['rows'] and r['system']=='postgres' and r['mode']=='exact')
  derived.append({'rows':d['rows'],'postgres_exact_p50_divided_by_pacific_exact_p50':b['p50_ms']/a['p50_ms']})
 availability_failures=[r for r in operational if r['mixed_read_error_count']]
 summary={'status':main['status'],'status_scope':'original primary run status; a separate continuation does not erase a failed startup budget',
   'continuation_status':continuation['status'] if continuation else 'not needed',
   'mixed_ann_read_availability_status':'FAIL' if availability_failures else 'PASS',
   'origin':'self-run project candidate benchmark; unpublished local report; no third-party endorsement',
   'release_status':'INCOMPLETE','versions':versions,'queries':queries,'costs':costs,'operational':operational,
   'exact_ratios':derived,'supplement_status':supp['status'],'qualification_checks':qual['top_level_checks'],
   'source_status':'uncommitted v1.2 candidate; complete source archive and binary hashes retained',
   'unexecuted':['actual physical power loss','complete mixed-version/platform packaging matrix','sustained multi-hour production load','distributed vector search'],
   'limitations':['p99 is empirical on 1000 held-out queries, not a production SLO','three rounds repeat the same held-out queries',
     'Pacific float64 packed exact versus PostgreSQL float32 vector exact; both checked against float64 reference within declared tolerance',
     'full-build HNSW and replica-local maintenance differ from PostgreSQL persistent index recovery',
     'separate synthetic high-dimensional workloads do not establish million-row high-dimensional performance',
     'BM25 versus ts_rank_cd hybrid scores are not relevance-equivalent','portable pgvector build uses OPTFLAGS empty; full build logs supplied']}
 (out/'results.json').write_text(json.dumps(summary,indent=2))
 qtable='\n'.join(f"| {r['rows']:,} | {r['system']} {r['mode']} | {r['p50_ms']:.3f} | {r['p95_ms']:.3f} | {r['p99_ms']:.3f} | {r['serial_qps']:.2f} | {r['strict_recall_at_10']:.4f} / {r['recall_at_10']:.4f} |" for r in queries)
 ctable='\n'.join(f"| {r['rows']:,} | {r['system']} | {r['ingestion_rows_s']:,.0f} | {r['flat_build_seconds']:.2f} | {r['ann_build_seconds']:.2f} | {r['peak_pss_mib']:.1f} | {r['storage_after_ann_mib']:.1f} | {r['wal_after_ann_mib']:.1f} |" for r in costs)
 otable='\n'.join(f"| {r['rows']:,} | {r['system']} | {r['concurrency4_qps']:.2f} | {r['update_commit_p50_ms']:.2f} | {r['post_ack_index_wait_p50_ms']:.2f} | {r['mixed_read_successes']}/{r['mixed_reads']} | {r['restart_to_connection_s']:.2f} | {r['restart_ann_rebuild_s']:.2f} |" for r in operational)
 cache_table=[]
 for row in queries:
  if row['system']!='pacific':continue
  for i,reuse in enumerate(row['round_cache_reuse']):
   if reuse is None:continue
   fraction=f"{reuse['hit_fraction']:.1%}" if reuse['hit_fraction'] is not None else 'n/a'
   cache_table.append(f"| {row['rows']:,} | {row['mode']} | {i+1} | {row['round_p50_ms'][i]:.3f} | {reuse['payload_cache_hits']:,} / {reuse['payload_fetches']:,} | {fraction} |")
 stable=[]
 for d in supp['workloads']:
  assert d['status']=='PASS'
  for system,s in d['systems'].items():
   for mode,m in s['modes'].items():stable.append(f"| {d['rows']:,} | {d['dimensions']} | {d['metric']} | {system} {mode} | {m['p50_ms']:.3f} | {m['p95_ms']:.3f} | {m['recall_at_10']:.4f} |")
 continuation_note=''
 if continuation:
  continuation_note='''The original 1M dirty restart did not become ready within the
120-second harness budget and remains FAIL. The separate
[recovery continuation](recovery-continuation/summary.json) uses an explicit
900-second startup budget on retained disposable roots; it does not retroactively
pass the original deadline. PostgreSQL is seeded with the same 50 acknowledged
updates and five deletes before its SIGKILL. Counts, all 45 surviving changed
records and independent post-recovery ANN references are verified.

For 1M rows only, the mutation/availability table uses new continuation measurements
**after** that recovery: ten updates to IDs 10–19, five additional deletes of IDs
5–9, and 40 mixed writes to IDs 200–239. The original 1M mutation latencies were
not saved before its startup failure and are not invented. Static query/build/load
measurements still come from the original primary run. No second crash after the
continuation mutations is implied. Peak sampled PSS includes both attempts.
'''
 report=f'''# PacificDB v1.2 Community candidate — benchmark report

**Original primary benchmark: {main['status']}. Production release: INCOMPLETE.**
This is a finalized **self-run project benchmark** prepared for company review.
It is not an official PostgreSQL/pgvector benchmark, independent certification,
release publication or evidence that every PacificDB deployment is production-ready.

**Mixed ANN read availability: {'FAIL' if availability_failures else 'PASS'}** against
an error-free read target for this measured workload. Explicit freshness errors
protect correctness but still count as unavailable reads. Completed static query
phases passed their stated reference/recall checks; PacificDB did not meet every
availability or performance target. Compare ingestion, ANN
latency, maintenance and recovery costs as well as exact-search latency.

Compared versions: PacificDB **1.2.0-dev**, PostgreSQL **18.6**, pgvector **0.8.7**.
The stable PostgreSQL/pgvector versions were verified on {versions['checked_date']} against the
[PostgreSQL version policy](https://www.postgresql.org/support/versioning/) and
[pgvector tagged changelog](https://github.com/pgvector/pgvector/blob/v0.8.7/CHANGELOG.md).
PostgreSQL 19 RC1 and pgvector 0.9.0 unreleased are not substituted for stable releases.

## Exact and approximate search

Shared GloVe-25 float32 source values, cosine, top-10, full vector/document payloads.
Each entry is the median of three round statistics; percentiles use NumPy linear
interpolation. Each round uses the same 1000
held-out queries (indices 100–1099), after ten warmups. Tuning uses disjoint queries
0–99. p95/p99 are empirical diagnostics, not production tail-latency guarantees.

| Rows | Public path | p50 ms | p95 ms | p99 ms | Serial QPS | Recall@10 strict / tie-aware |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
{qtable}

Exact is compared with exact; ANN is compared with ANN. The exact ratio for each
size is in [machine-readable results](results.json). No general performance
superiority claim follows from one machine/dataset. Strict IDs and a tie-aware
float64 exact reference, payload values, ranking and result cardinality are
checked outside the timed interval. Float32 score tolerance is 1e-6; raw strict
recall is also retained. PacificDB's packed exact index retains float64 values;
PostgreSQL stores vector float32. This precision/memory difference is disclosed.

### Verified payload-cache reuse by round

The first measured round follows tuning and warmups; it is not a cold-cache run.
Later rounds repeat the same held-out queries. PacificDB reuses full rows only
after a complete storage read has certified them for the current collection
version. Every query still traverses/ranks vectors, checks row visibility and
authorization, and validates the final index/source certificate. This is not a
query-result cache. Any collection mutation conservatively invalidates proofs.
The existing bounded row cache retains at most approximately 50K rows per
collection; proof storage adds approximately 40 bytes per entry on this build,
before allocator/alignment effects. PostgreSQL uses its normal server/OS caches;
no equivalent PostgreSQL row-hit metric is inferred.

| Rows | Pacific path | Round | p50 ms | Verified hits / payload fetches | Hit fraction |
| --- | --- | ---: | ---: | ---: | ---: |
{chr(10).join(cache_table)}

## Ingestion, build, memory and storage

There is one fresh load and one initial build per product/dataset; query rounds
are repeated on that loaded database. Ingestion/build results are single-run
measurements, not three independent load trials or confidence intervals.

Fresh durable batch ingestion, 1000 records per acknowledged transaction/batch,
including identical payload construction and final flush/checkpoint. No ANN index
exists during the initial load. Both methods use their native batch protocol:
PacificDB insertMany and PostgreSQL COPY inside a committed transaction.

| Rows | System | Ingestion rows/s | Flat build s | ANN source/build s | Peak sampled PSS MiB | Data+index MiB | WAL MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
{ctable}

PostgreSQL exact has no packed derived index, so its flat-build entry is zero.
Both ANN builds use M=16 and ef_construction=200; source scanning is included for
PacificDB. ef_search is tuned separately through the same fixed search grid,
using a prespecified tuning threshold 0.97 and held-out acceptance 0.95. All tuning
curves/configurations are retained. HNSW randomized construction can vary by run.

Memory numbers are whole-run sampled PSS maxima across live server processes,
including startup/build/search/recovery, not isolated query RAM or guaranteed
peaks. PostgreSQL RSS is not summed for a fair memory comparison because it
repeats shared mappings. CPU and I/O samples are live-process snapshots, not
complete lifetime counters after workers exit. Storage includes each engine's
actual durable artifacts. PostgreSQL preallocates WAL segments; allocated WAL
size is not equivalent to bytes newly written. No write-amplification claim is
inferred from directory sizes.

PacificDB drops the flat derived index before building ANN to respect existing
staging reservations. Records are retained. Exact and ANN phase memory/index
costs must not be interpreted as simultaneously available under these limits.

## Concurrency, mutation visibility and recovery

{continuation_note}

| Rows | System | Four-client ANN QPS | Update commit p50 ms | Post-ack ANN-ready wait p50 ms | Mixed successful reads | Restart-to-connection s | Additional ANN rebuild s |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
{otable}

Four-client static throughput includes connection setup and client validation in
wall time; individual query latency excludes those reference checks.

The mixed workload is 360 attempted reads and 40 acknowledged writes across four
clients. PacificDB index lag/freshness errors count as failed reads and are
retained. Successful-only latency must be read together with that availability.
Returned vector/score consistency and deleted-ID exclusion are checked; snapshot
recall during simultaneous mutations is not inferred. PostgreSQL updates its
index synchronously; PacificDB maintenance is asynchronous, so commit latency
and post-ack visibility wait are both shown. Ten controlled updates have
independent post-update reference checks; five deletes have count/exclusion checks.

Only owned disposable servers are SIGKILLed. Restart checks verify all expected
records and ANN recall. PacificDB definitions survive, but derived ANN is ABSENT
until reconstruction; PostgreSQL's persistent index is recovered and available.
Restart-to-connection alone is not PacificDB ANN recovery time. Full rebuild time
is separately reported. These are process-crash tests, not actual power cuts.
The new primary run preserves each owned dirty fixture after SIGKILL and before
restart. The forensic copy is outside the startup timer; raw total stop/restart
wall time includes that copy and records its separate duration. Recovery is warm
cache, including filesystem activity from fixture preservation.

## Filters and supplemental dimensions/metrics

A 1% metadata equality filter is measured through complete exact plans on both
systems; PacificDB uses its declared complete storage-scan fallback. Ten filter
queries are retained per size; no meaningful tails are claimed for that small set.
Other filter semantics have targeted correctness/regression coverage, not a full
selectivity performance matrix.

The supplemental workloads use deterministic normalized synthetic vectors, 10K
rows and 1000 held-out queries, one run per metric/dimension. They are separate
from GloVe, not substitutes for million-vector high-dimensional qualification.

| Rows | Dimensions | Metric | Path | p50 ms | p95 ms | Recall@10 |
| --- | ---: | --- | --- | ---: | ---: | ---: |
{chr(10).join(stable)}

Sparse/named-vector exact and hybrid capability timings are in
[the supplemental evidence](final-supplement/summary.json). Both hybrid examples
use RRF, but PacificDB BM25 and PostgreSQL ts_rank_cd are different lexical
rankers. Their relevance/latency cannot be presented as identical hybrid algorithms.
Supported sparse precision/format and missing-vector behavior differ and are
validated explicitly. This is a capability diagnostic, not a complete feature
parity certification.

## Hardware and measurement protocol

{versions['cpu_model']}; kernel {versions['kernel']}. Host memory and environment
are recorded in [methodology](final-primary/methodology.json). Both servers use
logical CPUs {main['server_cpus']}; clients use {main['client_cpus']}. Scratch roots
are private directories on local NVMe/ext4. Warm-cache results are reported; OS
caches are not dropped. No concurrent compiler or qualification suite was scheduled
for the final replay. Unrelated host services were left running; their activity,
thermal conditions and filesystem cache state are not controlled like a lab.

Both systems keep fsync and committed acknowledgement enabled. PostgreSQL has
synchronous_commit/full_page_writes on, shared_buffers 512 MiB, work_mem 64 MiB,
maintenance_work_mem 1 GiB, three gather/maintenance workers within four server
CPUs, JIT off. PacificDB WAL/checksum/fsync/commit flushing are on, the standalone
Raft bypass is disabled, and configured worker/memory budgets are retained.
Authentication is disabled only on loopback for both benchmark servers; security
correctness is tested separately. RF3 correctness qualification is separate from
standalone latency. No RF3-versus-single-node speed comparison is made.

PostgreSQL uses a fresh source build and pgvector's portable OPTFLAGS-empty build;
full configure/compiler logs are retained. Query-plan checks prove PostgreSQL
exact scans avoid ANN and ANN uses vectors_hnsw. Prepared generic-plan reuse is
disabled. Fresh connections are opened before each macro measurement phase on
both systems to avoid idle-session expiry during alternating-engine runs;
connection setup is excluded from query timing. PacificDB requires the named index and reports exact/ANN/fallback status
per response. Selective-filter fallback is reported explicitly. Each round
alternates database measurement order. All timed paths include client socket and
payload decoding; reference checking is outside the timer.

## Implementation and qualification

The candidate preserves authoritative WAL/LSM records, schemas and bounded derived
index maintenance. This checkpoint fixes float64 intermediate overflow producing
incorrect finite cosine scores, adds bounded query ef_search, and exposes rebuild
requirements/status. The pre-fix numeric failure and corrected tests are retained.
The follow-on corrects point reads crossing a flush boundary, late cache refill
after concurrent writes, and Raft visibility advancing during lookup. Eighteen
focused real-storage/real-Raft cases cover those interleavings. Update responses
must report actual mutation success, and every changed/deleted record is checked
before and after the benchmark crash; phase evidence is saved incrementally.
This candidate also qualifies reuse of authoritative point-cache entries: older
writer/replay cache rows cannot override newer SST versions, tombstones, restores
or namespace recreation. Native exact/ANN/hybrid queries retain authoritative
validation while avoiding unconditional cache eviction.

PASS: {qual['top_level_checks']} top-level regression checks including {ctest_count} CTest cases;
{len(tsan)} current targeted TSAN tests and {len(asan)} ASan/UBSan tests; Node/Python/Java units and live Python/Java
TCP/TLS/SIGKILL recovery. Prior checkpoint evidence supplies broader HNSW mutation
scale/sanitizer coverage for its recorded revision, not an invented rerun here.
Document/media/auth/WAL/LSM/RF3 compatibility is exercised by those tests; arbitrary
production workloads and every upgrade/platform are not certified.

## Release and publication decision

**INCOMPLETE** for production release. This scoped Community candidate permits a
validated per-index locking strategy and explicit manual maintenance rather than
requiring immutable graph snapshots/automatic compaction. It does not advertise
clustered vector schemas/catalogs or complete distributed shard search. Those
unsupported operations remain gated. Persistent history capacity, rebuild time,
lag/error behavior and lack of online schema migration are documented limits.

Actual power-loss evidence, complete upgrade/platform packaging, independent
security assessment and sustained production qualification remain unfinished.
Existing release-audit rules are not disabled. The uncommitted tree cannot be
published as a source-bound stable release under that audit. Preparing this local
report does not authorize a commit, push, public upload or deployment.

The benchmark was requested as a company submission. These results may be described
as **PacificDB's self-run candidate benchmark with attached methodology**, not as
an independently certified result or a PostgreSQL/pgvector-endorsed benchmark.

## Reproduction and evidence

Candidate SHA-256: `{main['candidate_sha256']}`.
Source baseline commit: `{versions['pacificdb']['commit']}`, branch v1.2 with attached
uncommitted source changes. See [source fingerprints](source-sha256.json),
[source archive](source-checkpoint.tar.gz), [dependency provenance](version-provenance.json),
[regressions](regressions/summary.json), [results](results.json),
[primary raw records](final-primary/summary.json),
[supplemental raw records](final-supplement/summary.json), and
[scoped Community contract](../../V1.2_COMMUNITY_SCOPE.md).

Use a fresh evidence directory, isolated build/data directories, the exact dataset
manifest and pinned PostgreSQL/pgvector source versions. Run all qualification
before timing. The complete scripts and toolchain/configuration are attached.

```bash
/path/to/venv/bin/python scripts/benchmark-community-v12.py \\
  --build build-v1.2-verified-cache \\
  --pg build-v1.2-official-deps/postgres18/bin \\
  --publication /path/to/shared-glove-source \\
  --output /path/to/empty-primary-evidence --scratch /path/to/private-ext4-scratch \\
  --sizes 10000,100000,1000000 --queries 1000 --rounds 3 --warmups 10 --startup-timeout 900
/path/to/venv/bin/python scripts/benchmark-community-v12-supplement.py \\
  --build build-v1.2-verified-cache \\
  --pg build-v1.2-official-deps/postgres18/bin \\
  --output /path/to/empty-supplement-evidence --scratch /path/to/private-ext4-scratch \\
  --queries 1000
```

Earlier runs remain separate in [historical attempts](historical-attempts/README.md).
They include the idle-session fixture failure, the October 31 million-row restart
deadline/changed-row failure and the interrupted November 1 pre-cache comparison.
Their engine hashes differ from this candidate. None is retroactively marked PASS
or pooled into these measurements. The smoke run is harness validation and excluded
from final results. Failed configuration, numeric and cache RED logs are preserved.
No failed run is silently replaced or counted as PASS.
'''
 (out/'REPORT.md').write_text(report)
 print('REPORT_READY',out/'REPORT.md')
if __name__=='__main__':main()
