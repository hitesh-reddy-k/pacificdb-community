#!/usr/bin/env python3
"""Reproducible, self-run Community candidate comparison; never a release certificate."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
os.environ['OMP_NUM_THREADS']='1'
import argparse,concurrent.futures,hashlib,json,math,platform,shutil,signal,socket,statistics,subprocess,threading,time,traceback
from pathlib import Path
import numpy as np
import psutil,psycopg
from pacificdb import PacificDB

def save(path,value):
    temporary=Path(str(path)+'.tmp');temporary.write_text(json.dumps(value,indent=2));temporary.replace(path)
def digest(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def stats(values):
    if not values:return {'samples':0}
    result={'samples':len(values),'p50_ms':statistics.median(values),'mean_ms':statistics.mean(values),
      'serial_qps':len(values)/sum(values)*1000,'p95_ms':None,'p99_ms':None}
    if len(values)>=100:result['p95_ms']=float(np.percentile(values,95))
    if len(values)>=1000:result['p99_ms']=float(np.percentile(values,99))
    return result

def disk(root):
    result={'bytes':0,'files':0,'wal_bytes':0}
    for path in root.rglob('*'):
        if path.is_symlink():raise RuntimeError('benchmark root symlink refused')
        if path.is_file():
            size=path.stat().st_size;result['bytes']+=size;result['files']+=1
            if path.suffix=='.wal' or 'pg_wal' in path.parts or path.name=='log.bin':result['wal_bytes']+=size
    return result

class Server:
    def __init__(self,kind,args,rows,dimensions,out):
        self.kind,self.args,self.rows,self.dimensions,self.out=kind,args,rows,dimensions,out
        self.metric=getattr(args,"metric","cosine")
        self.root=args.scratch/f'{kind}-{rows}-{time.time_ns()}';self.root.mkdir(parents=True)
        self.port=port();self.proc=None;self.log=None;self.connection=None;self.stop_monitor=threading.Event();self.thread=None
        self.memory=[];self.guard_error=None
    def connect(self):
        if self.kind=='pacific':return PacificDB.connect(f'pacificdb://127.0.0.1:{self.port}/bench',timeout=120,pool_size=1)
        return psycopg.connect(host='127.0.0.1',port=self.port,dbname='postgres',user=os.environ.get('USER','pacificdb'),autocommit=True,prepare_threshold=None,connect_timeout=2,options='-c statement_timeout=120000')
    def start(self,new=True):
        started=time.perf_counter();self.stop_monitor.clear()
        if self.kind=='postgres':
            data=self.root/'data'
            if new:
                with (self.out/'initdb.log').open('w') as log:subprocess.run([str(self.args.pg/'initdb'),'-D',str(data),'--encoding=UTF8','--locale=C','-A','trust'],stdout=log,stderr=subprocess.STDOUT,check=True)
            cmd=[str(self.args.pg/'postgres'),'-D',str(data),'-h','127.0.0.1','-p',str(self.port),'-c','unix_socket_directories=',
              '-c','fsync=on','-c','synchronous_commit=on','-c','full_page_writes=on','-c','shared_buffers=512MB','-c','work_mem=64MB',
              '-c','maintenance_work_mem=1GB','-c','max_parallel_workers_per_gather=3','-c','max_parallel_workers=4',
              '-c','max_parallel_maintenance_workers=3','-c','jit=off','-c','max_wal_size=2GB']
            env=os.environ.copy()
        else:
            cmd=[str(self.args.build/'db_engine')]
            env={**os.environ,'PACIFICDB_ENVIRONMENT':'development','PACIFICDB_HOME':str(self.root),'DATA_ROOT':str(self.root/'data'),
              'BACKUP_ROOT':str(self.root/'backup'),'RESTORE_DIR':str(self.root/'restore'),'ENGINE_BIND_HOST':'127.0.0.1','ENGINE_HOST':'127.0.0.1',
              'ENGINE_PORT':str(self.port),'RAFT_BIND_HOST':'127.0.0.1','RAFT_LISTEN_PORT':str(port()),'RAFT_IS_LEADER':'1',
              'RAFT_NODE_ID':'bench-node','RAFT_CLUSTER_ID':f'community-benchmark-{self.rows}','MIN_QUORUM_SIZE':'1',
              'ENGINE_AUTH_REQUIRED':'false','TLS_ENABLED':'false','PACIFICDB_VECTOR_PREVIEW_ENABLED':'1','VECTOR_HNSW_ENABLED':'true',
              'WAL_ENABLED':'true','WAL_FSYNC_ENABLED':'true','WAL_FLUSH_ON_COMMIT':'true','WAL_CHECKSUM_ENABLED':'true',
              'RAFT_STANDALONE_BYPASS':'0','MAX_QUERY_TIMEOUT_SEC':'120','MAX_SCAN_ROWS':str(self.rows+100000),
              'MAX_RESULT_DOCS':'10000','ENGINE_CPU_CORES':'4','CONN_MIN_THREADS':'8','CONN_MAX_THREADS':'16','ADAPTIVE_ADMISSION':'0',
              'MAX_MEMORY_MB':'8192','PROMETHEUS_ENABLED':'false','AUDIT_LOGGING_ENABLED':'false'}
        self.log=(self.out/f'server-{time.time_ns()}.log').open('w')
        self.proc=subprocess.Popen(['taskset','-c',','.join(map(str,self.args.server_cpus)),*cmd],env=env,stdout=self.log,stderr=subprocess.STDOUT,start_new_session=True)
        def monitor():
            while not self.stop_monitor.wait(.25):
                try:
                    root=psutil.Process(self.proc.pid);processes=[root,*root.children(recursive=True)];pss=rss=0;user_cpu=system_cpu=read_bytes=write_bytes=0
                    for p in processes:
                        try:
                            info=p.memory_full_info();pss+=info.pss;rss+=info.rss
                            cpu=p.cpu_times();user_cpu+=cpu.user;system_cpu+=cpu.system
                            io=p.io_counters();read_bytes+=io.read_bytes;write_bytes+=io.write_bytes
                        except psutil.NoSuchProcess:pass
                    self.memory.append({'time':time.monotonic(),'pss':pss,'rss':rss,'live_process_user_cpu_seconds':user_cpu,
                      'live_process_system_cpu_seconds':system_cpu,'live_process_read_bytes':read_bytes,'live_process_write_bytes':write_bytes})
                    if pss>6*1024**3:self.guard_error='6GiB sampled PSS guard exceeded';os.killpg(self.proc.pid,signal.SIGINT);return
                except psutil.Error:pass
        self.thread=threading.Thread(target=monitor,daemon=True);self.thread.start()
        deadline=time.monotonic()+getattr(self.args,'startup_timeout',120)
        while True:
            if self.proc.poll() is not None:raise RuntimeError(f'{self.kind} startup exited {self.proc.returncode}')
            try:
                self.connection=self.connect()
                if self.kind=='pacific':assert self.connection.request({'action':'ping'})['status']=='pong'
                else:self.connection.execute('SELECT 1')
                break
            except Exception:
                if self.connection:self.connection.close();self.connection=None
                if time.monotonic()>deadline:raise
                time.sleep(.1)
        self.start_seconds=time.perf_counter()-started
        if self.kind=='postgres':self.connection.execute("SET statement_timeout='120s'")
    def stop(self,crash=False):
        if self.connection:self.connection.close();self.connection=None
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid,signal.SIGKILL if crash else signal.SIGINT)
            try:self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:os.killpg(self.proc.pid,signal.SIGKILL);self.proc.wait();raise RuntimeError('graceful shutdown timeout')
        self.stop_monitor.set()
        if self.thread:self.thread.join(timeout=2)
        if self.log:self.log.close();self.log=None
        if not crash and self.proc and self.proc.returncode!=0:raise RuntimeError(f'{self.kind} shutdown {self.proc.returncode}')
        if self.guard_error:raise RuntimeError(self.guard_error)
    def refresh(self):
        # Alternating engines can leave the inactive connection beyond its idle
        # timeout. Start each macro phase with a fresh session on both systems;
        # never retry writes or include connection setup in query latency.
        if self.connection:self.connection.close()
        self.connection=self.connect()
    def setup(self):
        if self.kind=='pacific':
            self.connection.create_database('bench')
            self.connection.request({'action':'createCollection','collection':'vectors','vector_schema':{'default':{'dimensions':self.dimensions,'metric':self.metric,'required':True}}})
        else:
            self.connection.execute('CREATE EXTENSION vector')
            self.connection.execute(f'CREATE TABLE vectors(id text PRIMARY KEY,kind text NOT NULL,embedding vector({self.dimensions}) NOT NULL,payload jsonb NOT NULL,group_id integer NOT NULL)')
    def load(self,base,batch_size):
        times=[];start=time.perf_counter()
        for offset in range(0,len(base),batch_size):
            docs=[{'id':str(i),'kind':'vector','vector':base[i].tolist(),'group_id':i%100,'title':'ocean shared' if i%10 else 'rare ocean',
                   'active':bool(i%2),'optional':None if i%3 else i} for i in range(offset,min(offset+batch_size,len(base)))]
            begin=time.perf_counter()
            if self.kind=='pacific':
                response=self.connection.insert_many('vectors',docs)
                assert response.get('failed',0)==0 and response['inserted']==len(docs),response
            else:
                with self.connection.transaction():
                    with self.connection.cursor().copy('COPY vectors(id,kind,embedding,payload,group_id) FROM STDIN') as copy:
                        for doc in docs:
                            payload={k:v for k,v in doc.items() if k not in ('id','kind','vector')}
                            copy.write_row((doc['id'],doc['kind'],json.dumps(doc['vector']),json.dumps(payload),doc['group_id']))
            times.append((time.perf_counter()-begin)*1000)
        if self.kind=='pacific':self.connection.request({'action':'storage_flush'})
        else:self.connection.execute('CHECKPOINT');self.connection.execute('ANALYZE vectors')
        elapsed=time.perf_counter()-start
        return {'rows':len(base),'batch_size':batch_size,'seconds_including_preparation_and_final_flush':elapsed,
          'rows_per_second':len(base)/elapsed,'batch_latency':stats(times),'ack_boundary':'one durable committed batch; no ANN index yet','storage':disk(self.root/'data')}
    def count(self):
        return self.connection.count('vectors')['count'] if self.kind=='pacific' else self.connection.execute('SELECT count(*) FROM vectors').fetchone()[0]
    def build_index(self,mode):
        if self.kind=='pacific':
            name='flat' if mode=='exact' else 'ann'
            if mode=='ann':self.connection.request({'action':'dropVectorIndex','collection':'vectors','name':'flat'})
            options={'name':name,'algorithm':'flat' if mode=='exact' else 'hnsw','dimensions':self.dimensions,
              'metric':self.metric,'max_elements':int(self.rows*1.25)+1000,'max_scan_rows':self.rows+100000,
              'max_source_bytes':512*1024**2,'max_index_bytes':512*1024**2 if mode=='exact' else 1024**3,
              'max_build_ms':3600000,'M':16,'ef_construction':200,'ef_search':600}
            if mode=='ann':options['maintenance']='incremental'
            start=time.perf_counter();self.connection.request({'action':'createVectorIndex','collection':'vectors','options':options})
            state=self.wait(name);result={'source_and_build_seconds':time.perf_counter()-start,'index':state}
        elif mode=='ann':
            self.connection.execute("SET statement_timeout='3600s'")
            start=time.perf_counter();self.connection.execute('CREATE INDEX vectors_hnsw ON vectors USING hnsw (embedding '+{'cosine':'vector_cosine_ops','l2':'vector_l2_ops','dot':'vector_ip_ops'}[self.metric]+') WITH (m=16,ef_construction=200)')
            result={'build_seconds':time.perf_counter()-start,'parameters':{'M':16,'ef_construction':200}}
            self.connection.execute('CHECKPOINT');self.connection.execute('ANALYZE vectors');self.connection.execute("SET statement_timeout='120s'")
        else:result={'index':'sequential/parallel exact scan; no ANN index'}
        print('INDEX_READY',self.kind,self.rows,mode,result.get('source_and_build_seconds',result.get('build_seconds')),flush=True)
        return result
    def wait(self,name,timeout=3700):
        deadline=time.monotonic()+timeout;last_progress=0
        while time.monotonic()<deadline:
            status=self.connection.request({'action':'vectorIndexStatus','collection':'vectors','name':name})['index']
            if status['state']=='READY':return status
            if status['state']=='FAILED':raise RuntimeError(json.dumps(status))
            if time.monotonic()-last_progress>20:
                print('INDEX_STATE',self.kind,self.rows,name,status['state'],flush=True);last_progress=time.monotonic()
            time.sleep(.1)
        raise TimeoutError('index readiness')
    def plan(self,connection,mode,ef=100):
        if self.kind=='postgres':
            connection.execute('SET enable_indexscan='+('off' if mode=='exact' else 'on'))
            connection.execute('SET enable_bitmapscan='+('off' if mode=='exact' else 'on'))
            connection.execute('SET enable_seqscan='+('on' if mode=='exact' else 'off'))
            connection.execute(f'SET hnsw.ef_search={int(ef)}')
    def explain(self,q,mode):
        if self.kind!='postgres':return {'plan':'explicit named '+('flat' if mode=='exact' else 'ann')+'; per-response execution verified'}
        operator={'cosine':'<=>','l2':'<->','dot':'<#>'}[self.metric]
        plan=self.connection.execute('EXPLAIN (FORMAT JSON) SELECT id,kind,embedding,payload FROM vectors ORDER BY embedding '+operator+' %s::vector LIMIT 10',(json.dumps(q.tolist()),)).fetchone()[0]
        rendered=json.dumps(plan)
        if mode=='ann':assert 'vectors_hnsw' in rendered,plan
        else:assert 'Index Scan' not in rendered and 'Bitmap' not in rendered,plan
        return plan
    def query(self,q,mode,ef=100,group=None,connection=None):
        c=connection or self.connection;begin=time.perf_counter()
        if self.kind=='pacific':
            request={'action':'queryVector','collection':'vectors','vector':q.tolist(),'k':10,'metric':self.metric,'search_mode':mode,
              'index_name':'flat' if mode=='exact' else 'ann','require_index':group is None}
            if mode=='ann':request['ef_search']=ef;request['allow_exact_fallback']=False
            if group is not None:request['filter']={'group_id':group}
            response=c.request(request);docs=response['data'];execution=response['search'];assert not response.get('partial'),response
            assert execution['executed_mode']==mode and (group is not None or not execution['fallback']),execution
        else:
            where='' if group is None else ' WHERE group_id=%s'
            params=(json.dumps(q.tolist()),) if group is None else (json.dumps(q.tolist()),group)
            operator={'cosine':'<=>','l2':'<->','dot':'<#>'}[self.metric];scoring='1-distance' if self.metric=='cosine' else '-distance'
            sql='SELECT id,kind,embedding::text,payload,'+scoring+' FROM (SELECT id,kind,embedding,payload,embedding '+operator+' %s::vector AS distance FROM vectors'+where+' ORDER BY distance LIMIT 10) AS nearest ORDER BY distance'
            rows=c.execute(sql,params).fetchall();docs=[{'id':r[0],'kind':r[1],'vector':json.loads(r[2]),**r[3],'score':r[4]} for r in rows];execution={'executed_mode':mode}
        elapsed=(time.perf_counter()-begin)*1000
        return docs,elapsed,execution

def validate(docs,q,base,truth,group=None,overrides=None,deleted=None,metric="cosine"):
    ids=[int(d['id']) for d in docs];assert len(ids)==len(set(ids))==10
    if group is not None:assert all(i%100==group for i in ids)
    assert not deleted or not(set(ids)&set(deleted))
    expected=np.asarray([overrides.get(i,base[i]) if overrides else base[i] for i in ids],np.float32)
    np.testing.assert_array_equal(np.asarray([d['vector'] for d in docs],np.float32),expected)
    b=expected.astype(np.float64);query=q.astype(np.float64);scores=(b@query)/(np.linalg.norm(b,axis=1)*np.linalg.norm(query)) if metric=="cosine" else b@query if metric=="dot" else -np.linalg.norm(b-query,axis=1)
    np.testing.assert_allclose([d['score'] for d in docs],scores,rtol=1e-6,atol=1e-6)
    assert np.all(np.diff(scores)<=1e-6)
    return {'strict_recall':len(set(ids)&set(truth['ids']))/10,'tie_aware_recall':float(np.mean(scores>=truth['cutoff']-1e-6)),'ids':ids}

def mutable_reference(base,query,overrides,deleted):
    values=base.astype(np.float64);q=query.astype(np.float64)
    for key,value in overrides.items():values[key]=value
    scores=(values@q)/(np.linalg.norm(values,axis=1)*np.linalg.norm(q))
    if deleted:scores[list(deleted)]=-np.inf
    ids=np.argpartition(scores,-10)[-10:];ids=ids[np.argsort(-scores[ids])]
    return {'ids':ids.tolist(),'cutoff':float(scores[ids[-1]])}

def update(server,id,vector,connection=None):
    c=connection or server.connection;start=time.perf_counter()
    if server.kind=='pacific':
        response=c.update_one('vectors',{'id':str(id)},{'$set':{'vector':vector.tolist()}})
        assert response.get('status')=='updated',response
    else:assert c.execute('UPDATE vectors SET embedding=%s::vector WHERE id=%s RETURNING id',(json.dumps(vector.tolist()),str(id))).fetchone()[0]==str(id)
    return (time.perf_counter()-start)*1000

def verify_mutations(server,overrides,deleted):
    def fetch(id):
        if server.kind=='pacific':return server.connection.find_one('vectors',{'id':str(id)})
        row=server.connection.execute('SELECT id,embedding::text FROM vectors WHERE id=%s',(str(id),)).fetchone()
        return None if row is None else {'id':row[0],'vector':json.loads(row[1])}
    checked=[];hashes={}
    for id,expected in sorted(overrides.items()):
        if id in deleted:continue
        doc=fetch(id);assert doc is not None and doc['id']==str(id),(id,doc)
        actual=np.asarray(doc['vector'],np.float32)
        np.testing.assert_array_equal(actual,expected,err_msg=f'acknowledged vector mismatch id={id}')
        checked.append(id);hashes[str(id)]=hashlib.sha256(actual.tobytes()).hexdigest()
    for id in sorted(deleted):assert fetch(id) is None,f'deleted record returned id={id}'
    return {'changed_records_verified':len(checked),'changed_ids':checked,'deleted_ids':sorted(deleted),'float32_vector_sha256':hashes}

def extended(server,base,queries,ef):
    overrides={};deleted=set();result={};commits=[];visibility=[]
    checkpoint=lambda phase:save(server.out/'extended-progress.json',{'phase':phase,**result})
    for i in range(10):
        vector=queries[100+i];commits.append(update(server,i,vector));overrides[i]=vector
        start=time.perf_counter()
        if server.kind=='pacific':server.wait('ann',timeout=60)
        visibility.append((time.perf_counter()-start)*1000)
    result['updates']={'count':10,'commit_latency':stats(commits),'post_ack_ann_ready_wait':stats(visibility),
      'commit_latencies_ms':commits,'post_ack_ann_ready_wait_ms':visibility,
      'contract':'Pacific asynchronous index readiness versus PostgreSQL synchronous index update; compare both costs'}
    checkpoint('updates_completed')
    server.plan(server.connection,'ann',ef);checks=[]
    for i in range(10):
        q=queries[100+i];truth=mutable_reference(base,q,overrides,deleted);docs,ms,execution=server.query(q,'ann',ef)
        assert str(i) in [d['id'] for d in docs]
        checks.append({'ms':ms,**validate(docs,q,base,truth,overrides=overrides,deleted=deleted)})
    result['post_update_ann']={'records':checks,'recall_at_10':statistics.mean(r['tie_aware_recall'] for r in checks)}
    assert result['post_update_ann']['recall_at_10']>=.95
    checkpoint('post_update_reference_verified')
    deletion=[]
    for i in range(5):
        begin=time.perf_counter()
        if server.kind=='pacific':
            response=server.connection.delete_one('vectors',{'id':str(i)});assert response.get('status')=='deleted',response
        else:assert server.connection.execute('DELETE FROM vectors WHERE id=%s RETURNING id',(str(i),)).fetchone()[0]==str(i)
        deletion.append((time.perf_counter()-begin)*1000);deleted.add(i)
    if server.kind=='pacific':server.wait('ann',timeout=60)
    assert server.count()==len(base)-5
    result['deletes']={'commit_latency':stats(deletion),'verified_count':server.count()}
    checkpoint('deletes_completed')
    # 360 reads and 40 writes, four clients. During mutation, index lag is an
    # explicit outcome and is included in availability/latency statistics.
    errors=[];reads=[];writes=[];state_lock=threading.Lock()
    def mixed(worker):
        c=server.connect();server.plan(c,'ann',ef);local=[]
        try:
            for operation in range(100):
                if operation%10==0:
                    id=100+worker*10+operation//10;vector=queries[200+worker*10+operation//10]
                    ms=update(server,id,vector,c)
                    with state_lock:overrides[id]=vector;writes.append(ms)
                else:
                    qi=100+(worker*100+operation)%min(1000,len(queries)-100);start=time.perf_counter()
                    try:
                        docs,ms,execution=server.query(queries[qi],'ann',ef,connection=c)
                        ids=[int(d['id']) for d in docs];assert len(ids)==len(set(ids))==10 and not(set(ids)&deleted)
                        values=np.asarray([d['vector'] for d in docs],np.float64);q=queries[qi].astype(np.float64)
                        scores=(values@q)/(np.linalg.norm(values,axis=1)*np.linalg.norm(q))
                        np.testing.assert_allclose([d['score'] for d in docs],scores,atol=1e-6,rtol=1e-6)
                        with state_lock:reads.append(ms)
                    except Exception as error:
                        message=str(error)
                        if server.kind!='pacific' or not any(x in message for x in ['unavailable','freshness_changed','source_changing','visibility_changed']):raise
                        with state_lock:errors.append({'error':message[:500],'ms':(time.perf_counter()-start)*1000})
        finally:c.close()
    start=time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(mixed,range(4)))
    result['mixed_90_read_10_write']={'reads_attempted':360,'read_successes':len(reads),'read_errors':errors,'successful_read_latency':stats(reads),
      'write_latency':stats(writes),'write_latencies_ms':writes,'successful_read_latencies_ms':reads,
      'wall_seconds':time.perf_counter()-start,'successful_read_fraction':len(reads)/360,
      'note':'concurrent changing snapshots: returned payload scores checked; full snapshot recall not inferred'}
    checkpoint('mixed_workload_completed')
    server.refresh()
    if server.kind=='pacific':server.wait('ann',timeout=60)
    result['before_crash_mutation_verification']=verify_mutations(server,overrides,deleted)
    result['storage_after_mutations']=disk(server.root/'data')
    checkpoint('before_crash_verified')
    # SIGKILL only this owned disposable server; no acknowledged write is removed.
    start=time.perf_counter();server.stop(crash=True)
    begin=time.perf_counter();preserved=server.root.with_name(server.root.name+'-before-restart')
    shutil.copytree(server.root,preserved)
    result['preserved_dirty_fixture']={'root':str(preserved),'copy_seconds':time.perf_counter()-begin,
      'note':'forensic copy outside server startup timer; total stop/restart wall time includes this copy; warm-cache recovery only'}
    checkpoint('crashed_fixture_preserved');server.start(new=False)
    result['crash_recovery']={'restart_to_connection_seconds':server.start_seconds,'total_stop_restart_seconds':time.perf_counter()-start}
    assert server.count()==len(base)-5;result['crash_recovery']['count']=server.count()
    result['crash_recovery']['mutation_verification']=verify_mutations(server,overrides,deleted)
    checkpoint('after_crash_records_verified')
    if server.kind=='pacific':
        status=server.connection.request({'action':'vectorIndexStatus','collection':'vectors','name':'ann'})['index'];result['crash_recovery']['initial_ann_state']=status['state']
        assert status['state']=='ABSENT',status
        try:server.query(queries[100],'ann',ef);raise AssertionError('missing index silently succeeded')
        except AssertionError:raise
        except Exception as error:assert 'unavailable' in str(error)
        checkpoint('after_crash_ann_absent')
        start=time.perf_counter();server.connection.request({'action':'rebuildVectorIndex','collection':'vectors','name':'ann'});server.wait('ann')
        result['crash_recovery']['ann_reconstruction_seconds']=time.perf_counter()-start
    else:result['crash_recovery']['ann_reconstruction_seconds']=0;result['crash_recovery']['initial_ann_state']='persistent index available'
    server.plan(server.connection,'ann',ef);checks=[]
    for qi in range(100,110):
        truth=mutable_reference(base,queries[qi],overrides,deleted);docs,ms,execution=server.query(queries[qi],'ann',ef)
        checked=validate(docs,queries[qi],base,truth,overrides=overrides,deleted=deleted);checks.append(checked)
    result['crash_recovery']['ann_recall_at_10']=statistics.mean(r['tie_aware_recall'] for r in checks)
    assert result['crash_recovery']['ann_recall_at_10']>=.95
    checkpoint('recovery_completed')
    return result

def reference(base,queries,existing=None,metric="cosine"):
    if existing is not None:return [{'ids':row.tolist(),'cutoff':float(score[-1])} for row,score in zip(existing['ids'],existing['scores'])]
    b=base.astype(np.float64);norm=np.linalg.norm(b,axis=1);result=[]
    for q in queries:
        q=q.astype(np.float64);scores=(b@q)/(norm*np.linalg.norm(q)) if metric=="cosine" else b@q if metric=="dot" else -np.linalg.norm(b-q,axis=1);ids=np.argpartition(scores,-10)[-10:];ids=ids[np.argsort(-scores[ids])]
        result.append({'ids':ids.tolist(),'cutoff':float(scores[ids[-1]])})
    return result

def run(args):
    args.output.mkdir(parents=True,exist_ok=True)
    if any(args.output.iterdir()):raise RuntimeError('output must be empty; preserve failed runs')
    cpus=sorted(os.sched_getaffinity(0));assert len(cpus)>=6;args.server_cpus=cpus[:4];client_cpus=cpus[4:6]
    args.scratch.mkdir(parents=True,exist_ok=True)
    manifest=json.loads((args.publication/'data/manifest.json').read_text())
    for name in ['base.npy','queries.npy','truth.npz']:assert digest(args.publication/'data'/name)==manifest['hashes'][name]
    base=np.load(args.publication/'data/base.npy',mmap_mode='r');queries=np.load(args.publication/'data/queries.npy');existing=np.load(args.publication/'data/truth.npz')
    assert len(queries)>=100+args.queries
    result={'status':'RUNNING','scope':'self-run Community v1.2 candidate benchmark; no production certification',
      'source_manifest':manifest,'queries':args.queries,'rounds':args.rounds,'warmups':args.warmups,'server_cpus':args.server_cpus,'client_cpus':client_cpus,
      'candidate_sha256':digest(args.build/'db_engine'),'postgres_sha256':digest(args.pg/'postgres'),
      'pgvector_sha256':digest(args.pg.parent/'lib/vector.so'),'script_sha256':digest(__file__),
      'host':{'uname':platform.uname()._asdict(),'memory':psutil.virtual_memory()._asdict(),'loadavg':os.getloadavg()},
      'protocol':{'k':10,'metric':'cosine','dimensions':25,'recall_target':.95,'tuning_recall_threshold':.97,'M':16,'ef_construction':200,
        'startup_budget_seconds':args.startup_timeout,
        'durability':'fsync/commit acknowledgement on; standalone versus standalone','payload':'full vector and document payload; Pacific additionally returns engine metadata',
        'filesystem':'owned disposable roots under caller-provided scratch; no cache dropping','query_indices':list(range(100,100+args.queries)),
        'tail_limits':'p95 with >=100 observations; p99 with >=1000; repeated queries are not independent extra samples'},'datasets':[]}
    save(args.output/'methodology.json',result)
    os.sched_setaffinity(0,client_cpus)
    try:
      for size in args.sizes:
        out=args.output/f'rows-{size}';out.mkdir();data=base[:size]
        truth=reference(data,queries[:100+args.queries],existing if size==len(base) else None)
        save(out/'ground-truth.json',truth)
        servers={kind:Server(kind,args,size,data.shape[1],out/kind) for kind in ['pacific','postgres']}
        record={'rows':size,'status':'RUNNING','systems':{},'rounds':[]};result['datasets'].append(record);save(args.output/'summary.json',result)
        try:
          for kind in (['pacific','postgres'] if len(result['datasets'])%2 else ['postgres','pacific']):
            server=servers[kind];server.out.mkdir();print('START',size,kind,'load/build',flush=True);server.start();server.setup()
            record['systems'][kind]={'startup_seconds':server.start_seconds,'load':server.load(data,args.batch_size)};assert server.count()==size
            if kind=='pacific':record['systems'][kind]['runtime_wal_status']=server.connection.request({'action':'admin_wal_status'})
            record['systems'][kind]['indexes']={}
            if kind=='postgres':
              c=server.connection;record['systems'][kind]['versions']={'postgres':c.execute('SELECT version()').fetchone()[0],'pgvector':c.execute("SELECT extversion FROM pg_extension WHERE extname='vector'").fetchone()[0]}
              record['systems'][kind]['settings']={n:c.execute('SHOW '+n).fetchone()[0] for n in ['fsync','synchronous_commit','full_page_writes','shared_buffers','work_mem','maintenance_work_mem','max_parallel_workers_per_gather','max_parallel_maintenance_workers','jit']}
            save(args.output/'summary.json',result)
          selected={kind:100 for kind in servers}
          # Run one plan at a time. Pacific's declared staging reservations do
          # not permit simultaneous 1M flat/HNSW managers at this configuration.
          # Drop the flat derived index before ANN build; authoritative data stays.
          for mode in ['exact','ann']:
            for kind,server in servers.items():
              server.refresh();record['systems'][kind]['indexes'][mode]=server.build_index(mode)
              record['systems'][kind].setdefault('storage_after_build',{})[mode]=disk(server.root/'data')
              if mode=='ann':
                curves=[]
                for ef in [40,100,200,300,600]:
                  server.plan(server.connection,'ann',ef);raw=[]
                  for qi in range(100):
                    docs,ms,execution=server.query(queries[qi],'ann',ef);raw.append({'query':qi,'ms':ms,'execution':execution,**validate(docs,queries[qi],data,truth[qi])})
                  row={'ef_search':ef,**stats([r['ms'] for r in raw]),'recall_at_10':statistics.mean(r['tie_aware_recall'] for r in raw),'records':raw};curves.append(row)
                  if row['recall_at_10']>=.97:selected[kind]=ef;break
                record['systems'][kind]['ann_tuning']=curves
                assert curves[-1]['recall_at_10']>=.97,'ANN tuning did not reach prespecified .97 tuning threshold'
              server.plan(server.connection,mode,selected[kind])
              record['systems'][kind].setdefault('query_plans',{})[mode]=server.explain(queries[0],mode)
              save(args.output/'summary.json',result)
            for repeat in range(args.rounds):
              if mode=='exact':record['rounds'].append({'round':repeat+1,'order':['pacific','postgres'] if repeat%2==0 else ['postgres','pacific']})
              round_data=record['rounds'][repeat]
              for kind in round_data['order']:
                server=servers[kind];server.refresh();round_data.setdefault(kind,{})
                server.plan(server.connection,mode,selected[kind]);raw=[]
                for qi in range(args.warmups):server.query(queries[qi],mode,selected[kind])
                for qi in range(100,100+args.queries):
                  docs,ms,execution=server.query(queries[qi],mode,selected[kind]);row={'query':qi,'ms':ms,'execution':execution,**validate(docs,queries[qi],data,truth[qi])};raw.append(row)
                summary={**stats([r['ms'] for r in raw]),'recall_at_10':statistics.mean(r['tie_aware_recall'] for r in raw),'strict_recall':statistics.mean(r['strict_recall'] for r in raw),'records':raw}
                assert summary['recall_at_10']>=(1. if mode=='exact' else .95),summary
                round_data[kind][mode]=summary;save(args.output/'summary.json',result)
                print('MEASURED',size,repeat+1,kind,mode,round(summary['p50_ms'],3),summary['recall_at_10'],flush=True)
          # Selective filters use complete exact plans on both engines. Report the
          # Pacific storage-scan fallback, rather than mislabelling it as ANN.
          filtered_base=data[np.arange(size)%100==1];filtered_ids=np.arange(size)[np.arange(size)%100==1]
          filtered_truth=reference(filtered_base,queries[100:110]);filtered_truth=[{'ids':[int(filtered_ids[i]) for i in row['ids']],'cutoff':row['cutoff']} for row in filtered_truth]
          for kind,server in servers.items():
            server.refresh();server.plan(server.connection,'exact');rows=[]
            for i,qi in enumerate(range(100,110)):
              docs,ms,execution=server.query(queries[qi],'exact',group=1);checked=validate(docs,queries[qi],data,filtered_truth[i],group=1)
              assert checked['tie_aware_recall']==1.;rows.append({'query':qi,'ms':ms,'execution':execution,**checked})
            record['systems'][kind]['filter_1_percent_exact']={**stats([r['ms'] for r in rows]),'records':rows}
          # Warm-cache concurrent static ANN, one connection per worker.
          for kind,server in servers.items():
            def worker(worker_id):
              c=server.connect();server.plan(c,'ann',selected[kind]);rows=[]
              try:
                for i in range(min(args.queries,200)//4):
                  qi=100+(i*4+worker_id)%args.queries;docs,ms,execution=server.query(queries[qi],'ann',selected[kind],connection=c)
                  rows.append({'query':qi,'ms':ms,**validate(docs,queries[qi],data,truth[qi])})
              finally:c.close()
              return rows
            start=time.perf_counter()
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:rows=sum(list(pool.map(worker,range(4))),[])
            elapsed=time.perf_counter()-start;record['systems'][kind]['concurrency_4_ann']={**stats([r['ms'] for r in rows]),'wall_seconds':elapsed,'wall_qps':len(rows)/elapsed,'records':rows}
          for kind,server in servers.items():
            print('EXTENDED',size,kind,flush=True)
            server.refresh();record['systems'][kind]['extended']=extended(server,data,queries,selected[kind]);save(args.output/'summary.json',result)
          save(args.output/'summary.json',result)
          record['status']='PASS'
        except Exception as error:
          record['status']='FAIL';record['error']=f'{type(error).__name__}: {error}';(out/'failure.txt').write_text(traceback.format_exc());print('FAIL',size,record['error'],flush=True)
        finally:
          for kind,server in servers.items():
            try:server.stop()
            except Exception as error:record.setdefault('shutdown_errors',[]).append(f'{kind}: {error}');record['status']='FAIL'
            if kind in record['systems']:
              record['systems'][kind]['resource_samples']=server.memory
              record['systems'][kind]['peak_sampled_pss_bytes']=max((x['pss'] for x in server.memory),default=0)
              record['systems'][kind]['peak_sampled_rss_bytes']=max((x['rss'] for x in server.memory),default=0)
              record['systems'][kind]['retained_root']=str(server.root)
          save(args.output/'summary.json',result)
        if record['status']!='PASS':break
      result['status']='PASS' if len(result['datasets'])==len(args.sizes) and all(x['status']=='PASS' for x in result['datasets']) else 'FAIL'
      result['unexecuted_extended_workloads']=['higher dimensions/other metrics','sparse/named/hybrid capability comparison','actual physical power loss','multi-platform packaging and upgrade certification']
      result['release_qualification']='INCOMPLETE; this report is a self-run benchmark, not a release decision'
      save(args.output/'summary.json',result)
    finally:os.sched_setaffinity(0,cpus)
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--pg',type=Path,required=True)
    p.add_argument('--publication',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--scratch',type=Path,required=True)
    p.add_argument('--sizes',default='10000,100000,1000000');p.add_argument('--queries',type=int,default=1000);p.add_argument('--rounds',type=int,default=3)
    p.add_argument('--warmups',type=int,default=10);p.add_argument('--batch-size',type=int,default=1000)
    p.add_argument('--startup-timeout',type=int,default=120,help='Server connection-readiness budget in seconds; elapsed recovery is reported separately')
    args=p.parse_args()
    for name in ['build','pg','publication']:setattr(args,name,getattr(args,name).resolve(strict=True))
    args.output=args.output.resolve();args.scratch=args.scratch.resolve();args.sizes=[int(x) for x in args.sizes.split(',')]
    assert all(10000<=n<=1000000 for n in args.sizes) and 10<=args.queries<=1000 and 1<=args.rounds<=5 and 1<=args.batch_size<=10000
    assert 1<=args.startup_timeout<=3600
    assert args.publication not in args.output.parents and args.publication not in args.scratch.parents
    result=run(args);raise SystemExit(result['status']!='PASS')
if __name__=='__main__':main()
