#!/usr/bin/env python3
"""Exercise committed vector point visibility during writes, flush and dirty restart.

Uses the benchmark's owned standalone server, with no ANN build or performance claim.
The source dataset is read-only; all mutations use newly allocated scratch roots.
"""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
os.environ['OMP_NUM_THREADS']='1'
import argparse,concurrent.futures,importlib.util,json,shutil,threading,time,traceback
from pathlib import Path
import numpy as np
spec=importlib.util.spec_from_file_location('community_bench',Path(__file__).with_name('benchmark-community-v12.py'))
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','publication','output','scratch'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--rows',type=int,choices=(10000,100000,1000000),default=10000)
    args=p.parse_args();args.build=args.build.resolve(strict=True);args.publication=args.publication.resolve(strict=True)
    args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True);assert not any(args.output.iterdir())
    args.scratch=args.scratch.resolve();args.scratch.mkdir(parents=True,exist_ok=True)
    cpus=sorted(os.sched_getaffinity(0));assert len(cpus)>=6;args.server_cpus=cpus[:4];args.startup_timeout=900
    os.sched_setaffinity(0,cpus[4:6])
    manifest=json.loads((args.publication/'data/manifest.json').read_text())
    for name in ('base.npy','queries.npy'):assert b.digest(args.publication/'data'/name)==manifest['hashes'][name]
    base=np.load(args.publication/'data/base.npy',mmap_mode='r')[:args.rows];qs=np.load(args.publication/'data/queries.npy')
    result={'status':'RUNNING','rows':args.rows,'candidate_sha256':b.digest(args.build/'db_engine'),
      'script_sha256':b.digest(__file__),'scope':'correctness/recovery only; not a timing comparison','startup_budget_seconds':900}
    save=lambda phase:b.save(args.output/'summary.json',{'phase':phase,**result})
    server=b.Server('pacific',args,args.rows,25,args.output);save('created');overrides={};deleted=set()
    try:
        server.start();server.setup();result['load']=server.load(base,1000);assert server.count()==args.rows
        save('loaded');print('LOADED',args.rows,flush=True)
        for i in range(10):b.update(server,i,qs[100+i]);overrides[i]=qs[100+i]
        for i in range(5):
            response=server.connection.delete_one('vectors',{'id':str(i)});assert response.get('status')=='deleted',response;deleted.add(i)
        lock=threading.Lock()
        def worker(number):
            c=server.connect();records=[]
            try:
                for i in range(10):
                    id=100+number*10+i;vector=qs[200+number*10+i];ms=b.update(server,id,vector,c)
                    with lock:overrides[id]=vector
                    for _ in range(9):
                        doc=c.find_one('vectors',{'id':str(id)});assert doc is not None and doc['id']==str(id)
                        np.testing.assert_array_equal(np.asarray(doc['vector'],np.float32),vector,
                            err_msg=f'point read after acknowledged own write id={id}')
                    records.append({'id':id,'status':'updated','verified_following_reads':9,'commit_ms':ms})
            finally:c.close()
            return records
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:result['concurrent_writes']=sum(list(pool.map(worker,range(4))),[])
        server.refresh();result['before_crash']=b.verify_mutations(server,overrides,deleted);save('before_crash_verified')
        server.stop(crash=True)
        # Preserve the exact dirty fixture before any recovery changes it.
        preserved=server.root.with_name(server.root.name+'-before-restart')
        shutil.copytree(server.root,preserved);result['preserved_dirty_root']=str(preserved)
        raft=preserved/'data/raft/log.bin';result['preserved_raft_sha256']=b.digest(raft)
        result['preserved_raft_bytes']=raft.stat().st_size;save('crashed_fixture_preserved')
        server.start(new=False);result['restart_to_connection_seconds']=server.start_seconds;save('restarted')
        assert server.count()==args.rows-5
        result['after_crash']=b.verify_mutations(server,overrides,deleted)
        result['count_after_crash']=server.count();save('after_crash_verified')
        result['exact_filtered_checks']=[]
        for id in (5,100,105,110,139):
            q=overrides[id];response=server.connection.request({'action':'queryVector','collection':'vectors',
              'vector':q.tolist(),'metric':'cosine','search_mode':'exact','k':1,'filter':{'id':str(id)}})
            assert len(response['data'])==1 and response['data'][0]['id']==str(id)
            np.testing.assert_array_equal(np.asarray(response['data'][0]['vector'],np.float32),q)
            np.testing.assert_allclose(response['data'][0]['score'],1.,rtol=1e-12,atol=1e-12)
            result['exact_filtered_checks'].append({'id':id,'score':response['data'][0]['score']});save('exact_checks')
        result['status']='PASS';print('RECOVERY_PASS',args.rows,server.start_seconds,flush=True)
    except Exception as error:
        result['status']='FAIL';result['error']=str(error);(args.output/'failure.txt').write_text(traceback.format_exc())
        print('RECOVERY_FAIL',str(error),flush=True)
    finally:
        try:
            if server.proc and server.proc.poll() is None:server.stop()
        except Exception as error:result['status']='FAIL';result['shutdown_error']=str(error)
        result['retained_root']=str(server.root);result['resource_samples']=server.memory
        result['peak_sampled_pss_bytes']=max((x['pss'] for x in server.memory),default=0)
        save('finished');os.sched_setaffinity(0,cpus)
    raise SystemExit(result['status']!='PASS')
if __name__=='__main__':main()
