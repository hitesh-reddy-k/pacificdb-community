#!/usr/bin/env python3
"""Audit every configured cell and retain all rates, errors and recovery results."""
import argparse
import json
import math
import statistics
from pathlib import Path


def stats(values):
    assert values and all(math.isfinite(v) for v in values)
    return dict(mean=statistics.mean(values), median=statistics.median(values),
                minimum=min(values), maximum=max(values),
                sample_stddev=statistics.stdev(values) if len(values) > 1 else 0)


def summarize(directory):
    method = json.loads((directory / 'method.json').read_text())
    rows = []
    for case in method['config']['cases']:
        records = {}
        for label in ['v1.0.0', 'v1.1.1']:
            trials = []
            for number in range(1, method['config']['repeats'] + 1):
                file = directory / f'{case["name"]}-{number}-{label}.json'
                trial = json.loads(file.read_text())
                assert trial['workload'] == case, f'configuration mismatch: {file}'
                assert trial['round'] == number and trial['artifact']['label'] == label
                assert trial['artifact'] == next(a for a in method['artifacts'] if a['label'] == label)
                assert trial['integrity'].startswith('PASS'), f'integrity not passed: {file}'
                assert not trial['errors'] and trial['successful_calls'] == trial['calls'], f'errors: {file}'
                trials.append(trial)
            records[label] = dict(trials=trials,
                throughput=stats([t['attempted_calls_per_second'] for t in trials]),
                documents_per_second=stats([t['documents_per_second'] for t in trials]),
                latency_ms={q: stats([t['latency_ms'][q] for t in trials]) for q in ['mean', 'p50', 'p95', 'p99']},
                cpu_us_per_successful_call=stats([1e6*t['resources']['delta']['cpu_seconds']/t['successful_calls'] for t in trials]),
                peak_rss_mib=stats([t['resources']['sampled_peak_rss_kib']/1024 for t in trials]))
        old, new = records['v1.0.0'], records['v1.1.1']
        paired = [100 * (n['attempted_calls_per_second']/o['attempted_calls_per_second'] - 1)
                  for o, n in zip(old['trials'], new['trials'])]
        rows.append(dict(name=case['name'], versions=records,
            ratio_of_medians_change_pct=100*(new['throughput']['median']/old['throughput']['median']-1),
            paired_changes_pct=paired, median_paired_change_pct=statistics.median(paired)))
    return dict(status='PASS', complete_trials=sum(len(r['versions'][v]['trials']) for r in rows for v in r['versions']),
                method=method, rows=rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    summary = summarize(args.directory)
    (args.directory / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    lines = ['| Workload | v1.0.0 calls/s, median (range) | v1.1.1 calls/s, median (range) | Median paired change | p95 ms, old → new | p99 ms, old → new |',
             '|---|---:|---:|---:|---:|---:|']
    for row in summary['rows']:
        old, new = [row['versions'][v] for v in ['v1.0.0', 'v1.1.1']]
        throughput = lambda v: f'{v["throughput"]["median"]:,.1f} ({v["throughput"]["minimum"]:,.1f}–{v["throughput"]["maximum"]:,.1f})'
        latency = lambda q: f'{old["latency_ms"][q]["median"]:.3f} → {new["latency_ms"][q]["median"]:.3f}'
        lines.append(f'| {row["name"]} | {throughput(old)} | {throughput(new)} | {row["median_paired_change_pct"]:+.2f}% | {latency("p95")} | {latency("p99")} |')
    (args.directory / 'summary.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps({'status': summary['status'], 'complete_trials': summary['complete_trials']}))
