#!/usr/bin/env python3
"""Separate 1M recovery/mutation continuation; never erase the failed 120s trial."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
os.environ['OMP_NUM_THREADS']='1'
import argparse,concurrent.futures,importlib.util,json,threading,time,traceback,statistics
from pathlib import Path
import numpy as np
spec=importlib.util.spec_from_file_location('community_bench',Path(__file__).with_name('benchmark-community-v12.py'))
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','pg','scratch','output','primary','publication'):p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args()
    for name in ('build','pg','scratch','primary','publication'):setattr(args,name,getattr(args,name).resolve(strict=True))
    args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True);assert not any(args.output.iterdir())
    primary=json.loads((args.primary/'summary.json').read_text());row=next(d for d in primary['datasets'] if d['rows']==1000000)
    assert primary['status']=='FAIL' and row['status']=='FAIL' and 'connection failed' in row['error']
    assert b.digest(args.build/'db_engine')==primary['candidate_sha256']
    cpus=sorted(os.sched_getaffinity(0));args.server_cpus=cpus[:4];args.startup_timeout=900;os.sched_setaffinity(0,cpus[4:6])
    manifest=json.loads((args.publication/'data/manifest.json').read_text())
    for name in ('base.npy','queries.npy'):assert b.digest(args.publication/'data'/name)==manifest['hashes'][name]
    base=np.load(args.publication/'data/base.npy',mmap_mode='r');qs=np.load(args.publication/'data/queries.npy');assert len(base)==1000000
    result={'status':'RUNNING','candidate_sha256':primary['candidate_sha256'],'script_sha256':b.digest(__file__),
      'startup_budget_seconds':900,'original_primary_status':'FAIL','systems':{},
      'protocol':'Separate continuation on retained owned roots. Original Pacific SIGKILL and failed 120s startup retained. PostgreSQL seeded with the identical 50 updates/5 deletes before SIGKILL. Recovery precedes new controlled updates/deletes/mixed traffic; no second crash is implied.'}
    save=lambda:b.save(args.output/'summary.json',result)
    save()
    try:
      for kind in ('pacific','postgres'):
        path=args.output/kind;path.mkdir();server=b.Server(kind,args,len(base),25,path)
        retained=Path(row['systems'][kind]['retained_root']).resolve(strict=True)
        assert retained.parent==args.scratch and retained.name.startswith(kind+'-1000000-')
        server.root=retained;value={'status':'RUNNING','retained_root':str(retained)};result['systems'][kind]=value;save()
        overrides={i:qs[100+i] for i in range(10)};overrides.update({100+i:qs[200+i] for i in range(40)});deleted=set(range(5))
        ef=row['systems'][kind]['ann_tuning'][-1]['ef_search']
        try:
          print('RECOVERY_START',kind,flush=True);server.start(new=False)
          if kind=='postgres':
            assert server.count()==len(base)
            for i,v in overrides.items():b.update(server,i,v)
            for i in deleted:assert server.connection.execute('DELETE FROM vectors WHERE id=%s RETURNING id',(str(i),)).fetchone()[0]==str(i)
            server.stop(crash=True);server.start(new=False)
          recovery={'restart_to_connection_seconds':server.start_seconds,'startup_budget_seconds':900,'count':server.count()}
          assert recovery['count']==len(base)-5
          value['crash_recovery']=recovery;save();print('RECOVERY_CONNECTED',kind,server.start_seconds,flush=True)
          # Validate every acknowledged changed record, not just the total count.
          for i,v in overrides.items():
            if i in deleted:continue
            if kind=='pacific':doc=server.connection.find_one('vectors',{'id':str(i)})
            else:doc={'vector':json.loads(server.connection.execute('SELECT embedding::text FROM vectors WHERE id=%s',(str(i),)).fetchone()[0])}
            assert doc is not None,(kind,i);np.testing.assert_array_equal(np.asarray(doc['vector'],np.float32),v)
          recovery['acknowledged_changed_records_verified']=45;save()
          if kind=='pacific':
            state=server.connection.request({'action':'vectorIndexStatus','collection':'vectors','name':'ann'})['index'];assert state['state']=='ABSENT',state
            recovery['initial_ann_state']='ABSENT'
            try:server.query(qs[100],'ann',ef);raise AssertionError('missing graph silently succeeded')
            except AssertionError:raise
            except Exception as error:assert 'unavailable' in str(error)
            begin=time.perf_counter();server.connection.request({'action':'rebuildVectorIndex','collection':'vectors','name':'ann'});server.wait('ann')
            recovery['ann_reconstruction_seconds']=time.perf_counter()-begin
          else:recovery.update(initial_ann_state='persistent index available',ann_reconstruction_seconds=0)
          server.plan(server.connection,'ann',ef);checks=[]
          for qi in range(100,110):
            truth=b.mutable_reference(base,qs[qi],overrides,deleted);docs,ms,execution=server.query(qs[qi],'ann',ef)
            checks.append({'query':qi,'ms':ms,**b.validate(docs,qs[qi],base,truth,overrides=overrides,deleted=deleted)})
          recovery['ann_recall_at_10']=statistics.mean(r['tie_aware_recall'] for r in checks);recovery['records']=checks
          assert recovery['ann_recall_at_10']>=.95;save();print('RECOVERY_VALIDATED',kind,recovery['ann_reconstruction_seconds'],flush=True)
          commits=[];visibility=[];checks=[]
          for i in range(10):
            id=10+i;v=qs[300+i];commits.append(b.update(server,id,v));overrides[id]=v;begin=time.perf_counter()
            if kind=='pacific':server.wait('ann',timeout=60)
            visibility.append((time.perf_counter()-begin)*1000)
            truth=b.mutable_reference(base,v,overrides,deleted);docs,ms,execution=server.query(v,'ann',ef);assert str(id) in [d['id'] for d in docs]
            checks.append({'ms':ms,**b.validate(docs,v,base,truth,overrides=overrides,deleted=deleted)})
          value['updates']={'count':10,'ids':list(range(10,20)),'commit_latency':b.stats(commits),'post_ack_ann_ready_wait':b.stats(visibility),'records':checks};save()
          deletion=[]
          for i in range(5,10):
            begin=time.perf_counter()
            if kind=='pacific':server.connection.delete_one('vectors',{'id':str(i)})
            else:assert server.connection.execute('DELETE FROM vectors WHERE id=%s RETURNING id',(str(i),)).fetchone()[0]==str(i)
            deletion.append((time.perf_counter()-begin)*1000);deleted.add(i)
          if kind=='pacific':server.wait('ann',timeout=60)
          assert server.count()==len(base)-10
          value['deletes']={'commit_latency':b.stats(deletion),'verified_count':server.count(),'ids':list(range(5,10))};save()
          errors=[];reads=[];writes=[];lock=threading.Lock()
          def mixed(worker):
            c=server.connect();server.plan(c,'ann',ef)
            try:
              for operation in range(100):
                if operation%10==0:
                  id=200+worker*10+operation//10;v=qs[400+worker*10+operation//10];ms=b.update(server,id,v,c)
                  with lock:overrides[id]=v;writes.append(ms)
                else:
                  qi=100+worker*100+operation;begin=time.perf_counter()
                  try:
                    docs,ms,execution=server.query(qs[qi],'ann',ef,connection=c);ids=[int(d['id']) for d in docs]
                    assert len(set(ids))==len(ids)==10 and not(set(ids)&deleted)
                    vectors=np.asarray([d['vector'] for d in docs],np.float64);q=qs[qi].astype(np.float64);scores=(vectors@q)/(np.linalg.norm(vectors,axis=1)*np.linalg.norm(q))
                    np.testing.assert_allclose([d['score'] for d in docs],scores,atol=1e-6,rtol=1e-6)
                    with lock:reads.append(ms)
                  except Exception as error:
                    message=str(error)
                    if kind!='pacific' or not any(x in message for x in ('unavailable','freshness_changed','source_changing','visibility_changed')):raise
                    with lock:errors.append({'error':message[:500],'ms':(time.perf_counter()-begin)*1000})
            finally:c.close()
          begin=time.perf_counter()
          with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(mixed,range(4)))
          value['mixed_90_read_10_write']={'reads_attempted':360,'read_successes':len(reads),'read_errors':errors,'successful_read_latency':b.stats(reads),'write_latency':b.stats(writes),'wall_seconds':time.perf_counter()-begin,'successful_read_fraction':len(reads)/360}
          server.refresh()
          if kind=='pacific':server.wait('ann',timeout=60)
          value['status']='PASS';save()
        except Exception as error:value['status']='FAIL';value['error']=str(error);(path/'failure.txt').write_text(traceback.format_exc());raise
        finally:
          if server.proc and server.proc.poll() is None:server.stop()
          value['resource_samples']=server.memory;value['peak_sampled_pss_bytes']=max((x['pss'] for x in server.memory),default=0);save()
      result['status']='PASS'
    except Exception:result['status']='FAIL'
    finally:save();os.sched_setaffinity(0,cpus)
    raise SystemExit(result['status']!='PASS')
if __name__=='__main__':main()
