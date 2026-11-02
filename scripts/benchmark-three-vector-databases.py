#!/usr/bin/env python3
"""Standalone, sequential three-database trial. Never a release certificate.

Reuses the existing public PacificDB/pgvector workloads without changing engines.
Qdrant cosine vectors are returned normalized; their values are checked as such.
"""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
os.environ['OMP_NUM_THREADS']='1'
import argparse
import concurrent.futures
import hashlib
import http.client
import importlib.util
import json
from pathlib import Path
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
import traceback

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sdk/python'))
spec=importlib.util.spec_from_file_location('paired_benchmark',Path(__file__).with_name('benchmark-community-v12.py'))
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
np,psutil=b.np,b.psutil


def payload(i):
    return {'kind':'vector','group_id':i%100,'title':'ocean shared' if i%10 else 'rare ocean',
            'active':bool(i%2),'optional':None if i%3 else i}


def validate(docs,q,base,truth,kind,group=None,overrides=None,deleted=None):
    ids=[int(d['id']) for d in docs]
    assert len(ids)==len(set(ids))==10,ids
    assert all(0<=i<len(base) for i in ids)
    assert not deleted or not(set(ids)&deleted)
    if group is not None:assert all(i%100==group for i in ids)
    expected=np.asarray([overrides.get(i,base[i]) if overrides else base[i] for i in ids],dtype=np.float64)
    for i,doc in zip(ids,docs):
        for key,value in payload(i).items():assert doc[key]==value,(i,key)
    vectors=np.asarray([d['vector'] for d in docs],dtype=np.float64)
    assert np.isfinite(vectors).all() and np.isfinite([d['score'] for d in docs]).all()
    if kind=='qdrant':
        np.testing.assert_allclose(vectors,expected/np.linalg.norm(expected,axis=1)[:,None],atol=2e-6,rtol=2e-6)
    else:np.testing.assert_array_equal(vectors.astype(np.float32),expected.astype(np.float32))
    scores=expected@q.astype(np.float64)/(np.linalg.norm(expected,axis=1)*np.linalg.norm(q.astype(np.float64)))
    np.testing.assert_allclose([d['score'] for d in docs],scores,atol=2e-6,rtol=2e-6)
    assert np.all(np.diff(scores)<=2e-6)
    return {'ids':ids,'strict_recall':len(set(ids)&set(truth['ids']))/10,
            'tie_aware_recall':float(np.mean(scores>=truth['cutoff']-1e-6))}


def qdrant_ready(info,rows):
    return (info.get('status')=='green' and info.get('optimizer_status')=='ok' and
            info.get('points_count',0)>=rows and info.get('indexed_vectors_count',0)>=rows)


def mixed_status(successes,errors,writes):
    return 'PASS' if successes==360 and not errors and writes==40 else 'FAIL'


def qualified_recall(records,minimum):
    value=statistics.mean(r['tie_aware_recall'] for r in records)
    assert value>=minimum,{'recall':value,'required':minimum}
    return value


def append_event(path,row):
    with path.open('a') as log:
        log.write(json.dumps(row,allow_nan=True)+'\n');log.flush()


def attempt(server,q,mode,ef,qi,base,truth,phase,round_=None,connection=None,group=None,overrides=None,deleted=None):
    begin=time.perf_counter();row={'query':qi,'phase':phase,'round':round_};docs=None
    try:
        kwargs={}
        if connection is not None:kwargs['connection']=connection
        if group is not None:kwargs['group']=group
        docs,ms,execution=server.query(q,mode,ef,**kwargs)
        row.update({'ms':ms,'execution':execution})
        row.update(validate(docs,q,base,truth,server.kind,group=group,overrides=overrides,deleted=deleted));row['status']='PASS'
        return row
    except BaseException:
        row.update({'status':'FAIL','response':docs,'error':traceback.format_exc(),
                    'failed_attempt_wall_ms':(time.perf_counter()-begin)*1000})
        raise
    finally:
        # Outside the latency timer, append even failed validation/transport
        # attempts. Never discard preceding results when a round later fails.
        append_event(server.out/(phase+'-attempts.jsonl'),row)


class Native(b.Server):
    def load(self,base,batch_size):
        start=time.perf_counter();times=[]
        for offset in range(0,len(base),batch_size):
            docs=[{'id':str(i),'vector':base[i].tolist(),**payload(i)}
                  for i in range(offset,min(offset+batch_size,len(base)))]
            begin=time.perf_counter()
            if self.kind=='pacific':
                response=self.connection.insert_many('vectors',docs)
                assert response.get('failed',0)==0 and response['inserted']==len(docs),response
            else:
                with self.connection.transaction():
                    with self.connection.cursor().copy('COPY vectors(id,kind,embedding,payload,group_id) FROM STDIN') as copy:
                        for doc in docs:
                            data={k:v for k,v in doc.items() if k not in ('id','kind','vector')}
                            copy.write_row((doc['id'],doc['kind'],json.dumps(doc['vector']),json.dumps(data),doc['group_id']))
            ms=(time.perf_counter()-begin)*1000;times.append(ms)
            append_event(self.out/'load-acknowledgements.jsonl',{'offset':offset,'count':len(docs),'ms':ms,'status':'PASS'})
        elapsed=time.perf_counter()-start;begin=time.perf_counter()
        if self.kind=='pacific':self.connection.request({'action':'storage_flush'})
        else:self.connection.execute('CHECKPOINT');self.connection.execute('ANALYZE vectors')
        return {'rows':len(base),'batch_size':batch_size,'seconds_to_last_ack_including_preparation':elapsed,
          'rows_per_second':len(base)/elapsed,'batch_latency':b.stats(times),
          'post_load_finalization_seconds':time.perf_counter()-begin,
          'finalization':'storage_flush' if self.kind=='pacific' else 'CHECKPOINT and ANALYZE',
          'ack_boundary':'Durable committed batch; throughput stops at last ACK, before separately timed finalization',
          'storage':b.disk(self.root/'data')}


def qdrant_config_path(root):
    # Qdrant's configuration loader does not accept a .json extension, even
    # though JSON content is valid YAML. Never let it silently use defaults.
    return root/'config.yaml'


def qdrant_hnsw_config():
    # Qdrant 1.20 rejects thresholds below 10 KiB. At 10K+ 25D points,
    # this minimum still selects an indexed plan over the full dataset.
    return {'m':16,'ef_construct':200,'max_indexing_threads':4,'full_scan_threshold':10}


class HTTP:
    def __init__(self,port):self.c=http.client.HTTPConnection('127.0.0.1',port,timeout=120)
    def call(self,method,path,body=None):
        wire=None if body is None else json.dumps(body,allow_nan=False,separators=(',',':'))
        self.c.request(method,path,body=wire,headers={'Content-Type':'application/json'})
        r=self.c.getresponse();data=r.read();value=json.loads(data)
        if r.status>=400:raise RuntimeError(f'Qdrant HTTP {r.status}: {str(value)[:1000]}')
        return value
    def close(self):self.c.close()


class Qdrant(b.Server):
    def connect(self):return HTTP(self.port)
    def start(self,new=True):
        started=time.perf_counter();self.stop_monitor.clear()
        config={'log_level':'WARN','telemetry_disabled':True,
          'storage':{'storage_path':str(self.root/'data'),'snapshots_path':str(self.root/'snapshots'),
                     'performance':{'max_search_threads':4,'max_optimization_threads':1},
                     'optimizers':{'max_optimization_threads':1}},
          'service':{'host':'127.0.0.1','http_port':self.port,'grpc_port':None,'max_workers':4},
          'cluster':{'enabled':False}}
        config_file=qdrant_config_path(self.root);config_file.write_text(json.dumps(config))
        self.log=(self.out/f'server-{time.time_ns()}.log').open('w')
        self.proc=subprocess.Popen(['taskset','-c',','.join(map(str,self.args.server_cpus)),str(self.args.qdrant),
          '--config-path',str(config_file)],cwd=self.root,stdout=self.log,stderr=subprocess.STDOUT,start_new_session=True)
        def monitor():
            while not self.stop_monitor.wait(.5):
                try:
                    p=psutil.Process(self.proc.pid);mem=p.memory_full_info();cpu=p.cpu_times();io=p.io_counters()
                    self.memory.append({'time':time.monotonic(),'pss':mem.pss,'rss':mem.rss,
                      'live_process_user_cpu_seconds':cpu.user,'live_process_system_cpu_seconds':cpu.system,
                      'live_process_read_bytes':io.read_bytes,'live_process_write_bytes':io.write_bytes})
                    if mem.pss>6*1024**3:
                        self.guard_error='6GiB sampled PSS guard exceeded';os.killpg(self.proc.pid,signal.SIGINT);return
                except psutil.Error:pass
        self.thread=threading.Thread(target=monitor,daemon=True);self.thread.start()
        deadline=time.monotonic()+self.args.startup_timeout
        while True:
            if self.proc.poll() is not None:raise RuntimeError(f'Qdrant startup exited {self.proc.returncode}')
            try:
                self.connection=self.connect();self.version=self.connection.call('GET','/');break
            except Exception:
                if self.connection:self.connection.close();self.connection=None
                if time.monotonic()>deadline:raise
                time.sleep(.1)
        self.start_seconds=time.perf_counter()-started
    def setup(self):
        self.connection.call('PUT','/collections/vectors',{
          'vectors':{'size':self.dimensions,'distance':'Cosine','on_disk':False},
          'shard_number':1,'replication_factor':1,'write_consistency_factor':1,'on_disk_payload':False,
          'hnsw_config':qdrant_hnsw_config(),
          'optimizers_config':{'indexing_threshold':0,'default_segment_number':1,'max_optimization_threads':1}})
    def load(self,base,batch_size):
        start=time.perf_counter();times=[]
        for offset in range(0,len(base),batch_size):
            points=[{'id':i,'vector':base[i].tolist(),'payload':payload(i)} for i in range(offset,min(offset+batch_size,len(base)))]
            begin=time.perf_counter();r=self.connection.call('PUT','/collections/vectors/points?wait=true&ordering=strong',{'points':points})
            assert r['result']['status']=='completed',r;ms=(time.perf_counter()-begin)*1000;times.append(ms)
            append_event(self.out/'load-acknowledgements.jsonl',{'offset':offset,'count':len(points),'ms':ms,'status':'PASS'})
        elapsed=time.perf_counter()-start
        return {'rows':len(base),'batch_size':batch_size,'seconds_to_last_ack_including_preparation':elapsed,
          'rows_per_second':len(base)/elapsed,'batch_latency':b.stats(times),'storage':b.disk(self.root/'data'),
          'post_load_finalization_seconds':0,'finalization':'No explicit segment flush; synchronous WAL ACK already complete',
          'ack_boundary':'wait=true ordering=strong: Qdrant 1.20 update_worker_internal explicitly flushes WAL before apply. Native WAL-backed synchronous ACK; no physical-power-loss certification.'}
    def count(self):return self.connection.call('POST','/collections/vectors/points/count',{'exact':True})['result']['count']
    def build_index(self,mode):
        if mode=='exact':return {'index':'params.exact=true; no graph at this phase'}
        started=time.perf_counter()
        self.connection.call('PATCH','/collections/vectors',{'optimizers_config':{'indexing_threshold':1}})
        info=self.wait_graph(self.rows)
        return {'source_and_build_seconds':time.perf_counter()-started,'index':info}
    def wait_graph(self,rows):
        last=0;deadline=time.monotonic()+3700;resumed=False
        while time.monotonic()<deadline:
            info=self.connection.call('GET','/collections/vectors')['result']
            if info.get('status')=='grey' and not resumed:
                # An applied update resumes paused optimizers after restart.
                # Reapply one existing metadata value without changing logical
                # content; record this extra ACK and include its cost in readiness.
                # Config-only PATCH did not resume this pinned native fixture.
                point=self.rows-1;body={'payload':{'group_id':point%100},'points':[point]}
                begin=time.perf_counter()
                response=self.connection.call('POST','/collections/vectors/points/payload?wait=true&ordering=strong',body)
                b.save(self.out/'optimizer-resume.json',{'observed':info,'action':body,'response':response,
                  'time':time.time(),'ack_seconds':time.perf_counter()-begin,
                  'note':'One additional idempotent metadata-write ACK after crash, outside mixed workload counts.'})
                resumed=True
            if qdrant_ready(info,rows):return info
            if info.get('optimizer_status')!='ok':raise RuntimeError(str(info))
            if time.monotonic()-last>20:print('INDEX_STATE qdrant',info.get('status'),info.get('indexed_vectors_count'),flush=True);last=time.monotonic()
            time.sleep(.5)
        raise TimeoutError(f'Qdrant readiness: {info}')
    def plan(self,connection,mode,ef=100):pass
    def explain(self,q,mode):
        return {'request':{'exact':mode=='exact','hnsw_ef':'selected on disjoint tuning queries'},
                'collection':self.connection.call('GET','/collections/vectors')['result'],
                'limitation':'No per-query graph trace; exact=false allows planner choice. Healthy optimizer and reported index counts checked. Counts are approximate, not proof of physical graph coverage.'}
    def query(self,q,mode,ef=100,group=None,connection=None):
        c=connection or self.connection;request={'query':q.tolist(),'limit':10,'with_payload':True,'with_vector':True,
          'params':{'exact':mode=='exact','hnsw_ef':ef}}
        if group is not None:request['filter']={'must':[{'key':'group_id','match':{'value':group}}]}
        begin=time.perf_counter();r=c.call('POST','/collections/vectors/points/query',request)
        docs=[{'id':str(p['id']),'vector':p['vector'],'score':p['score'],**p['payload']} for p in r['result']['points']]
        return docs,(time.perf_counter()-begin)*1000,{'requested_mode':mode,'normalized_cosine_vectors':True,'server_seconds':r.get('time')}
    def update(self,i,vector,c):
        started=time.perf_counter();r=c.call('PUT','/collections/vectors/points?wait=true&ordering=strong',
          {'points':[{'id':i,'vector':vector.tolist(),'payload':payload(i)}]});assert r['result']['status']=='completed'
        return (time.perf_counter()-started)*1000
    def delete(self,i):
        r=self.connection.call('POST','/collections/vectors/points/delete?wait=true&ordering=strong',{'points':[i]})
        assert r['result']['status']=='completed'
    def fetch(self,i):
        rows=self.connection.call('POST','/collections/vectors/points',{'ids':[i],'with_payload':True,'with_vector':True})['result']
        return None if not rows else {'id':str(rows[0]['id']),'vector':rows[0]['vector'],**rows[0]['payload']}


def update(server,i,vector,c=None):
    return server.update(i,vector,c or server.connection) if server.kind=='qdrant' else b.update(server,i,vector,c)


def verify_records(server,overrides,deleted):
    if server.kind!='qdrant':return b.verify_mutations(server,overrides,deleted)
    hashes={}
    for i,vector in overrides.items():
        if i in deleted:continue
        row=server.fetch(i);assert row is not None
        expected=vector.astype(np.float64);expected/=np.linalg.norm(expected)
        np.testing.assert_allclose(row['vector'],expected,atol=2e-6,rtol=2e-6)
        hashes[str(i)]=hashlib.sha256(np.asarray(row['vector'],np.float32).tobytes()).hexdigest()
    for i in deleted:assert server.fetch(i) is None
    return {'changed_records_verified':len(hashes),'deleted_ids':sorted(deleted),'returned_normalized_float32_sha256':hashes}


def lifecycle(server,base,queries,ef):
    result={};overrides={};deleted=set();writes=[];readiness=[]
    def checkpoint(phase):b.save(server.out/'lifecycle.json',{'phase':phase,**result})
    for i in range(10):
        vector=queries[100+i];writes.append(update(server,i,vector));overrides[i]=vector
        begin=time.perf_counter()
        if server.kind=='pacific':server.wait('ann',60)
        readiness.append((time.perf_counter()-begin)*1000)
    result['updates']={'commit_latency':b.stats(writes),'post_ack_index_wait':b.stats(readiness)}
    for i in range(5):
        if server.kind=='qdrant':server.delete(i)
        elif server.kind=='pacific':assert server.connection.delete_one('vectors',{'id':str(i)}).get('status')=='deleted'
        else:assert server.connection.execute('DELETE FROM vectors WHERE id=%s RETURNING id',(str(i),)).fetchone()[0]==str(i)
        deleted.add(i)
    if server.kind=='pacific':server.wait('ann',60)
    assert server.count()==len(base)-5
    checkpoint('updates_and_deletes')
    reads=[];errors=[];write_times=[];lock=threading.Lock()
    def worker(w):
        c=server.connect();server.plan(c,'ann',ef)
        try:
            for op in range(100):
                event={'worker':w,'operation':op,'status':'RUNNING'}
                if op%10==0:
                    i=100+w*10+op//10;vector=queries[200+w*10+op//10];event.update({'type':'write','id':i})
                    try:
                        ms=update(server,i,vector,c);event.update({'ms':ms,'status':'PASS'})
                        with lock:overrides[i]=vector;write_times.append(ms)
                    except BaseException:
                        event.update({'status':'FAIL','error':traceback.format_exc()});raise
                    finally:append_event(server.out/f'mixed-worker-{w}.jsonl',event)
                else:
                    q=queries[100+(w*100+op)%1000];begin=time.perf_counter()
                    docs=None;event['type']='read'
                    try:
                        docs,ms,_=server.query(q,'ann',ef,connection=c)
                        ids=[int(d['id']) for d in docs];assert len(ids)==len(set(ids))==10 and not(set(ids)&deleted)
                        values=np.asarray([d['vector'] for d in docs],np.float64);scores=values@q.astype(float)/(np.linalg.norm(values,axis=1)*np.linalg.norm(q.astype(float)))
                        np.testing.assert_allclose([d['score'] for d in docs],scores,atol=2e-6,rtol=2e-6)
                        event.update({'status':'PASS','ids':ids,'ms':ms,'execution':_})
                        with lock:reads.append(ms)
                    except Exception as e:
                        event.update({'status':'FAIL','response':docs,'error':str(e)[:1000],'ms':(time.perf_counter()-begin)*1000})
                        with lock:errors.append({'error':str(e)[:1000],'ms':(time.perf_counter()-begin)*1000})
                    finally:append_event(server.out/f'mixed-worker-{w}.jsonl',event)
        finally:c.close()
    begin=time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(worker,range(4)))
    result['mixed']={'status':mixed_status(len(reads),errors,len(write_times)),'read_attempts':360,'read_successes':len(reads),
      'read_errors':errors,'successful_read_ms':reads,'write_ms':write_times,'successful_read_latency':b.stats(reads),
      'write_latency':b.stats(write_times),'wall_seconds':time.perf_counter()-begin,
      'note':'No exact snapshot recall claim during concurrent writes; cardinality/deletion/scores checked.'}
    server.refresh()
    if server.kind=='pacific':server.wait('ann',60)
    result['pre_crash_records']=verify_records(server,overrides,deleted);checkpoint('mixed_and_records_verified')
    server.stop(crash=True);preserved=server.root.with_name(server.root.name+'-before-restart')
    begin=time.perf_counter();shutil.copytree(server.root,preserved);result['dirty_copy_seconds']=time.perf_counter()-begin
    server.start(new=False);assert server.count()==len(base)-5
    result['recovery']={'connection_seconds':server.start_seconds,'count':server.count(),
                        'records':verify_records(server,overrides,deleted)};checkpoint('restart_records_verified')
    if server.kind=='pacific':
        state=server.connection.request({'action':'vectorIndexStatus','collection':'vectors','name':'ann'})['index'];assert state['state']=='ABSENT',state
        begin=time.perf_counter();server.connection.request({'action':'rebuildVectorIndex','collection':'vectors','name':'ann'});server.wait('ann')
        result['recovery']['ann_reconstruction_seconds']=time.perf_counter()-begin
    elif server.kind=='qdrant':
        result['recovery']['initial_index_info']=server.connection.call('GET','/collections/vectors')['result']
        begin=time.perf_counter();result['recovery']['ready_index_info']=server.wait_graph(len(base)-5)
        result['recovery']['ann_readiness_wait_seconds']=time.perf_counter()-begin
        result['recovery']['ann_reconstruction_seconds']=None
        result['recovery']['note']='Persistent graph plus any measured optimizer readiness wait; no zero rebuild assumption.'
    else:
        server.plan(server.connection,'ann',ef)
        result['recovery']['query_plan']=server.explain(queries[100],'ann')
        result['recovery']['ann_reconstruction_seconds']=0
    server.plan(server.connection,'ann',ef);records=[]
    for qi in range(100,110):
        truth=b.mutable_reference(base,queries[qi],overrides,deleted)
        records.append(attempt(server,queries[qi],'ann',ef,qi,base,truth,'recovery',overrides=overrides,deleted=deleted))
    recall=statistics.mean(r['tie_aware_recall'] for r in records)
    result['recovery'].update({'sampled_recall_at_10':recall,'queries':records})
    result['status']='PASS' if recall>=.95 and result['mixed']['status']=='PASS' else 'FAIL'
    checkpoint('finished');return result


def measured(server,base,queries,truth,mode,ef,count,rounds,warmups):
    server.plan(server.connection,mode,ef);result=[]
    for round_ in range(rounds):
        rows=[]
        for qi in range(warmups):server.query(queries[qi],'ann' if mode=='ann' else 'exact',ef)
        for qi in range(100,100+count):
            rows.append(attempt(server,queries[qi],mode,ef,qi,base,truth[qi],mode,round_+1))
        summary={'round':round_+1,**b.stats([r['ms'] for r in rows]),
          'recall_at_10':statistics.mean(r['tie_aware_recall'] for r in rows),
          'strict_recall':statistics.mean(r['strict_recall'] for r in rows),'records':rows}
        assert summary['recall_at_10']>=(1. if mode=='exact' else .95),summary['recall_at_10']
        result.append(summary);b.save(server.out/(mode+'-rounds.json'),result)
        print('MEASURED',server.kind,mode,round_+1,summary['p50_ms'],summary['recall_at_10'],flush=True)
    return result


def run_system(kind,args,base,queries,truth,result):
    out=args.output/kind;out.mkdir()
    server=Qdrant(kind,args,len(base),25,out) if kind=='qdrant' else Native(kind,args,len(base),25,out)
    record={'status':'RUNNING','phase':'startup','kind':kind};result['systems'][kind]=record
    def checkpoint(phase):
        record['phase']=phase;b.save(args.output/'summary.json',result);print('PHASE',kind,phase,flush=True)
    try:
        server.start();record['startup_seconds']=server.start_seconds;server.setup();checkpoint('ingestion')
        record['load']=server.load(base,args.batch_size);assert server.count()==len(base)
        if kind=='postgres':
            c=server.connection;record['versions']={'postgres':c.execute('SELECT version()').fetchone()[0],'pgvector':c.execute("SELECT extversion FROM pg_extension WHERE extname='vector'").fetchone()[0]}
            record['settings']={n:c.execute('SHOW '+n).fetchone()[0] for n in ['fsync','synchronous_commit','full_page_writes','shared_buffers','work_mem','maintenance_work_mem','max_parallel_workers_per_gather','max_parallel_maintenance_workers','jit']}
        elif kind=='pacific':record['wal_status']=server.connection.request({'action':'admin_wal_status'})
        else:record['version']=server.version
        checkpoint('exact_build');record['exact_build']=server.build_index('exact')
        checkpoint('exact_queries');record['exact']=measured(server,base,queries,truth,'exact',100,args.queries,args.rounds,args.warmups)
        checkpoint('ann_build');record['ann_build']=server.build_index('ann')
        checkpoint('ann_tuning');curves=[];selected=None
        for ef in [40,100,200,300,600,1000]:
            server.plan(server.connection,'ann',ef);rows=[]
            for qi in range(100):
                rows.append(attempt(server,queries[qi],'ann',ef,qi,base,truth[qi],'tuning',ef))
            recall=statistics.mean(r['tie_aware_recall'] for r in rows)
            curves.append({'ef_search':ef,'recall_at_10':recall,**b.stats([r['ms'] for r in rows]),'records':rows})
            b.save(out/'tuning.json',curves)
            if recall>=.97:selected=ef;break
        record['tuning']=curves;assert selected is not None,'ANN tuning failed prespecified .97 target'
        record['selected_ef_search']=selected;record['ann_plan']=server.explain(queries[0],'ann')
        checkpoint('ann_queries');record['ann']=measured(server,base,queries,truth,'ann',selected,args.queries,args.rounds,args.warmups)
        checkpoint('filtered_exact');mask=np.arange(len(base))%100==1;ids=np.where(mask)[0]
        filtered=b.reference(base[mask],queries[100:110]);rows=[];server.plan(server.connection,'exact')
        for qi,row in zip(range(100,110),filtered):
            t={'ids':[int(ids[i]) for i in row['ids']],'cutoff':row['cutoff']}
            checked=attempt(server,queries[qi],'exact',100,qi,base,t,'filtered-exact',group=1)
            assert checked['tie_aware_recall']==1.;rows.append(checked)
        record['filter_1_percent_exact']={**b.stats([r['ms'] for r in rows]),'records':rows}
        checkpoint('concurrency4')
        def worker(w):
            c=server.connect();server.plan(c,'ann',selected);rows=[]
            try:
                for i in range(min(args.queries,200)//4):
                    qi=100+i*4+w
                    rows.append(attempt(server,queries[qi],'ann',selected,qi,base,truth[qi],f'concurrency-worker-{w}',connection=c))
            finally:c.close()
            return rows
        begin=time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:rows=sum(list(pool.map(worker,range(4))),[])
        elapsed=time.perf_counter()-begin
        record['concurrency4']={**b.stats([r['ms'] for r in rows]),'wall_seconds':elapsed,'wall_qps':len(rows)/elapsed,'records':rows,
          'recall_at_10':statistics.mean(r['tie_aware_recall'] for r in rows)}
        b.save(out/'concurrency4.json',record['concurrency4'])
        qualified_recall(rows,.95)
        checkpoint('lifecycle');server.refresh();record['lifecycle']=lifecycle(server,base,queries,selected)
        record['status']=record['lifecycle']['status'];checkpoint('finished')
    except BaseException:
        record['status']='FAIL';record['error']=traceback.format_exc();(out/'failure.txt').write_text(record['error']);print(record['error'],flush=True)
    finally:
        try:server.stop()
        except BaseException:record['shutdown_error']=traceback.format_exc();record['status']='FAIL'
        record['resource_samples']=server.memory
        record['peak_sampled_pss_bytes']=max((x['pss'] for x in server.memory),default=0)
        record['peak_sampled_rss_bytes']=max((x['rss'] for x in server.memory),default=0)
        record['storage']=b.disk(server.root/'data') if (server.root/'data').exists() else None
        record['private_scratch_root']=str(server.root)
        b.save(args.output/'summary.json',result)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['build','pg','qdrant','publication','output','scratch']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--rows',type=int,choices=[10000,100000,1000000],default=1000000)
    p.add_argument('--queries',type=int,default=1000);p.add_argument('--rounds',type=int,default=3)
    p.add_argument('--warmups',type=int,default=10);p.add_argument('--batch-size',type=int,default=1000)
    p.add_argument('--systems',default='pacific,postgres,qdrant');p.add_argument('--startup-timeout',type=int,default=240)
    args=p.parse_args()
    for name in ['build','pg','qdrant','publication']:setattr(args,name,getattr(args,name).resolve(strict=True))
    args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
    if any(args.output.iterdir()):p.error('Evidence output must be empty; preserve previous attempts')
    args.scratch=args.scratch.resolve();args.scratch.mkdir(parents=True,exist_ok=True)
    assert args.publication not in args.scratch.parents and args.scratch!=args.publication
    assert 10<=args.queries<=1000 and 1<=args.rounds<=5 and 1<=args.batch_size<=10000 and 1<=args.startup_timeout<=3600
    cpus=sorted(os.sched_getaffinity(0));assert len(cpus)>=6;args.server_cpus=cpus[:4]
    kinds=args.systems.split(',');assert len(kinds)==len(set(kinds)) and set(kinds)<=set(['pacific','postgres','qdrant'])
    manifest=json.loads((args.publication/'data/manifest.json').read_text())
    for name in ['base.npy','queries.npy','truth.npz']:assert b.digest(args.publication/'data'/name)==manifest['hashes'][name]
    base=np.load(args.publication/'data/base.npy',mmap_mode='r')[:args.rows];queries=np.load(args.publication/'data/queries.npy')
    frozen=np.load(args.publication/'data/truth.npz');truth=b.reference(base,queries,frozen if args.rows==1000000 else None)
    result={'status':'RUNNING','scope':'Self-run standalone warm-cache three-database trial; not production certification',
      'started_at':time.time(),'input_manifest':manifest,'rows':args.rows,'queries':args.queries,'rounds':args.rounds,'warmups':args.warmups,
      'system_order':kinds,'server_cpus':args.server_cpus,'client_cpus':cpus[4:6],
      'protocol':{'dimensions':25,'metric':'cosine','top_k':10,'M':16,'ef_construction':200,'sampled_pss_guard_gib':6,
        'tuning_queries':'0..99','measured_queries':f'100..{99+args.queries}','tuning_recall':.97,'held_out_recall':.95,
        'payload':'Full vector and common metadata; Qdrant returns cosine-normalized vector, independently verified.',
        'durability':'PostgreSQL/PacificDB fsync+commit ON; Qdrant wait=true/strong explicitly flushes WAL before apply (pinned 1.20 source). Native synchronous WAL acknowledgements; no physical-power-loss claim.',
        'limitations':['Different native protocols: TCP/SQL/HTTP','One server at a time, fixed order; three query rounds are not independent full index-build repetitions',
          'Shared workstation background load recorded, not controlled; no cache dropping','No RF3/clustered or physical-power-loss claim','25D dataset is not representative of all embedding models']},
      'versions':{'pacific_build':json.loads(subprocess.check_output([str(args.build/'db_engine'),'--build-info'],text=True)),
        'postgres':subprocess.check_output([str(args.pg/'postgres'),'--version'],text=True).strip(),
        'qdrant':subprocess.check_output([str(args.qdrant),'--version'],text=True).strip()},
      'sha256':{'pacific':b.digest(args.build/'db_engine'),'postgres':b.digest(args.pg/'postgres'),'pgvector':b.digest(args.pg.parent/'lib/vector.so'),
        'qdrant':b.digest(args.qdrant),'driver':b.digest(__file__),'reused_helper':b.digest(Path(__file__).with_name('benchmark-community-v12.py'))},
      'host':{'platform':platform.platform(),'memory':psutil.virtual_memory()._asdict(),'loadavg':os.getloadavg()},'systems':{}}
    b.save(args.output/'methodology.json',result);b.save(args.output/'ground-truth.json',truth)
    os.sched_setaffinity(0,cpus[4:6]);stop=threading.Event()
    def heartbeat():
        while not stop.wait(10):
            b.save(args.output/'heartbeat.json',{'time':time.time(),'pid':os.getpid(),
              'phases':{k:v.get('phase') for k,v in result['systems'].items()},'memory':psutil.virtual_memory()._asdict(),'loadavg':os.getloadavg()})
    t=threading.Thread(target=heartbeat,daemon=True);t.start()
    try:
        for kind in kinds:run_system(kind,args,base,queries,truth,result)
        result['status']='PASS' if len(result['systems'])==len(kinds) and all(v['status']=='PASS' for v in result['systems'].values()) else 'FAIL'
    finally:
        stop.set();t.join(2);os.sched_setaffinity(0,cpus)
        result['finished_at']=time.time();b.save(args.output/'summary.json',result)
    print('FINISHED',result['status'],flush=True)
    return 0 if result['status']=='PASS' else 1

if __name__=='__main__':raise SystemExit(main())
