import argparse,pathlib,json,importlib.util,sys,os,time,shutil,traceback,subprocess,statistics
from types import SimpleNamespace
root=pathlib.Path(__file__).resolve().parents[2];sys.path.insert(0,str(root/'sdk/python'));spec=importlib.util.spec_from_file_location('three',root/'scripts/benchmark-three-vector-databases.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);b=m.b
p=argparse.ArgumentParser();p.add_argument('--variant',required=True);p.add_argument('--build',type=pathlib.Path,required=True);a=p.parse_args();home=pathlib.Path(__file__).resolve().parent;out=home/('index-'+a.variant);out.mkdir(exist_ok=False)
prior=json.loads((root/'build-v1.2-release/three-db-optimized-1m-performance/summary.json').read_text());fixture=pathlib.Path(prior['systems']['pacific']['private_scratch_root']).resolve();allowed=(root/'build-v1.2-release/three-db-optimized-1m-performance-scratch').resolve();assert prior['status']=='PASS' and fixture.is_relative_to(allowed)
publication=pathlib.Path('<public-inputs>');manifest=json.loads((publication/'manifest.json').read_text());assert b.digest(publication/'queries.npy')==manifest['hashes']['queries.npy'];queries=b.np.load(publication/'queries.npy');overrides={i:queries[100+i] for i in range(10)}
for w in range(4):
 for op in range(10):overrides[100+w*10+op]=queries[200+w*10+op]
deleted=set(range(5))
scratch=home/'benchmark-scratch';scratch.mkdir(exist_ok=True);args=SimpleNamespace(build=a.build.resolve(),scratch=scratch,server_cpus=[0,1,2,3],startup_timeout=240);server=b.Server('pacific',args,1000000,25,out)
for file in fixture.rglob('*'):assert not file.is_symlink(),file
os.sched_setaffinity(0,{4,5});r={'status':'RUNNING','variant':a.variant,'engine_sha256':b.digest(args.build/'db_engine'),'scope':'1M original/999995 live vectors; synchronous creation of metadata index on kind; no increased request timeout, unchanged durable configuration; disposable clone of the recovered fixture','server_cpus':[0,1,2,3],'client_cpus':[4,5],'api_ack_pass':False}
try:
 # Initialize a new destination through the engine; retain its root-local
 # ownership identity unchanged while restoring offline authoritative data.
 server.start(new=True);server.stop()
 destination=server.root/'data';identity=destination/'.pacificdb-root-identity.json';identity_bytes=identity.read_bytes()
 marker=json.loads(identity_bytes);assert marker['canonicalRoot']==str(destination.resolve()) and marker['nodeId']=='bench-node'
 source_data=fixture/'data';source_marker=json.loads((source_data/identity.name).read_text())
 assert marker['clusterId']==source_marker['clusterId'] and marker['nodeId']==source_marker['nodeId'] and marker['storageFormatVersion']==source_marker['storageFormatVersion']
 for child in destination.iterdir():
  if child.name in {identity.name,'.pacificdb-root.lock'}:continue
  if child.is_dir():shutil.rmtree(child)
  else:child.unlink()
 for child in source_data.iterdir():
  if child.name in {identity.name,'.pacificdb-root.lock'}:continue
  if child.is_dir():shutil.copytree(child,destination/child.name)
  else:shutil.copy2(child,destination/child.name)
 assert identity.read_bytes()==identity_bytes
 r['fixture_provisioning']='offline data restored into engine-initialized owned disposable destination; original source and destination ownership identity unchanged'
 server.memory.clear()
 server.start(new=False);assert server.count()==999995
 r['before_mutation_verification']=m.verify_records(server,overrides,deleted)
 assert not any(x.get('field')=='kind' for x in server.connection.request({'action':'listIndexes','collection':'vectors'})['indexes'])
 begin=time.perf_counter()
 try:
  r['creation_response']=server.connection.request({'action':'createIndex','collection':'vectors','name':'production_kind_1','fields':{'kind':1}});r['api_ack_pass']=True
 except Exception:r['creation_error']=traceback.format_exc()
 r['create_request_seconds']=time.perf_counter()-begin
 deadline=time.monotonic()+180;server.refresh()
 while True:
  listing=server.connection.request({'action':'listIndexes','collection':'vectors'});definition=next((x for x in listing['indexes'] if x.get('name')=='production_kind_1'),{})
  if definition.get('status')=='ready' and definition.get('complete'):break
  if time.monotonic()>deadline:raise TimeoutError(str(listing))
  time.sleep(.25)
 r['observed_ready_seconds']=time.perf_counter()-begin;r['definition']=definition;assert definition['documents']==999995
 if a.variant=='candidate':assert r['api_ack_pass'] and definition.get('build_strategy')=='streaming',r
 else:assert not r['api_ack_pass'] and any(x in r['creation_error'] for x in ['write_outcome_unknown','write_not_committed']),r
 server.stop(crash=True);begin=time.perf_counter();server.start(new=False);r['post_ack_or_ambiguous_kill_restart_seconds']=time.perf_counter()-begin
 listing=server.connection.request({'action':'listIndexes','collection':'vectors'});ready=next(x for x in listing['indexes'] if x.get('name')=='production_kind_1');assert ready['status']=='ready' and ready['complete'] and server.count()==999995
 r['post_crash_definition']=ready
 r['after_crash_mutation_verification']=m.verify_records(server,overrides,deleted)
 for id in [5,101,138,999999]:assert server.connection.find_one('vectors',{'id':str(id)})['kind']=='vector'
 for id in range(5):assert server.connection.find_one('vectors',{'id':str(id)}) is None
 # Matched small-sample check of the changed shared SST scan, after restart.
 # No tail latency inference: two warmups, five measured independent queries.
 assert b.digest(publication/'base.npy')==manifest['hashes']['base.npy']
 base=b.np.load(publication/'base.npy',mmap_mode='r');ids=b.np.array([i for i in range(1,len(base),100) if i not in deleted]);filtered=b.np.array(base[ids]);positions={int(id):j for j,id in enumerate(ids)}
 for id,value in overrides.items():
  if id in positions:filtered[positions[id]]=value
 truth=b.reference(filtered,queries[100:105]);truth=[{'ids':[int(ids[i]) for i in x['ids']],'cutoff':x['cutoff']} for x in truth]
 scans=[]
 for phase,qi in [('warmup',100),('warmup',101),*[('measured',i) for i in range(100,105)]]:
  begin=time.perf_counter();response=server.connection.request({'action':'queryVector','collection':'vectors','vector':queries[qi].tolist(),'k':10,'metric':'cosine','search_mode':'exact','index_name':'flat','require_index':False,'filter':{'$or':[{'group_id':1},{'group_id':101}]}});ms=(time.perf_counter()-begin)*1000
  assert not response.get('partial') and response['search']['executed_mode']=='exact' and response['search']['candidate_source']=='merged_scan',response
  checked=b.validate(response['data'],queries[qi],base,truth[qi-100],group=1,overrides=overrides,deleted=deleted);assert checked['strict_recall']==1 and checked['tie_aware_recall']==1,checked
  scans.append({'phase':phase,'query':qi,'ms':ms,'execution':response['search'],**checked})
 r['shared_scan_diagnostic']={'records':scans,'measured_median_ms':statistics.median(x['ms'] for x in scans if x['phase']=='measured'),'scope':'5 measured queries and 2 warmups, same post-restart 999995-row fixture; OR forces full scan; independent filtered exact ground truth; no tail claims'}
 server.stop()
 audit=subprocess.run([str(home/'audit-index'),str(server.root/'data/system/bench/vectors.idx/kind.cidx')],text=True,capture_output=True);r['physical_audit']={'exit_code':audit.returncode,'stdout':audit.stdout,'stderr':audit.stderr};assert audit.returncode==0,r['physical_audit']
 r['status']='PASS' if a.variant=='candidate' else 'BASELINE_ACK_FAIL_REPRODUCED'
except BaseException:r['status']='FAIL';r['error']=traceback.format_exc();raise
finally:
 try:server.stop()
 except BaseException:r['status']='FAIL';r['shutdown_error']=traceback.format_exc();raise
 finally:r['resource_samples']=server.memory;r['peak_sampled_pss_bytes']=max((x['pss'] for x in server.memory),default=0);r['peak_sampled_rss_bytes']=max((x['rss'] for x in server.memory),default=0);b.save(out/'result.json',r)
print(json.dumps({k:v for k,v in r.items() if k not in ['resource_samples','creation_response','creation_error']}),flush=True)
