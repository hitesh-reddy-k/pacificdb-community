#!/usr/bin/env python3
"""Read-only-source matched GloVe replay; disposable servers, no release claim."""
import argparse, hashlib, importlib.util, json, os, platform, shutil, signal, statistics
import subprocess, tempfile, threading, time
from pathlib import Path
from types import SimpleNamespace
import numpy as np
import psutil
import psycopg

SPEC = importlib.util.spec_from_file_location('checkpoint_replay', Path(__file__).with_name('benchmark-vector-checkpoint.py'))
replay = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(replay)
SQL = ('SELECT id,kind,embedding::text,1-distance FROM ('
       'SELECT id,kind,embedding,embedding <=> %s::vector AS distance '
       'FROM vectors ORDER BY distance LIMIT 10) AS nearest ORDER BY distance')

def save(path, value):
    path.write_text(json.dumps(value, indent=2))

def aggregate(rows):
    ms = [r['ms'] for r in rows]
    return {'queries': len(rows), 'p50_ms': statistics.median(ms),
            'mean_ms': statistics.mean(ms), 'p95_ms': None, 'p99_ms': None,
            'serial_qps': len(ms)/(sum(ms)/1000),
            'tie_aware_recall': statistics.mean(r['tie_aware_recall'] for r in rows),
            'strict_recall': statistics.mean(r['strict_recall'] for r in rows)}

def postgres(args, base, queries, truth, output):
    result = {'status': 'RUNNING', 'records': [], 'peak_sampled_pss_bytes': 0,
              'server_cpus': sorted(os.sched_getaffinity(0))[:4]}
    proc = None
    done = threading.Event()
    thread = None
    try:
        source = args.publication/'postgres-data-r1'
        if (source/'postmaster.pid').exists():
            raise RuntimeError('source PostgreSQL snapshot must be stopped')
        with tempfile.TemporaryDirectory(prefix='pgvector-v12-replay-') as temp:
            data = Path(temp)/'data'
            shutil.copytree(source, data)
            port = replay.free_port()
            command = [str(args.pg_bin/'postgres'), '-D', str(data), '-h', '127.0.0.1',
                       '-p', str(port), '-c', 'unix_socket_directories=', '-c', 'fsync=on',
                       '-c', 'synchronous_commit=on', '-c', 'full_page_writes=on']
            with (output/'postgres-server.log').open('w') as log:
                proc = subprocess.Popen(['taskset', '-c', ','.join(map(str,result['server_cpus'])), *command],
                                        stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                def monitor():
                    while not done.wait(.2):
                        try:
                            root = psutil.Process(proc.pid)
                            pss = sum(p.memory_full_info().pss for p in [root,*root.children(recursive=True)])
                            result['peak_sampled_pss_bytes'] = max(result['peak_sampled_pss_bytes'], pss)
                            if pss > 6*1024**3:
                                result['memory_guard_exceeded'] = True
                                os.killpg(proc.pid, signal.SIGINT)
                                return
                        except psutil.Error:
                            pass
                thread = threading.Thread(target=monitor,daemon=True);thread.start()
                deadline = time.monotonic()+90
                while True:
                    try:
                        conn = psycopg.connect(host='127.0.0.1',port=port,dbname='postgres',user='pacificdb',
                                               autocommit=True,connect_timeout=1,prepare_threshold=None)
                        break
                    except psycopg.OperationalError:
                        if proc.poll() is not None or time.monotonic()>deadline: raise
                        time.sleep(.1)
                with conn:
                    cpus = sorted(os.sched_getaffinity(0));os.sched_setaffinity(0,cpus[4:6])
                    try:
                        result['server_version']=conn.execute('SELECT version()').fetchone()[0]
                        result['pgvector_version']=conn.execute("SELECT extversion FROM pg_extension WHERE extname='vector'").fetchone()[0]
                        result['validated_count']=conn.execute('SELECT count(*) FROM vectors').fetchone()[0]
                        assert result['validated_count']==len(base)
                        settings=['fsync','synchronous_commit','full_page_writes','shared_buffers','work_mem',
                                  'max_parallel_workers_per_gather','max_parallel_workers','jit']
                        result['settings']={n:conn.execute('SHOW '+n).fetchone()[0] for n in settings}
                        result['indexes']=conn.execute("SELECT indexdef FROM pg_indexes WHERE tablename='vectors'").fetchall()
                        conn.execute("SET statement_timeout='60s'")
                        def query(qi, phase):
                            text=json.dumps(queries[qi].tolist(),separators=(',',':'))
                            start=time.perf_counter();rows=conn.execute(SQL,(text,)).fetchall()
                            # Match the PacificDB SDK timing boundary: payload
                            # decoding is part of end-to-end response latency.
                            docs=[{'id':r[0],'kind':r[1],'vector':json.loads(r[2]),'score':r[3]} for r in rows]
                            ms=(time.perf_counter()-start)*1000
                            ids=[int(r[0]) for r in rows]
                            assert len(set(ids))==len(ids)==10
                            assert all(r[1]=='vector' for r in rows)
                            returned=np.array([d['vector'] for d in docs],dtype=np.float32)
                            np.testing.assert_array_equal(returned,base[ids])
                            b=base[ids].astype(np.float64);q=queries[qi].astype(np.float64)
                            scores=(b@q)/(np.linalg.norm(b,axis=1)*np.linalg.norm(q))
                            np.testing.assert_allclose([r[3] for r in rows],scores,atol=1e-6,rtol=1e-6)
                            row={'query':qi,'phase':phase,'ms':ms,'ids':ids,
                                 'tie_aware_recall':float(np.mean(scores>=truth['scores'][qi,-1]-1e-6)),
                                 'strict_recall':len(set(ids)&set(truth['ids'][qi].tolist()))/10}
                            result['records'].append(row)
                            return row
                        conn.execute('SET enable_indexscan=off');conn.execute('SET enable_bitmapscan=off')
                        vector=json.dumps(queries[0].tolist())
                        result['exact_plan']=conn.execute('EXPLAIN (FORMAT JSON) '+SQL,(vector,)).fetchone()[0]
                        for qi in range(args.warmups):query(qi,'exact-warmup')
                        exact=[query(qi,'exact') for qi in range(100,100+args.queries)]
                        result['exact']=aggregate(exact)
                        assert result['exact']['tie_aware_recall']==1., 'exact ground truth mismatch'
                        conn.execute('SET enable_indexscan=on');conn.execute('SET enable_bitmapscan=on');conn.execute('SET enable_seqscan=off')
                        result['tuning']=[];selected=None
                        for ef in (40,100,200,400,800,1000):
                            conn.execute(f'SET hnsw.ef_search={ef}')
                            tuning=[query(qi,'ann-tuning') for qi in range(100)]
                            value={'ef_search':ef,**aggregate(tuning)};result['tuning'].append(value)
                            if value['tie_aware_recall']>=.95:selected=ef;break
                        assert selected is not None,'No tuning configuration reached Recall@10 >=0.95'
                        result['selected_ef_search']=selected
                        result['ann_plan']=conn.execute('EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) '+SQL,(vector,)).fetchone()[0]
                        assert 'vectors_hnsw' in json.dumps(result['ann_plan']), 'ANN index not used'
                        for qi in range(args.warmups):query(qi,'ann-warmup')
                        ann=[query(qi,'ann') for qi in range(100,100+args.queries)]
                        result['ann']=aggregate(ann)
                        assert result['ann']['tie_aware_recall']>=.95,'held-out ANN recall below target'
                        result['status']='PASS'
                    finally:os.sched_setaffinity(0,cpus)
                os.killpg(proc.pid,signal.SIGINT);proc.wait(timeout=30)
                assert proc.returncode==0,proc.returncode
    except Exception as error:
        result.update(status='FAIL',error=f'{type(error).__name__}: {error}')
    finally:
        if proc and proc.poll() is None:
            os.killpg(proc.pid,signal.SIGINT)
            try:proc.wait(timeout=30)
            except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
        done.set()
        if thread:thread.join(timeout=2)
        save(output/'postgres.json',result)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--publication',type=Path,required=True)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--pg-bin',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--queries',type=int,default=10)
    parser.add_argument('--warmups',type=int,default=2)
    parser.add_argument('--rounds',type=int,default=3)
    parser.add_argument('--pacific-index',choices=['none','flat'],default='none',help='Explicit public packed float64 exact index (preview)')
    parser.add_argument('--pg-only',action='store_true',help='Replay PostgreSQL phases without rerunning PacificDB')
    args=parser.parse_args()
    if not 1<=args.queries<=1000 or not 0<=args.warmups<=100 or not 1<=args.rounds<=5:
        parser.error('invalid workload bounds')
    for key in ('publication','build','pg_bin'):setattr(args,key,getattr(args,key).resolve(strict=True))
    args.output=args.output.resolve()
    if args.output==args.publication or args.publication in args.output.parents:parser.error('preserve source evidence')
    args.output.mkdir(parents=True,exist_ok=True)
    if any(args.output.iterdir()):parser.error('output must be empty')
    if len(os.sched_getaffinity(0))<6:parser.error('requires at least 6 CPUs for consistent affinity')
    manifest=json.loads((args.publication/'data/manifest.json').read_text())
    for name in ('base.npy','queries.npy','truth.npz'):
        assert replay.sha256(args.publication/'data'/name)==manifest['hashes'][name]
    base=np.load(args.publication/'data/base.npy',mmap_mode='r');queries=np.load(args.publication/'data/queries.npy',mmap_mode='r')
    truth=np.load(args.publication/'data/truth.npz')
    result={'status':'RUNNING','scope':'development search diagnostic, not production qualification',
            'source_manifest':manifest,'rows':len(base),'dimensions':base.shape[1],'metric':'cosine','k':10,
            'measured_query_indices':list(range(100,100+args.queries)),'warmups':args.warmups,
            'repeated_query_set':True,'tails':'not reported; small repeated query set',
            'filesystem':'disposable copies under tempfile; inspect host temp filesystem',
            'host':{'uname':platform.uname()._asdict(),'loadavg':os.getloadavg(),'memory':psutil.virtual_memory()._asdict()},
            'engine_sha256':replay.sha256(args.build/'db_engine'),'pg_sha256':replay.sha256(args.pg_bin/'postgres'),
            'pgvector_sha256':replay.sha256(args.pg_bin.parent/'lib/vector.so'),
            'script_sha256':replay.sha256(Path(__file__)),
            'postgres_prepare_threshold':None,
            'public_pacific_ann':'Not measured; optional preview ANN is separately gated and not qualified for release',
            'pacific_index':args.pacific_index,
            'replay_script_sha256':replay.sha256(Path(__file__).with_name('benchmark-vector-checkpoint.py')),
            'rounds':[]}
    save(args.output/'methodology.json',result)
    for rep in range(1,args.rounds+1):
        output=args.output/f'round-{rep}';output.mkdir()
        row={'round':rep};order=['pacific','postgres'] if rep%2 else ['postgres','pacific']
        if args.pg_only:order=['postgres']
        row['order']=order
        for name in order:
            print('START',rep,name,flush=True)
            if name=='pacific':
                a=SimpleNamespace(publication=args.publication,output=output,queries=args.queries,warmups=args.warmups,pacific_index=args.pacific_index)
                row[name]=replay.replay('pacific',args.build,a,base,queries,truth)
            else:row[name]=postgres(args,base,queries,truth,output)
            print('END',rep,name,row[name]['status'],flush=True)
        result['rounds'].append(row);save(args.output/'summary.json',result)
    result['status']='PASS' if all(r[n]['status']=='PASS' for r in result['rounds'] for n in r['order']) else 'FAIL'
    save(args.output/'summary.json',result)
    raise SystemExit(0 if result['status']=='PASS' else 1)

if __name__=='__main__':main()
