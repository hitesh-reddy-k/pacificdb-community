#!/usr/bin/env python3
"""Linux-only disposable mixed-load qualification with real authentication/TLS/WAL.

Requires a clean checkout matching --expected-commit, a matching engine binary,
a checksum-frozen public dataset, valid PNG/WAV/MP4 fixtures, Linux CPUs0-5,
and at least8GiB initially available RAM. Creates owned scratch data and tests
SIGKILL/rebuild recovery only for its own child. Never targets a running service.
No automatic request replay or PostgreSQL/Qdrant comparison.
"""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
os.environ['OMP_NUM_THREADS']='1'
import argparse, concurrent.futures, hashlib, json, math, platform, secrets, shutil, signal, socket, statistics, struct, subprocess, sys, threading, time, traceback, wave, zlib
from pathlib import Path
import numpy as np
import psutil
REPO=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(REPO/'sdk/python'))
from pacificdb import PacificDB
BASE=None
BINARY=None
EXPECTED=None
GIB=1024**3
def sha(p):
    with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def save(p,x):
    q=Path(str(p)+'.tmp-'+str(os.getpid())+'-'+str(threading.get_ident()));q.write_text(json.dumps(x,indent=2));q.replace(p)
def utc():return time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
def port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def pct(a):
    if not a:return {'samples':0}
    return {'samples':len(a),'p50_ms':float(np.percentile(a,50)), 'p95_ms':float(np.percentile(a,95)) if len(a)>=100 else None,'p99_ms':float(np.percentile(a,99)) if len(a)>=1000 else None,'max_ms':max(a)}
class Run:
    def __init__(self,args):
        self.args=args;self.root=args.root.resolve();self.root.mkdir(parents=True,exist_ok=True)
        if (self.root/'provenance.json').exists():raise RuntimeError('Evidence root already used; refusing overwrite')
        self.root.chmod(0o700);self.home=self.root/'engine';self.home.mkdir(mode=0o700)
        self.fixtures=args.fixtures.resolve();self.downloads=self.root/'downloads';self.downloads.mkdir()
        self.password=secrets.token_urlsafe(32);self.port=port();self.raft=port();self.proc=None;self.log=None
        self.stop=threading.Event();self.monitor_stop=threading.Event();self.lock=threading.Lock();self.records={};self.phase='INITIALIZING';self.errors=[];self.guard=None
        self.overrides={};self.deleted=set();self.doc_expected={};self.media_expected={};self.media_deleted=[];self.started=time.monotonic();self.phase_started=self.started
        self.events=(self.root/'operations.jsonl').open('a',buffering=1);self.resources=(self.root/'resources.jsonl').open('a',buffering=1)
        self.state=(self.root/'events.jsonl').open('a',buffering=1)
        self.host_resources=(self.root/'host-processes.jsonl').open('a',buffering=1)
        self.main=None;self.summaries=[];self.media_paths=[];self.monitor=None;self.expected_down=True
    def event(self,**x):
        with self.lock:self.state.write(json.dumps({'utc':utc(),**x})+'\n')
    def set_phase(self,name):
        self.phase=name;self.phase_started=time.monotonic();self.event(phase=name);self.heartbeat()
    def heartbeat(self):
        with self.lock:counts={k:{'success':v['ok'],'failed':v['failed']} for k,v in self.records.items()}
        save(self.root/'heartbeat.json',{'status':'RUNNING','utc':utc(),'phase':self.phase,'phase_elapsed_seconds':time.monotonic()-self.phase_started,'elapsed_seconds':time.monotonic()-self.started,'pid':os.getpid(),'engine_pid':self.proc.pid if self.proc else None,'operations':counts,'errors':len(self.errors),'guard':self.guard,'last_resources':getattr(self,'last_resources',None)})
    def connect(self):
        c=PacificDB(host='localhost',port=self.port,database='load',username='admin',password=self.password,use_tls=True,ca_file=str(self.root/'cert.pem'),timeout=120,pool_size=1)
        c.request({'action':'ping'});return c
    def start(self):
        env={**os.environ,'PACIFICDB_ENVIRONMENT':'development','PACIFICDB_HOME':str(self.home),'DATA_ROOT':str(self.home/'data'),'BACKUP_ROOT':str(self.home/'backup'),'RESTORE_DIR':str(self.home/'restore'),'ENGINE_BIND_HOST':'127.0.0.1','ENGINE_PORT':str(self.port),'RAFT_BIND_HOST':'127.0.0.1','RAFT_LISTEN_PORT':str(self.raft),'RAFT_CLUSTER_ID':'load-'+self.root.name,'RAFT_NODE_ID':'load-node','RAFT_IS_LEADER':'1','MIN_QUORUM_SIZE':'1','RAFT_STANDALONE_BYPASS':'0','ENGINE_AUTH_REQUIRED':'1','PACIFICDB_ENGINE_ADMIN_USERNAME':'admin','PACIFICDB_ENGINE_ADMIN_PASSWORD':self.password,'TLS_ENABLED':'1','TLS_REQUIRE_CLIENT_CERT':'0','TLS_CERT_PATH':str(self.root/'cert.pem'),'TLS_KEY_PATH':str(self.root/'key.pem'),'TLS_CA_PATH':str(self.root/'cert.pem'),'WAL_ENABLED':'true','WAL_FSYNC_ENABLED':'true','WAL_FLUSH_ON_COMMIT':'true','WAL_CHECKSUM_ENABLED':'true','PACIFICDB_VECTOR_PREVIEW_ENABLED':'1','VECTOR_HNSW_ENABLED':'true','MAX_QUERY_TIMEOUT_SEC':'120','MAX_SCAN_ROWS':str(self.args.rows+100000),'MAX_RESULT_DOCS':'10000','ENGINE_CPU_CORES':'4','CONN_MIN_THREADS':'8','CONN_MAX_THREADS':'48','MAX_CONNECTIONS':'96','MAX_MEMORY_MB':'4096','ADAPTIVE_ADMISSION':'1','PROMETHEUS_ENABLED':'false','AUDIT_LOGGING_ENABLED':'true','ENGINE_KEEPALIVE_MAX_REQUESTS':'10000'}
        self.log=(self.root/'engine.log').open('a',buffering=1)
        self.proc=subprocess.Popen(['taskset','-c','0-3',str(BINARY)],env=env,stdout=self.log,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+240
        while time.monotonic()<deadline:
            if self.proc.poll() is not None:raise RuntimeError('Engine exited during startup: '+str(self.proc.returncode))
            try:self.main=self.connect();self.expected_down=False;return
            except Exception:time.sleep(.25)
        raise TimeoutError('Engine startup')
    def stop_engine(self,crash=False):
        self.expected_down=True
        if self.main:self.main.close();self.main=None
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if crash else signal.SIGINT)
            try:self.proc.wait(timeout=90)
            except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait();raise RuntimeError('Graceful stop exceeded 90s')
            if not crash and self.proc.returncode!=0:raise RuntimeError('Nonzero graceful shutdown')
        if self.log:self.log.close();self.log=None
    def sample(self):
        last_host=0
        while not self.monitor_stop.wait(5):
            try:
                row={'utc':utc(),'phase':self.phase,'host_available_bytes':psutil.virtual_memory().available,'free_disk_bytes':shutil.disk_usage(self.root).free}
                client=psutil.Process();cm=client.memory_full_info();swap=psutil.swap_memory()
                row.update(client_rss=cm.rss,client_pss=cm.pss,client_swap=getattr(cm,'swap',0),host_swap_used=swap.used,host_swap_in=swap.sin,host_swap_out=swap.sout)
                try:row['memory_pressure']=(Path('/proc/pressure/memory').read_text()).strip()
                except OSError:pass
                try:
                    group=Path('/sys/fs/cgroup')/Path('/proc/self/cgroup').read_text().strip().split('::',1)[1].lstrip('/')
                    for field in ['memory.current','memory.swap.current','memory.events']:row['cgroup_'+field]=(group/field).read_text().strip()
                except (OSError,IndexError):pass
                if self.proc and self.proc.poll() is None:
                    p=psutil.Process(self.proc.pid);mem=p.memory_full_info();io=p.io_counters();cpu=p.cpu_times()
                    row.update(engine_rss=mem.rss,engine_pss=mem.pss,engine_swap=getattr(mem,'swap',0),engine_cpu_user=cpu.user,engine_cpu_system=cpu.system,engine_read_bytes=io.read_bytes,engine_write_bytes=io.write_bytes,engine_threads=p.num_threads(),engine_fds=p.num_fds())
                if time.monotonic()-last_host>=30 or row['host_available_bytes']<2*GIB:
                    processes=[]
                    for process in psutil.process_iter(['pid','name','memory_info']):
                        try:
                            info=process.info;meminfo=info.get('memory_info')
                            if meminfo:processes.append({'pid':info['pid'],'name':info.get('name'),'rss':meminfo.rss})
                        except (psutil.Error,OSError):pass
                    self.host_resources.write(json.dumps({'utc':row['utc'],'phase':self.phase,'largest_processes_by_RSS':sorted(processes,key=lambda x:x['rss'],reverse=True)[:15]})+'\n');last_host=time.monotonic()
                total=wal=0
                for parent,dirs,files in os.walk(self.home):
                    for name in files:
                        path=Path(parent)/name
                        try:n=path.stat().st_size
                        except FileNotFoundError:continue
                        total+=n
                        if path.suffix=='.wal' or name=='log.bin':wal+=n
                row.update(engine_disk_bytes=total,wal_bytes=wal)
                self.last_resources=row;self.resources.write(json.dumps(row)+'\n');self.heartbeat()
                reason=None
                if row.get('engine_pss',0)>6*GIB:reason='engine PSS exceeded 6GiB'
                if total>20*GIB:reason='owned engine disk exceeded 20GiB'
                if row['free_disk_bytes']<15*GIB:reason='host free disk below 15GiB'
                if row['host_available_bytes']<1*GIB:reason='host available RAM below 1GiB'
                if self.proc and self.proc.poll() is not None and not self.expected_down:reason='engine exited unexpectedly'
                if reason:self.guard=reason;self.stop.set();self.event(resource_guard=reason);return
            except psutil.NoSuchProcess as e:
                # Intentional SIGKILL/restart can race a successful poll() above.
                # Do not confuse disappearing owned engine telemetry with a
                # spontaneous exit; the restart/start routines verify the exit.
                if self.expected_down:continue
                self.guard='engine process disappeared during resource sample';self.stop.set();return
            except Exception as e:self.guard='resource monitor failed: '+str(e);self.stop.set();return
    def op(self,kind,fn,**extra):
        begin=time.monotonic();ok=False;error=None
        try:r=fn();ok=True;return r
        except Exception as e:error={'type':type(e).__name__,'code':getattr(e,'code',None),'message':str(e)[:1000]};raise
        finally:
            elapsed=(time.monotonic()-begin)*1000
            with self.lock:
                key=self.phase+'/'+kind;v=self.records.setdefault(key,{'ok':0,'failed':0,'latencies':[]});v['ok' if ok else 'failed']+=1;v['latencies'].append(elapsed)
                self.events.write(json.dumps({'utc':utc(),'phase':self.phase,'kind':kind,'ok':ok,'ms':elapsed,**extra,**({'error':error} if error else {})})+'\n')
                if error:self.errors.append(error)
    def prepare(self):
        self.set_phase('PROVENANCE')
        available=psutil.virtual_memory().available
        self.event(initial_host_available_bytes=available,required_initial_available_bytes=8*GIB)
        if available<8*GIB:raise RuntimeError('qualification_host_headroom_below_8GiB_at_start')
        commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=REPO,text=True).strip();dirty=subprocess.check_output(['git','status','--porcelain'],cwd=REPO,text=True)
        assert commit==EXPECTED and not dirty,(commit,dirty)
        build=json.loads(subprocess.check_output([str(BINARY),'--build-info'],text=True));assert build.get('gitCommit')==EXPECTED,build
        manifest=json.loads((BASE/'manifest.json').read_text())
        for name in ['base.npy','queries.npy','truth.npz']:assert sha(BASE/name)==manifest['hashes'][name],name
        self.base=np.load(BASE/'base.npy',mmap_mode='r')[:self.args.rows];self.queries=np.load(BASE/'queries.npy',mmap_mode='r')
        save(self.root/'provenance.json',{'utc':utc(),'source_commit':commit,'source_clean':not bool(dirty),'binary_sha256':sha(BINARY),'build_info':build,'dataset_manifest':manifest,'vector_rows':len(self.base),'document_rows':self.args.docs,'dimensions':25,'metric':'cosine','soak_seconds':self.args.seconds,'ramp_seconds':self.args.ramp_seconds,'concurrency_ramps':[1,8,32],'server_cpus':'0-3','client_cpus':'4-5','standalone_replicas':1,'transport':'authenticated TLS; trust private test CA','durability':'WAL enabled; fsync, flush-on-commit, checksum enabled; no standalone Raft bypass','admission':'enabled','engine_memory_budget_MiB':4096,'guards':{'engine_PSS_GiB':6,'owned_disk_GiB':20,'min_free_disk_GiB':15,'min_available_RAM_GiB':1},'host':{'platform':platform.platform(),'cpus':psutil.cpu_count(),'ram':psutil.virtual_memory().total},'harness_sha256':sha(__file__),'background_build_probes':'document cycle every 15s; image/audio/video upload-download-delete every fourth probe; included in counted operations','latency_method':'closed-loop operation wall time incl SDK/TLS; no retry. p95 >=100 samples, p99 >=1000. Ramp concurrency is not an offered-rate guarantee. Media latency is full logical file operation. Dataset preparation/index build excluded from timed soak.','scope':'engine/Python SDK/API; not eight-hour Workbench UI or multi-host RF3 qualification'})
        provenance=json.loads((self.root/'provenance.json').read_text());provenance['component_ramps_executed_in_this_run']=not self.args.skip_ramps;provenance['qualification_scope']='30-minute short validation; does not satisfy eight-hour gate' if self.args.seconds==1800 else 'duration recorded above';save(self.root/'provenance.json',provenance)
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','3','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-keyout',str(self.root/'key.pem'),'-out',str(self.root/'cert.pem')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True);(self.root/'key.pem').chmod(0o600)
        for path,mime in [(self.fixtures/'test-image.png','image/png'),(self.fixtures/'test-audio.wav','audio/wav'),(self.fixtures/'test-video.mp4','video/mp4')]:
            assert path.is_file() and path.stat().st_size>0,path
            self.media_paths.append((path,mime))
        save(self.root/'fixtures.json',[{'name':p.name,'bytes':p.stat().st_size,'sha256':sha(p),'content_type':m} for p,m in self.media_paths])
        self.set_phase('STARTUP');self.start();self.monitor=threading.Thread(target=self.sample,daemon=True);self.monitor.start()
        self.main.create_database('load')
        self.main.create_collection('vectors',vector_schema={'default':{'dimensions':25,'metric':'cosine','required':True}})
        self.main.create_collection('documents');self.main.create_collection('media')
        self.set_phase('SEED_VECTORS');begin=time.monotonic()
        for offset in range(0,self.args.rows,1000):
            if self.stop.is_set():raise RuntimeError(self.guard)
            docs=[{'id':str(i),'vector':self.base[i].tolist(),'group_id':i%100,'epoch':0} for i in range(offset,min(offset+1000,self.args.rows))]
            response=self.op('vector_batch_insert',lambda:self.main.insert_many('vectors',docs),rows=len(docs));assert response.get('failed',0)==0 and response.get('inserted')==len(docs),response
            if offset%50000==0:self.event(seeded_vectors=offset+len(docs));self.heartbeat()
        self.event(vector_seed_seconds=time.monotonic()-begin)
        self.set_phase('SEED_DOCUMENTS');begin=time.monotonic()
        for offset in range(0,self.args.docs,1000):
            docs=[{'id':'seed-'+str(i),'group_id':i%100,'ordinal':i,'payload':'x'*512} for i in range(offset,min(offset+1000,self.args.docs))]
            response=self.op('document_batch_insert',lambda:self.main.insert_many('documents',docs),rows=len(docs));assert response.get('failed',0)==0 and response.get('inserted')==len(docs),response
        self.event(document_seed_seconds=time.monotonic()-begin)
        self.main.request({'action':'storage_flush'})
        self.set_phase('BUILD_ANN');begin=time.monotonic()
        options={'name':'ann','algorithm':'hnsw','dimensions':25,'metric':'cosine','max_elements':int(self.args.rows*1.25)+1000,'max_scan_rows':self.args.rows+100000,'max_source_bytes':512*1024**2,'max_index_bytes':GIB,'max_build_ms':3600000,'M':16,'ef_construction':200,'ef_search':600,'maintenance':'incremental'}
        self.main.request({'action':'createVectorIndex','collection':'vectors','options':options})
        self.wait_index(3700)
        self.event(index_build_seconds=time.monotonic()-begin)
        self.checkpoint('initial')
    def wait_index(self,timeout=300):
        deadline=time.monotonic()+timeout;last=0;last_probe=0;probe_sequence=0
        while time.monotonic()<deadline:
            if self.stop.is_set():raise RuntimeError(self.guard or 'stopped')
            state=self.main.request({'action':'vectorIndexStatus','collection':'vectors','name':'ann'})['index']
            if state['state']=='READY':return state
            if state['state']=='FAILED':raise RuntimeError('ANN index FAILED '+json.dumps(state))
            if self.phase in ('BUILD_ANN','REBUILD_AFTER_RECOVERY') and time.monotonic()-last_probe>=15:
                # Real durable unrelated traffic while constructing the graph.
                # Every operation is counted; no retries or accepted failures.
                self.document_cycle(self.main,900,probe_sequence)
                if probe_sequence%4==0:self.media_cycle(self.main,900,probe_sequence)
                probe_sequence+=1;last_probe=time.monotonic()
            if time.monotonic()-last>20:self.event(index_state=state);last=time.monotonic()
            time.sleep(1)
        raise TimeoutError('ANN index readiness')
    def ann(self,c,i):
        q=self.queries[i%len(self.queries)]
        response=c.request({'action':'queryVector','collection':'vectors','vector':q.tolist(),'k':10,'metric':'cosine','search_mode':'ann','index_name':'ann','require_index':True,'allow_exact_fallback':False,'ef_search':600})
        assert not response.get('partial'),response
        execution=response['search'];assert execution['executed_mode']=='ann' and not execution.get('fallback'),execution
        docs=response['data'];assert len(docs)==10 and len({d['id'] for d in docs})==10
        v=np.asarray([d['vector'] for d in docs],dtype=np.float64);query=q.astype(np.float64);scores=v@query/(np.linalg.norm(v,axis=1)*np.linalg.norm(query))
        np.testing.assert_allclose([d['score'] for d in docs],scores,rtol=1e-6,atol=1e-6);assert np.all(np.diff(scores)<=1e-6)
        return docs
    def checkpoint(self,label):
        if self.main:self.main.close()
        self.main=self.connect()
        self.set_phase('CHECK_'+label.upper());self.wait_index();recalls=[];tie_recalls=[]
        # Independent float64 exact reference over the current acknowledged, quiescent state.
        values=self.base.astype(np.float64)
        for key,value in self.overrides.items():values[key]=value
        normalized=values/np.linalg.norm(values,axis=1)[:,None]
        if self.deleted:normalized[list(self.deleted)]=0
        samples=20 if self.args.rows<100000 else 100
        for i in range(100,100+samples):
            q=self.queries[i].astype(np.float64);scores=normalized@(q/np.linalg.norm(q))
            if self.deleted:scores[list(self.deleted)]=-np.inf
            truth=np.argpartition(scores,-10)[-10:];cutoff=float(scores[truth].min())
            docs=self.op('ann_recall_query',lambda:self.ann(self.main,i));ids=[int(d['id']) for d in docs]
            recalls.append(len(set(ids)&set(map(int,truth)))/10);tie_recalls.append(sum(scores[x]>=cutoff-1e-6 for x in ids)/10)
            for d in docs:np.testing.assert_array_equal(np.asarray(d['vector'],np.float32),self.overrides.get(int(d['id']),self.base[int(d['id'])]))
        result={'label':label,'queries':samples,'strict_recall_at_10':statistics.mean(recalls),'tie_aware_recall_at_10':statistics.mean(tie_recalls),'ef_search':600,'target':.95,'reference':'independent float64 exhaustive cosine on quiescent committed state','index':self.wait_index()}
        save(self.root/('recall-'+label+'.json'),result);assert result['tie_aware_recall_at_10']>=.95,result
        assert self.main.count('vectors')['count']==self.args.rows-len(self.deleted)
        assert self.main.count('documents')['count']==self.args.docs+len(self.doc_expected)
        for key,expected in list(self.doc_expected.items()):assert self.main.find_one('documents',{'id':key})['sequence']==expected
        for key,value in list(self.overrides.items()):
            if key not in self.deleted:np.testing.assert_array_equal(np.asarray(self.main.find_one('vectors',{'id':str(key)})['vector'],np.float32),value)
        for key in self.deleted:assert self.main.find_one('vectors',{'id':str(key)}) is None
        for key,fixture in list(self.media_expected.items()):
            out=self.downloads/('check-'+key);self.main.media.download_file(key,out,collection='media');assert sha(out)==sha(fixture);out.unlink()
        for key in self.media_deleted[-100:]:
            try:self.main.media.get_manifest(media_id=key)
            except Exception as e:assert getattr(e,'code',None)=='media_not_found',(type(e).__name__,str(e))
            else:raise AssertionError('Deleted media remains visible')
        checked_seed=0
        if label=='after-restart':
            seen=set()
            for offset in range(0,self.args.docs+len(self.doc_expected),1000):
                page=self.main.find('documents',{},limit=1000,offset=offset)
                # find marks an explicitly limited page partial=result_limit. Prove
                # full pagination by expected page lengths, unique IDs and total.
                assert not page.get('partial') or page.get('partial_reason')=='result_limit',{'reason':page.get('partial_reason'),'offset':offset}
                assert len(page['data'])==min(1000,self.args.docs+len(self.doc_expected)-offset),{'offset':offset,'rows':len(page['data'])}
                for d in page['data']:
                    assert d['id'] not in seen,d['id'];seen.add(d['id'])
                    if d['id'].startswith('seed-'):
                        i=int(d['id'][5:]);assert 0<=i<self.args.docs and d['ordinal']==i and d['group_id']==i%100 and d['payload']=='x'*512
                        checked_seed+=1
            assert checked_seed==self.args.docs and len(seen)==self.args.docs+len(self.doc_expected),(checked_seed,len(seen))
        save(self.root/('integrity-'+label+'.json'),{'status':'PASS','acknowledged_mutated_documents_verified':len(self.doc_expected),'acknowledged_mutated_vectors_verified':len(self.overrides),'deleted_vectors_verified':len(self.deleted),'retained_media_verified':len(self.media_expected),'recent_deleted_media_verified':min(100,len(self.media_deleted)),'seed_document_count_verified':self.args.docs,'untouched_seed_document_payloads_verified':checked_seed,'all_untouched_seed_vector_payloads_read':False})
    def media_cycle(self,c,w,seq,retain=False):
        fixture,mime=self.media_paths[seq%len(self.media_paths)]
        m=self.op('media_upload',lambda:c.media.upload_file('media',fixture,content_type=mime,chunk_bytes=256*1024),bytes=fixture.stat().st_size,fixture=fixture.name)
        key=m['id'];self.media_expected[key]=fixture
        out=self.downloads/(str(w)+'-'+str(seq));self.op('media_download',lambda:c.media.download_file(key,out,collection='media'),bytes=fixture.stat().st_size,fixture=fixture.name);assert sha(out)==sha(fixture);out.unlink()
        if not retain:
            response=self.op('media_delete',lambda:c.media.delete(media_id=key));assert response['status']=='ok',response
            del self.media_expected[key];self.media_deleted.append(key)
    def document_cycle(self,c,w,seq):
        key='load-'+str(w)+'-'+str(seq%32)
        if key not in self.doc_expected:
            r=self.op('document_insert',lambda:c.insert('documents',{'id':key,'owner':w,'sequence':seq,'payload':'y'*512}));assert r.get('status') in ('ok','inserted'),r
        else:
            r=self.op('document_update',lambda:c.update_one('documents',{'id':key},{'$set':{'sequence':seq}}));assert r.get('status')=='updated',r
        self.doc_expected[key]=seq
        row=self.op('document_read',lambda:c.find_one('documents',{'id':key}));assert row is not None and row['sequence']==seq
        if seq%17==0:
            self.op('document_delete',lambda:c.delete_one('documents',{'id':key}));del self.doc_expected[key]
            assert self.op('document_read_deleted',lambda:c.find_one('documents',{'id':key})) is None
        if seq%25==0:
            # Indexed primary-key seed fetch exercises cold document storage without a full scan.
            row=self.op('document_seed_read',lambda:c.find_one('documents',{'id':'seed-'+str((w*997+seq)%self.args.docs)}));assert row['payload']=='x'*512
    def vector_cycle(self,c,w,seq,mutate=False):
        self.op('ann_query',lambda:self.ann(c,100+(w*97+seq)%1000))
        if mutate:
            key=(w*37+seq)%min(self.args.rows,10000);value=self.base[(key+seq+1)%self.args.rows].copy()
            r=self.op('vector_update',lambda:c.update_one('vectors',{'id':str(key)},{'$set':{'vector':value.tolist(),'epoch':seq}}));assert r.get('status')=='updated',r
            self.overrides[key]=value
            row=self.op('vector_read',lambda:c.find_one('vectors',{'id':str(key)}));np.testing.assert_array_equal(np.asarray(row['vector'],np.float32),value)
            if seq%(4 if self.args.rows<100000 else 120)==0:
                reserved=self.args.rows-1
                if reserved in self.deleted:
                    r=self.op('vector_reinsert',lambda:c.insert('vectors',{'id':str(reserved),'vector':self.base[reserved].tolist(),'group_id':reserved%100,'epoch':seq}));assert r.get('status') in ('ok','inserted'),r
                    self.deleted.remove(reserved);self.overrides.pop(reserved,None)
                else:
                    self.op('vector_delete',lambda:c.delete_one('vectors',{'id':str(reserved)}));self.deleted.add(reserved)
                    assert self.op('vector_read_deleted',lambda:c.find_one('vectors',{'id':str(reserved)})) is None
    def run_phase(self,name,workers,seconds,mixed=False,period=0):
        self.set_phase(name);start=time.monotonic();end=start+seconds;abort=threading.Event()
        def worker(w):
            c=None;opened=0;seq=0
            try:
                while time.monotonic()<end and not abort.is_set() and not self.stop.is_set():
                    t=time.monotonic()
                    if c is None or t-opened>1800:
                        if c:c.close()
                        c=self.op('session_connect',self.connect);opened=time.monotonic()
                    kind=('media' if w==workers-1 else 'vector' if w%3==0 else 'document') if mixed else name.split('_')[0].lower()
                    if kind=='document':self.document_cycle(c,w,seq)
                    elif kind=='vector':self.vector_cycle(c,w,seq,mutate=mixed and w==0 and seq%(2 if self.args.rows<100000 else 60)==0)
                    elif kind=='media':self.media_cycle(c,w,seq)
                    else:raise AssertionError(kind)
                    seq+=1
                    pace=15 if mixed and kind=='media' else period
                    if pace:abort.wait(min(max(0,pace-(time.monotonic()-t)),max(0,end-time.monotonic())))
            except Exception as e:
                self.event(worker_failure={'worker':w,'sequence':seq,'type':type(e).__name__,'code':getattr(e,'code',None),'message':str(e)[:1500]});abort.set();self.stop.set();raise
            finally:
                if c:c.close()
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures=[pool.submit(worker,w) for w in range(workers)]
            for f in futures:f.result()
        actual=time.monotonic()-start
        if self.stop.is_set():raise RuntimeError(self.guard or 'Workload failure/cancelled')
        selected={k.split('/',1)[1]:{'success':v['ok'],'failed':v['failed'],'latency':pct(v['latencies'])} for k,v in self.records.items() if k.startswith(name+'/')}
        result={'name':name,'workers':workers,'requested_seconds':seconds,'actual_seconds':actual,'operations':selected,'successful_logical_operations_per_second_excluding_session_setup':sum(x['success'] for kind,x in selected.items() if kind!='session_connect')/actual,'period_seconds_per_worker':period,'media_period_seconds':15 if mixed else 0,'status':'PASS'}
        assert actual>=seconds-.1,result
        self.summaries.append(result);save(self.root/'phase-results.json',self.summaries)
    def execute(self):
        result={'status':'FAIL','candidate':EXPECTED,'started_utc':utc()}
        try:
            self.prepare()
            if self.args.skip_ramps:
                self.event(ramps_skipped=True,reason='Existing same-candidate 60-second component ramp evidence retained by short-run controller; this run is 30-minute mixed qualification only.')
                self.checkpoint('before-soak')
            else:
                for kind in ['DOCUMENT','VECTOR','MEDIA']:
                    for clients in [1,8,32]:self.run_phase(kind+'_RAMP_'+str(clients),clients,self.args.ramp_seconds)
                self.checkpoint('after-ramps')
            self.run_phase('MIXED_SOAK',12,self.args.seconds,mixed=True,period=.5)
            self.checkpoint('after-soak')
            # Keep a fixture of each actual media type for the abrupt-restart check.
            for i in range(3):self.media_cycle(self.main,100,i,retain=True)
            self.set_phase('CRASH_AND_RECOVER');self.stop_engine(crash=True);begin=time.monotonic();self.start();self.event(restart_seconds=time.monotonic()-begin)
            # Derived graph bytes are deliberately not authoritative/persisted. Recovery
            # restores the durable spec as ABSENT; explicitly rebuild before strict ANN.
            self.event(recovered_index=self.main.request({'action':'vectorIndexStatus','collection':'vectors','name':'ann'})['index'])
            self.set_phase('REBUILD_AFTER_RECOVERY');begin=time.monotonic()
            self.main.request({'action':'rebuildVectorIndex','collection':'vectors','name':'ann'})
            self.wait_index(3700);self.checkpoint('after-restart')
            self.event(recovery_index_rebuild_and_integrity_seconds=time.monotonic()-begin)
            self.set_phase('GRACEFUL_SHUTDOWN');self.stop_engine()
            result.update(status='PASS',phases=self.summaries,scope='single-node engine/API authenticated TLS load; no production-readiness certification')
        except BaseException as e:
            result.update(error={'type':type(e).__name__,'message':str(e)[:2500]},traceback=traceback.format_exc());self.event(terminal_error=result['error'])
            # Evidence only, never retry a failed query or change FAIL to PASS.
            # The authenticated administrative session can still inspect a failed
            # run before teardown while its generated credentials remain in RAM.
            if self.proc and self.proc.poll() is None:
                diagnostic={'status':'INCOMPLETE','utc':utc()}
                try:
                    c=self.connect()
                    try:diagnostic['index']=c.request({'action':'vectorIndexStatus','collection':'vectors','name':'ann'})
                    except Exception as error:diagnostic['index_error']=str(error)[:1500]
                    c.close()
                except Exception as error:diagnostic['connection_error']=str(error)[:1500]
                save(self.root/'failure-diagnostic.json',diagnostic)
        finally:
            self.monitor_stop.set()
            if self.monitor:self.monitor.join(timeout=10)
            try:self.stop_engine()
            except Exception as e:result['shutdown_error']=str(e);result['status']='FAIL'
            result.update(finished_utc=utc(),elapsed_seconds=time.monotonic()-self.started,resource_guard=self.guard,request_failures=len(self.errors))
            save(self.root/'exit.json',result);save(self.root/'heartbeat.json',{'status':result['status'],'utc':utc(),'phase':'TERMINAL','result':'exit.json'})
            self.events.close();self.resources.close();self.state.close();self.host_resources.close()
        return 0 if result['status']=='PASS' else 1
def fixtures(folder):
    folder.mkdir(exist_ok=True)
    rng=np.random.default_rng(20261105);width=height=640
    raw=b''.join(b'\0'+rng.integers(0,256,(width,3),dtype=np.uint8).tobytes() for _ in range(height))
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    (folder/'test-image.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
    x=np.arange(44100*6);sound=(np.sin(x*2*np.pi*440/44100)*10000).astype('<i2');stereo=np.column_stack([sound,sound])
    with wave.open(str(folder/'test-audio.wav'),'wb') as f:f.setnchannels(2);f.setsampwidth(2);f.setframerate(44100);f.writeframes(stereo.tobytes())
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--engine',type=Path,required=True);p.add_argument('--dataset',type=Path,required=True);p.add_argument('--expected-commit',required=True);p.add_argument('--root',type=Path,required=True);p.add_argument('--fixtures',type=Path,required=True);p.add_argument('--rows',type=int,default=1000000);p.add_argument('--docs',type=int,default=100000);p.add_argument('--seconds',type=float,default=1800);p.add_argument('--ramp-seconds',type=float,default=60);p.add_argument('--make-fixtures',action='store_true');p.add_argument('--skip-ramps',action='store_true');a=p.parse_args()
    BINARY=a.engine.resolve(strict=True);BASE=a.dataset.resolve(strict=True);EXPECTED=a.expected_commit
    if len(EXPECTED)!=40 or any(c not in '0123456789abcdef' for c in EXPECTED):p.error('--expected-commit must be a full lowercase Git SHA')
    if a.skip_ramps:p.error('this reproducible profile requires fresh component ramps; --skip-ramps is unsupported')
    if a.make_fixtures:fixtures(a.fixtures);sys.exit(0)
    assert a.root and 100<=a.rows<=1000000 and a.docs>0 and a.seconds>0
    r=Run(a)
    def cancelled(*_):r.guard='received cancellation signal';r.stop.set()
    signal.signal(signal.SIGTERM,cancelled);signal.signal(signal.SIGINT,cancelled)
    sys.exit(r.execute())
