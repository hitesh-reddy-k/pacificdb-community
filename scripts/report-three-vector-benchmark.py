#!/usr/bin/env python3
"""Generate factual tables from the recorded three-system benchmark, never guesses."""
import argparse
import csv
import json
from pathlib import Path
import statistics

KINDS=('pacific','postgres','qdrant')
NAMES={'pacific':'PacificDB','postgres':'PostgreSQL + pgvector','qdrant':'Qdrant'}


def median_round(system,mode,key):
    rows=system.get(mode,[])
    values=[r.get(key) for r in rows]
    return statistics.median(values) if values and all(isinstance(x,(int,float)) for x in values) else None


def metrics(system):
    lifecycle=system.get('lifecycle',{})
    recovery=lifecycle.get('recovery',{})
    mixed=lifecycle.get('mixed',{})
    build=system.get('ann_build',{})
    result={
      'System qualification status':system.get('status','NOT MEASURED'),
      'Load rows/s (to last durable ACK)':system.get('load',{}).get('rows_per_second'),
      'Post-load finalization seconds (separate)':system.get('load',{}).get('post_load_finalization_seconds'),
      'Exact plan build seconds':system.get('exact_build',{}).get('source_and_build_seconds',0 if 'exact_build' in system else None),
      'Exact p50 ms (median of round medians)':median_round(system,'exact','p50_ms'),
      'Exact p95 ms (median of round p95)':median_round(system,'exact','p95_ms'),
      'Exact p99 ms (median of round p99)':median_round(system,'exact','p99_ms'),
      'ANN source/build seconds':build.get('source_and_build_seconds',build.get('build_seconds')),
      'Chosen ef_search':system.get('selected_ef_search'),
      'ANN p50 ms (median of round medians)':median_round(system,'ann','p50_ms'),
      'ANN p95 ms (median of round p95)':median_round(system,'ann','p95_ms'),
      'ANN p99 ms (median of round p99)':median_round(system,'ann','p99_ms'),
      'ANN Recall@10 (mean across rounds)':statistics.mean(r['recall_at_10'] for r in system['ann']) if system.get('ann') else None,
      '1% filter exact p50 ms (10 queries)':system.get('filter_1_percent_exact',{}).get('p50_ms'),
      'Four-client static ANN wall QPS':system.get('concurrency4',{}).get('wall_qps'),
      'Four-client static ANN Recall@10':system.get('concurrency4',{}).get('recall_at_10'),
      'Strict mixed ANN reads succeeded':f"{mixed['read_successes']}/{mixed['read_attempts']}" if mixed else None,
      'Mixed successful read p50 ms':mixed.get('successful_read_latency',{}).get('p50_ms'),
      'Mixed write ACK p50 ms':mixed.get('write_latency',{}).get('p50_ms'),
      'Crash restart to connection seconds':recovery.get('connection_seconds'),
      'Recovered acknowledged changed records':recovery.get('records',{}).get('changed_records_verified'),
      'ANN reconstruction seconds':recovery.get('ann_reconstruction_seconds'),
      'Post-restart index readiness wait seconds':recovery.get('ann_readiness_wait_seconds'),
      'Post-recovery sampled Recall@10 (10 queries)':recovery.get('sampled_recall_at_10'),
      'Peak sampled PSS MiB (whole lifecycle)':system.get('peak_sampled_pss_bytes',0)/1024**2 if 'peak_sampled_pss_bytes' in system else None,
      'Peak sampled RSS MiB (whole lifecycle)':system.get('peak_sampled_rss_bytes',0)/1024**2 if 'peak_sampled_rss_bytes' in system else None,
      'Live data-directory MiB (includes WAL)':system['storage']['bytes']/1024**2 if system.get('storage') else None,
    }
    return result


def cell(value):
    if value is None:return 'NOT MEASURED'
    if isinstance(value,float):return f'{value:.4f}'
    return str(value)


def render(data,output):
    output.mkdir(parents=True,exist_ok=True)
    summaries={k:metrics(data.get('systems',{}).get(k,{})) for k in KINDS}
    status=all(data.get('systems',{}).get(k,{}).get('status')=='PASS' for k in KINDS)
    labels=list(next(iter(summaries.values())).keys())
    with (output/'results.csv').open('w',newline='') as f:
        writer=csv.writer(f);writer.writerow(['Metric',*[NAMES[k] for k in KINDS]])
        for label in labels:writer.writerow([label,*[cell(summaries[k][label]) for k in KINDS]])
    widths=[max(len('Metric'),max(map(len,labels))),*[max(len(NAMES[k]),max(len(cell(summaries[k][label])) for label in labels)) for k in KINDS]]
    def row(values):return ' | '.join(value.ljust(width) for value,width in zip(values,widths))
    lines=['PACIFICDB / POSTGRESQL + PGVECTOR / QDRANT — RECORDED BENCHMARK',
      f"Dataset: {data.get('rows','NOT MEASURED')} vectors, cosine, 25 dimensions, K=10.",
      f"Held-out queries per round: {data.get('queries')}; rounds: {data.get('rounds')}.",
      'Qualification: '+('PASS for this measured workload' if status else 'FAIL / INCOMPLETE — inspect individual system outcomes'),
      'This is a self-run standalone warm-cache trial, not independent certification or production approval.','',
      row(['Metric',*[NAMES[k] for k in KINDS]]),'-+-'.join('-'*w for w in widths)]
    for label in labels:lines.append(row([label,*[cell(summaries[k][label]) for k in KINDS]]))
    lines.extend(['','READ BEFORE COMPARING',
      '- Exact and ANN are separate workloads. ANN tuning uses disjoint100queries; target held-out Recall@10 >=0.95.',
      '- p50/p95/p99 cells are medians of the recorded per-round statistics, not pooled independent samples or best runs.',
      '- Three query rounds reuse the same held-out queries/index. There is one full ingest/build per system, fixed sequential order.',
      '- Ingestion throughput ends at the last acknowledged batch for every database. Final flush/checkpoint/analyze is separate.',
      '- Same float32 inputs; PacificDB preserves legacy float64 numeric storage. Qdrant cosine returns normalized vectors.',
      '- Full vector and common metadata are requested. Native TCP/SQL/HTTP protocol and client parsing costs differ.',
      '- Four server CPUs/two client CPUs and a sampled6GiB PSS guard; no hard-cgroup equivalence claim.',
      '- Shared active workstation; background load, caches, build ordering and library differences can affect results.',
      '- PostgreSQL/PacificDB fsync+commit enabled. Qdrant1.20wait=true explicitly flushes WAL before applying updates.',
      '- SIGKILL/restart is process-crash testing, not physical power-loss proof.',
      '- PacificDB graph reconstruction is explicit. Qdrant persists graphs; its readiness wait and any extra idempotent metadata ACK are recorded.',
      '- Qdrant collection counts are approximate; readiness/healthyoptimizer are checked, but exact=false permits planner choice without a per-query graph trace.',
      '- Qdrant reconstruction NOT MEASURED means not independently isolated; use measured readiness wait, not an invented zero.',
      '- PostgreSQL summed RSS can count shared pages multiple times; use PSS for cross-process memory comparison.',
      '- Disk totals are live data directories incl preallocated WAL. They exclude forensic copies; PacificDB ANN graph remains in memory.',
      '- Filter/recovery samples are small (10); do not infer tail latency or broad recall confidence from them.',
      '- A25Dworkload does not establish768/1536Dbehavior, RF3/distributed performance, complete feature parity or production readiness.',
      '- All failed smoke attempts and validation controls are retained separately. These are not substituted for completed1Mresults.'])
    for kind in KINDS:
        system=data.get('systems',{}).get(kind,{})
        if system.get('error'):lines.extend(['',NAMES[kind]+' ERROR:',system['error']])
    (output/'REPORT.txt').write_text('\n'.join(lines)+'\n')
    result={'all_systems_pass':status,'rows':data.get('rows'),'systems':summaries,'source_status':data.get('status')}
    (output/'metrics.json').write_text(json.dumps(result,indent=2)+'\n');return result


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('summary',type=Path);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();data=json.loads(args.summary.read_text());render(data,args.output)
if __name__=='__main__':main()
