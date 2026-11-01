#!/usr/bin/env python3
"""Paired synthetic ingestion/exact replay; standalone, identical durable settings.

Requires prebuilt db_engine_vector_exact_benchmark in both directories. Uses fresh
OwnedTestRoot datasets. Run without competing compilation or benchmark processes.
Not a distributed, sustained-load, or ANN benchmark.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build', type=Path, required=True)
    parser.add_argument('--candidate-build', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'linux' or not hasattr(os, 'wait4'):
        parser.error('requires Linux wait4 for per-process peak RSS in KiB')
    output = args.evidence.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error('evidence directory must be empty')
    binaries = {label: (directory / 'db_engine_vector_exact_benchmark').resolve(strict=True)
                for label, directory in [('baseline', args.baseline_build), ('candidate', args.candidate_build)]}
    env_changes = {'PACIFICDB_ENVIRONMENT': 'development', 'WAL_ENABLED': 'true',
                   'WAL_FSYNC_ENABLED': 'true', 'WAL_FLUSH_ON_COMMIT': 'true',
                   'WAL_GROUP_COMMIT': 'false', 'VECTOR_HNSW_ENABLED': 'false'}
    env = {**os.environ, **env_changes}
    runs = []
    expected_answers = None
    # Alternate order to reduce systematic ordering bias; only three pairs.
    order = ['baseline', 'candidate', 'candidate', 'baseline', 'baseline', 'candidate']
    for index, label in enumerate(order):
        command = [str(binaries[label]), '10000', '10', '25']
        log = output / f'{index}-{label}.log'
        with log.open('w') as stream:
            process = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            def timeout(signum, frame):
                raise TimeoutError('benchmark exceeded 120 seconds')
            previous_handler = signal.signal(signal.SIGALRM, timeout)
            signal.alarm(120)
            try:
                _, status, usage = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
            except BaseException:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise
            finally:
                signal.alarm(0)
                signal.signal(signal.SIGALRM, previous_handler)
        record = {'label': label, 'command': command, 'exit_code': process.returncode,
                  'peak_rss_kib': usage.ru_maxrss, 'status': 'FAIL'}
        lines = [line for line in log.read_text().splitlines() if line.startswith('VECTOR_BENCHMARK_JSON ')]
        if process.returncode == 0 and len(lines) == 1:
            result = json.loads(lines[0].split(' ', 1)[1])
            record['result'] = result
            if expected_answers is None:
                expected_answers = result['answers']
            if (result['rows'] == 10000 and result['dimensions'] == 25 and
                    result['reported_wal_physical_syncs'] == 20 and
                    result['answers'] == expected_answers):
                record['status'] = 'PASS'
        runs.append(record)
        (output / 'runs.json').write_text(json.dumps(runs, indent=2))
        if record['status'] != 'PASS':
            raise SystemExit(f'failed run: {log}; raw failure retained')
    summary = {'status': 'PASS', 'scope': 'synthetic standalone paired replay only',
               'rows': 10000, 'dimensions': 25, 'batch_size': 500, 'repeats_per_build': 3,
               'environment': env_changes, 'results': {}, 'runs': runs,
               'limitations': ['three pairs, no tail latency claim', 'load includes input generation',
                               'not RF3 or sustained mixed traffic', 'paired answers, not independent ground truth']}
    for label, binary in binaries.items():
        selected = [r for r in runs if r['label'] == label]
        summary['results'][label] = {
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'median_load_seconds': statistics.median(r['result']['load_seconds'] for r in selected),
            'median_query_p50_ms': statistics.median(r['result']['p50_ms'] for r in selected),
            'maximum_peak_rss_kib': max(r['peak_rss_kib'] for r in selected),
            'reported_wal_physical_syncs_per_run': 20}
    (output / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary['results'], indent=2))


if __name__ == '__main__':
    main()
