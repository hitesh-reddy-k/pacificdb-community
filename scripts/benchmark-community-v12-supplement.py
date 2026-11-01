#!/usr/bin/env python3
"""Supplemental public metric/dimension and sparse/named/hybrid comparisons."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
os.environ['OMP_NUM_THREADS']='1'
import argparse,importlib.util,json,time,traceback,statistics
from pathlib import Path
import numpy as np
spec=importlib.util.spec_from_file_location('community_bench',Path(__file__).with_name('benchmark-community-v12.py'))
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)

def capabilities(server):
    times=[];sparse={'format':'sparse-v1','precision':'float64','dimensions':1000,'indices':[3,999],'values':[1.,2.]}
    if server.kind=='pacific':
        c=server.connection;c.request({'action':'createCollection','collection':'named','vector_schema':{'named':{
          'dense':{'dimensions':2,'metric':'dot','required':True},'terms':{'kind':'sparse','dimensions':1000,'metric':'dot','required':True}}}})
        docs=[]
        for i in range(1000):docs.append({'id':str(i),'vectors':{'dense':[float(i),1.],
          'terms':{**sparse,'values':[float(i),2.]}},'title':'rare ocean' if i==999 else 'ocean shared','group':i%2})
        begin=time.perf_counter();assert c.insert_many('named',docs)['inserted']==1000;load=time.perf_counter()-begin
        sparse_results=[]
        for i in range(100):
            start=time.perf_counter();r=c.request({'action':'queryVector','collection':'named','vector':sparse,'vector_name':'terms','metric':'dot','k':10,'search_mode':'exact'});times.append((time.perf_counter()-start)*1000)
            assert r['data'][0]['id']=='999';assert r['data'][0]['score']==1003.;sparse_results=[x['id'] for x in r['data']]
        begin=time.perf_counter();c.request({'action':'createVectorIndex','collection':'named','options':{
          'name':'hybrid','algorithm':'flat','dimensions':2,'metric':'dot','vector_name':'dense','text_field':'title',
          'max_elements':2000,'max_source_bytes':4*1024**2,'max_index_bytes':8*1024**2,'max_lexical_bytes':8*1024**2}})
        deadline=time.monotonic()+60
        while True:
            status=c.request({'action':'vectorIndexStatus','collection':'named','name':'hybrid'})['index']
            if status['state']=='READY':break
            assert status['state']!='FAILED' and time.monotonic()<deadline,status;time.sleep(.02)
        build=time.perf_counter()-begin;hybrid=[]
        for i in range(100):
            start=time.perf_counter();r=c.request({'action':'queryHybrid','collection':'named','index_name':'hybrid','vector':[1.,0.],
              'vector_name':'dense','metric':'dot','text':'rare','k':10,'candidates':100});hybrid.append((time.perf_counter()-start)*1000)
            assert r['data'][0]['id']=='999';assert len({d['id'] for d in r['data']})==10
        return {'rows':1000,'ingestion_seconds':load,'sparse_named_exact':{**b.stats(times),'latencies_ms':times,'ids':sparse_results,'top_score':1003.},
          'hybrid':{**b.stats(hybrid),'latencies_ms':hybrid,'build_seconds':build,'lexical_scoring':'native BM25','fusion':'RRF','top_id':'999'},
          'comparison_limit':'PostgreSQL lexical ranking is ts_rank_cd, not BM25; hybrid latency is a capability diagnostic, not identical retrieval quality'}
    c=server.connection;c.execute('CREATE TABLE named(id text PRIMARY KEY,dense vector(2),terms sparsevec(1000),title text,lexical tsvector)')
    begin=time.perf_counter()
    with c.transaction():
        with c.cursor().copy('COPY named(id,dense,terms,title) FROM STDIN') as copy:
            for i in range(1000):copy.write_row((str(i),f'[{i},1]',f'{{4:{i},1000:2}}/1000','rare ocean' if i==999 else 'ocean shared'))
    load=time.perf_counter()-begin;begin=time.perf_counter()
    c.execute("UPDATE named SET lexical=to_tsvector('english',title)");c.execute('CREATE INDEX named_lexical ON named USING gin(lexical)');build=time.perf_counter()-begin
    for i in range(100):
        start=time.perf_counter();r=c.execute("SELECT id,-(terms <#> '{4:1,1000:2}/1000'::sparsevec) AS score,dense::text,terms::text,title FROM named ORDER BY terms <#> '{4:1,1000:2}/1000'::sparsevec LIMIT 10").fetchall();times.append((time.perf_counter()-start)*1000)
        assert r[0][0]=='999' and r[0][1]==1003.
        assert json.loads(r[0][2])==[999.,1.] and r[0][4]=='rare ocean'
    hybrid=[]
    sparse_ids=[x[0] for x in r]
    sql="""WITH v AS (SELECT id,row_number() OVER (ORDER BY dense <#> '[1,0]'::vector) AS rank FROM named ORDER BY dense <#> '[1,0]'::vector LIMIT 100),
    l AS (SELECT id,row_number() OVER (ORDER BY ts_rank_cd(lexical,to_tsquery('english','rare')) DESC,id) AS rank FROM named WHERE lexical @@ to_tsquery('english','rare') ORDER BY ts_rank_cd(lexical,to_tsquery('english','rare')) DESC,id LIMIT 100),
    fused AS (SELECT id,1.0/(60+rank) AS score FROM v UNION ALL SELECT id,1.0/(60+rank) FROM l)
    SELECT n.id,n.dense::text,n.title,sum(f.score) FROM fused f JOIN named n USING(id) GROUP BY n.id,n.dense,n.title ORDER BY sum(f.score) DESC,n.id LIMIT 10"""
    for i in range(100):
        start=time.perf_counter();r=c.execute(sql).fetchall();hybrid.append((time.perf_counter()-start)*1000);assert r[0][0]=='999' and len(r)==10
    return {'rows':1000,'ingestion_seconds':load,'sparse_named_exact':{**b.stats(times),'latencies_ms':times,'ids':sparse_ids,'top_score':1003.},
      'hybrid':{**b.stats(hybrid),'latencies_ms':hybrid,'build_seconds':build,'lexical_scoring':'PostgreSQL ts_rank_cd','fusion':'RRF','top_id':'999'},
      'comparison_limit':'ts_rank_cd and BM25 are different lexical scorers; no equal hybrid relevance claim'}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['build','pg','scratch','output']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--queries',type=int,default=1000);args=p.parse_args()
    for name in ['build','pg']:setattr(args,name,getattr(args,name).resolve(strict=True))
    args.scratch=args.scratch.resolve();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
    assert not any(args.output.iterdir()) and 100<=args.queries<=1000
    cpus=sorted(os.sched_getaffinity(0));args.server_cpus=cpus[:4];os.sched_setaffinity(0,cpus[4:6]);args.scratch.mkdir(parents=True,exist_ok=True)
    result={'status':'RUNNING','scope':'self-run supplemental candidate workload matrix','workloads':[],'script_sha256':b.digest(__file__),'candidate_sha256':b.digest(args.build/'db_engine')}
    try:
      for dims,metric in [(128,'cosine'),(384,'cosine'),(128,'l2'),(128,'dot')]:
        args.metric=metric;label=f'dim-{dims}-{metric}';out=args.output/label;out.mkdir();row={'dimensions':dims,'rows':10000,'metric':metric,'status':'RUNNING','systems':{}};result['workloads'].append(row)
        rng=np.random.default_rng(20261031+dims);base=rng.normal(size=(10000,dims)).astype(np.float32);base/=np.linalg.norm(base,axis=1)[:,None]
        qs=rng.normal(size=(args.queries+100,dims)).astype(np.float32);qs/=np.linalg.norm(qs,axis=1)[:,None]
        truth=b.reference(base,qs,metric=metric);servers=[]
        try:
          for kind in ['pacific','postgres']:
            server=b.Server(kind,args,len(base),dims,out/kind);server.out.mkdir();servers.append(server);server.start();server.setup()
            value={'load':server.load(base,1000),'modes':{}};row['systems'][kind]=value
            for mode in ['exact','ann']:
              value[mode+'_build']=server.build_index(mode);ef=600 if kind=='pacific' else 600
              server.plan(server.connection,mode,ef);value[mode+'_plan']=server.explain(qs[0],mode)
              for q in qs[:10]:server.query(q,mode,ef)
              records=[]
              for qi in range(100,100+args.queries):
                docs,ms,execution=server.query(qs[qi],mode,ef);records.append({'query':qi,'ms':ms,'execution':execution,**b.validate(docs,qs[qi],base,truth[qi],metric=metric)})
              summary={**b.stats([x['ms'] for x in records]),'recall_at_10':statistics.mean(x['tie_aware_recall'] for x in records),'records':records}
              assert summary['recall_at_10']>=(1 if mode=='exact' else .95);value['modes'][mode]=summary
              print('SUPPLEMENT',label,kind,mode,summary['p50_ms'],summary['recall_at_10'],flush=True)
            if dims==128 and metric=='dot':
                # Completed dense timings do not require retaining that manager's
                # declared staging reservation during a separate capability case.
                if kind=='pacific':server.connection.request({'action':'dropVectorIndex','collection':'vectors','name':'ann'})
                else:server.connection.execute('DROP INDEX vectors_hnsw')
                value['sparse_named_hybrid']=capabilities(server)
            value['storage']=b.disk(server.root/'data');server.stop();value['peak_sampled_pss_bytes']=max(x['pss'] for x in server.memory)
            b.save(args.output/'summary.json',result)
          row['status']='PASS'
        except Exception as error:row['status']='FAIL';row['error']=str(error);(out/'failure.txt').write_text(traceback.format_exc())
        finally:
          for server in servers:
            if server.proc and server.proc.poll() is None:server.stop()
          b.save(args.output/'summary.json',result)
        if row['status']!='PASS':break
      result['status']='PASS' if len(result['workloads'])==4 and all(x['status']=='PASS' for x in result['workloads']) else 'FAIL';b.save(args.output/'summary.json',result)
    finally:os.sched_setaffinity(0,cpus)
    raise SystemExit(result['status']!='PASS')
if __name__=='__main__':main()
