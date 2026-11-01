#!/usr/bin/env python3
"""Summarize real published-artifact runs without promoting partial qualification."""
import argparse
import json
import statistics
from pathlib import Path


def read(path):
    return json.loads(path.read_text())


def number(value, digits=3):
    return f'{value:.{digits}f}' if isinstance(value, (int, float)) else 'INCOMPLETE'


def rounds(dataset, kind, mode):
    return [row[kind][mode] for row in dataset.get('rounds', []) if mode in row.get(kind, {})]


def render(main_runs, supplemental, release_url, complete=False):
    inputs = [*main_runs, supplemental]
    status = 'PASS' if all(run.get('status') == 'PASS' for run in inputs) else 'FAIL' if any(run.get('status') == 'FAIL' for run in inputs) else 'INCOMPLETE'
    if complete and status != 'PASS':
        raise ValueError('complete report requires every input run to finish PASS; observed ' + status)
    identities = {run.get('candidate_sha256') for run in inputs}
    if len(identities) != 1 or None in identities:
        raise ValueError('cannot combine different or unidentified candidate binaries')
    script_hashes = {run.get('script_sha256') for run in main_runs}
    if len(script_hashes) != 1 or None in script_hashes:
        raise ValueError('cannot silently combine different main measurement scripts')
    datasets = [dataset for run in main_runs for dataset in run['datasets']]
    if len({dataset['rows'] for dataset in datasets}) != len(datasets):
        raise ValueError('duplicate dataset attempts must be reported separately')
    lines = ['# Published PacificDB candidate versus PostgreSQL + pgvector', '',
        f'Execution status: **{status}** for the supplied workload matrix. Production readiness: **INCOMPLETE**.', '',
        f'Release: {release_url}', f'Candidate engine SHA-256: `{next(iter(identities))}`.',
        f'Main harness SHA-256: `{next(iter(script_hashes))}`.', '',
        'This is a self-run Linux x64, standalone, warm-cache comparison. It is not independent certification or a distributed/RF3 benchmark. Every result retains its actual artifact identity; RC.1 numbers must not be relabeled RC.2.', '',
        '## Held-out dense search', '',
        'GloVe25 cosine, K=10;100 tuning queries and1000 held-out queries. Three alternating-order rounds. The table reports the median of the per-round p50 values and mean round Recall@10, with no pooling of repeated queries into independent samples.', '',
        '| Vectors | Mode | PacificDB ms | PostgreSQL ms | Pacific recall | PostgreSQL recall |',
        '| ---: | --- | ---: | ---: | ---: | ---: |']
    for dataset in datasets:
        for mode in ['exact', 'ann']:
            values = {kind: rounds(dataset, kind, mode) for kind in ['pacific', 'postgres']}
            if all(len(items) == 3 for items in values.values()):
                lines.append(f'| {dataset["rows"]} | {mode} | ' + ' | '.join(number(statistics.median(item['p50_ms'] for item in values[kind])) for kind in values) + ' | ' + ' | '.join(number(statistics.mean(item['recall_at_10'] for item in values[kind]), 4) for kind in values) + ' |')
            else:
                lines.append(f'| {dataset["rows"]} | {mode} | INCOMPLETE | INCOMPLETE | INCOMPLETE | INCOMPLETE |')
    lines += ['', '## Per-round latency distributions', '',
        'Each row is a round of1000 observations. p95/p99 describe that round only. No tail percentiles are inferred from three aggregate values.', '',
        '| Vectors | System | Mode | Round | p50 ms | p95 ms | p99 ms | Mean ms | Serial QPS |',
        '| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for dataset in datasets:
        for kind in ['pacific', 'postgres']:
            for mode in ['exact', 'ann']:
                for i, value in enumerate(rounds(dataset, kind, mode), 1):
                    lines.append(f'| {dataset["rows"]} | {kind} | {mode} | {i} | ' + ' | '.join(number(value.get(key)) for key in ['p50_ms', 'p95_ms', 'p99_ms', 'mean_ms', 'serial_qps']) + ' |')
    lines += ['', '## Ingestion, initial indexing and resource costs', '',
        '| Vectors | System | Durable ingestion rows/s | Ingestion s | Packed exact setup s | Initial ANN build s | Peak sampled PSS MiB | Peak sampled RSS MiB |',
        '| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for dataset in datasets:
        for kind in ['pacific', 'postgres']:
            system = dataset['systems'].get(kind, {})
            load = system.get('load', {})
            indexes = system.get('indexes', {})
            ann = indexes.get('ann', {})
            memory = [number(system[key] / 1024**2) if key in system else 'INCOMPLETE' for key in ['peak_sampled_pss_bytes', 'peak_sampled_rss_bytes']]
            exact = number(indexes.get('exact', {}).get('source_and_build_seconds')) if kind == 'pacific' else 'none (scan)' if 'exact' in indexes else 'INCOMPLETE'
            lines.append(f'| {dataset["rows"]} | {kind} | ' + number(load.get('rows_per_second'), 1) + ' | ' + number(load.get('seconds_including_preparation_and_final_flush')) + ' | ' + exact + ' | ' + number(ann.get('source_and_build_seconds', ann.get('build_seconds'))) + ' | ' + ' | '.join(memory) + ' |')
    lines += ['', '## Storage after initial ANN construction', '',
        '| Vectors | System | Data-root MiB | WAL/log MiB | Pacific graph bytes at READY MiB |',
        '| ---: | --- | ---: | ---: | ---: |']
    for dataset in datasets:
        for kind in ['pacific', 'postgres']:
            system = dataset['systems'].get(kind, {})
            storage = system.get('storage_after_build', {}).get('ann', {})
            resource = system.get('indexes', {}).get('ann', {}).get('index', {}).get('resources', {})
            lines.append(f'| {dataset["rows"]} | {kind} | ' + ' | '.join([
                number(storage['bytes'] / 1024**2) if 'bytes' in storage else 'INCOMPLETE',
                number(storage['wal_bytes'] / 1024**2) if 'wal_bytes' in storage else 'INCOMPLETE',
                number(resource['graph_bytes'] / 1024**2) if 'graph_bytes' in resource else 'not reported here']) + ' |')
    lines += ['', 'Data-root totals include each engine\'s own WAL/retention policy, not equivalent table/index-only storage. PacificDB ANN graphs occupy RAM and are rebuilt after restart; PostgreSQL persists its graph on disk. Retained forensic copies and benchmark input/output files are separate space. Graph-accounted bytes at READY differ from sampled process RSS/PSS.', '',
        '## Filtering, concurrent reads and mutation availability', '',
        '| Vectors | System | 1% filter exact p50 ms | Static four-reader ANN QPS | Mixed strict ANN reads succeeded/attempted | Update commit p50 ms | Post-ack ANN readiness wait p50 ms |',
        '| ---: | --- | ---: | ---: | ---: | ---: | ---: |']
    for dataset in datasets:
        for kind in ['pacific', 'postgres']:
            system = dataset['systems'].get(kind, {})
            extended = system.get('extended', {})
            mixed = extended.get('mixed_90_read_10_write', {})
            updates = extended.get('updates', {})
            fraction = f'{mixed["read_successes"]}/{mixed["reads_attempted"]}' if mixed else 'INCOMPLETE'
            lines.append(f'| {dataset["rows"]} | {kind} | ' + ' | '.join([
                number(system.get('filter_1_percent_exact', {}).get('p50_ms')),
                number(system.get('concurrency_4_ann', {}).get('wall_qps')), fraction,
                number(updates.get('commit_latency', {}).get('p50_ms')),
                number(updates.get('post_ack_ann_ready_wait', {}).get('p50_ms'))]) + ' |')
    lines += ['', '**Availability is separate from correctness:** explicit index-unavailable errors during mutations remain counted as failed reads. A workload execution PASS does not mean all requests succeeded. PacificDB updates commit before asynchronous ANN readiness; PostgreSQL index updates are synchronous. Compare both costs. No p95/p99 is inferred from10 updates,5 deletes or10 filter queries.', '',
        '## Process crash and recovery', '',
        '| Vectors | System | Restart to connection s | Recovered count | Verified changed records | ANN reconstruction s | Post-recovery Recall@10 |',
        '| ---: | --- | ---: | ---: | ---: | ---: | ---: |']
    for dataset in datasets:
        for kind in ['pacific', 'postgres']:
            recovery = dataset['systems'].get(kind, {}).get('extended', {}).get('crash_recovery', {})
            lines.append(f'| {dataset["rows"]} | {kind} | ' + ' | '.join([
                number(recovery.get('restart_to_connection_seconds')),
                number(recovery.get('count'), 0), number(recovery.get('mutation_verification', {}).get('changed_records_verified'), 0),
                number(recovery.get('ann_reconstruction_seconds')), number(recovery.get('ann_recall_at_10'), 4)]) + ' |')
    lines += ['', 'The harness verifies45 surviving changed records and5 tombstones after SIGKILL, in addition to total record count. It preserves a forensic data-root copy. Copy time is separate from restart-to-connection time. These warm-cache process-crash checks do not certify physical power-loss safety. PacificDB persisted definitions require graph reconstruction; PostgreSQL persists its index.', '',
        '## Supplemental workloads', '', f'Supplemental execution status: **{supplemental.get("status", "INCOMPLETE")}**.', '',
        '| Rows | Dimensions | Metric | System | Exact p50 ms / recall | ANN p50 ms / recall |',
        '| ---: | ---: | --- | --- | --- | --- |']
    for workload in supplemental.get('workloads', []):
        for kind, system in workload.get('systems', {}).items():
            values = [system.get('modes', {}).get(mode, {}) for mode in ['exact', 'ann']]
            lines.append(f'| {workload["rows"]} | {workload["dimensions"]} | {workload["metric"]} | {kind} | ' + ' | '.join(number(value.get('p50_ms')) + ' / ' + number(value.get('recall_at_10'), 4) for value in values) + ' |')
    lines += ['', '| Capability | System | Rows | p50 ms | p95 ms | Index/lexical setup s |',
        '| --- | --- | ---: | ---: | ---: | ---: |']
    for workload in supplemental.get('workloads', []):
        for kind, system in workload.get('systems', {}).items():
            capability = system.get('sparse_named_hybrid')
            if not capability:
                continue
            for mode in ['sparse_named_exact', 'hybrid']:
                value = capability[mode]
                lines.append(f'| {mode} | {kind} | {capability["rows"]} | ' + ' | '.join([
                    number(value.get('p50_ms')), number(value.get('p95_ms')),
                    number(value.get('build_seconds')) if mode == 'hybrid' else 'scan']) + ' |')
    lines += ['', 'Supplemental dense cases have100 measured queries (p95, no p99). Sparse/named/hybrid diagnostics use1000 records and independently check expected IDs/scores. Native BM25 and PostgreSQL ts_rank_cd are different lexical scorers; the RRF hybrid comparison is not equal retrieval-quality certification.', '',
        '## Interpretation and reproducibility limits', '',
        '- PostgreSQL 18.6 and pgvector 0.8.7 are recorded in raw version/configuration output. Both systems retain durable WAL/fsync acknowledgements. No durability setting was weakened.',
        '- PacificDB uses JSON batch insertion; PostgreSQL uses transactional COPY,1000 rows per batch. Dense JSON numeric precision is preserved versus pgvector float32. Wire formats and payload metadata differ.',
        '- Exact timing compares a separately built PacificDB packed derived index against PostgreSQL verified sequential/parallel exact scan. Include packed-index setup. It does not imply every unindexed exact query has this latency.',
        '- HNSW M=16/ef_construction=200; ef_search is selected separately on100 tuning queries to reach0.97 recall. Held-out minimum Recall@10 is0.95. Raw curves and plans are retained.',
        '- Four logical CPUs are assigned to each server, two to the client. Affinity does not isolate unrelated host activity, thermal/power conditions or shared memory bandwidth. Operating-system caches are not dropped.',
        '- Peak RSS/PSS is sampled. Raw per-phase disk/WAL bytes reflect different retention/format policies, not normalized index-only space. All raw measurements, including failures and explicit availability errors, remain in the archive.',
        '- The benchmark used cached native/SDK bytes checked against the exact public release digests. Separate fresh public-package consumer tests identify their own receipts; no development executable was substituted.',
        '- Timings change with hardware/cache/concurrency. This is not a promise of identical company results or superiority across workloads. Complete distribution/security/upgrade/distributed/power-loss qualification is separate.', '',
        f'Follow `docs/releases/{release_url.rstrip("/").split("/")[-1]}/BENCHMARK_REPRODUCTION.md` for the frozen inputs, hashes and commands. Summary/methodology/per-query/progress/error evidence accompanies this report. Public copies redact private host/path identifiers only; original local evidence is preserved.']
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', type=Path, action='append', required=True, help='main run directory; repeat for separate dataset sizes')
    parser.add_argument('--supplement', type=Path, required=True)
    parser.add_argument('--release-url', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-complete', action='store_true')
    args = parser.parse_args()
    result = render([read(path / 'summary.json') for path in args.main], read(args.supplement / 'summary.json'), args.release_url, args.require_complete)
    args.output.write_text(result)
    print('REPORT_WRITTEN', args.output)


if __name__ == '__main__':
    main()
