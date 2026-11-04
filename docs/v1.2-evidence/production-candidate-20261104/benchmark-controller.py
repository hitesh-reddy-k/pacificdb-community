import pathlib,subprocess,json,os,time,traceback,shlex
root=pathlib.Path(__file__).resolve().parents[2];os.chdir(root);out=pathlib.Path(__file__).resolve().parent;r={'status':'RUNNING','phase':'await-qualification','started_at':time.time()};original=subprocess.check_output(['powerprofilesctl','get'],text=True).strip();r['original_profile']=original
try:
 while not all((out/name).exists() for name in ['exit.json','sanitizers-exit.json','additional-gates-exit.json']):
  r['heartbeat_at']=time.time();(out/'benchmark-controller.json').write_text(json.dumps(r,indent=2));time.sleep(5)
 aux=json.loads((out/'additional-gates-exit.json').read_text());assert aux['runtime_status']=='PASS',aux
 q=json.loads((out/'exit.json').read_text());san=json.loads((out/'sanitizers-exit.json').read_text());assert q['status']=='PASS' and san['status']=='PASS',(q,san)
 link=shlex.split((root/'build-v1.2-ann-optimized/CMakeFiles/db_engine_secondary_index_build_test.dir/link.txt').read_text());libs=link[link.index('libpacificdb_engine_core.a'):];libs=[str(root/'build-v1.2-ann-optimized'/x) if x.endswith('.a') and not x.startswith('/') else x for x in libs]
 with (out/'audit-build.log').open('w') as log:subprocess.run(['/usr/bin/c++','-std=c++17','-O2','-Iengine/include','-Ibuild-v1.2-deps/usr/include',str(out/'audit-index.cpp'),*libs,'-o',str(out/'audit-index')],stdout=log,stderr=subprocess.STDOUT,check=True)
 for variant,build in [('baseline',out/'baseline'),('candidate',root/'build-v1.2-ann-optimized')]:
  r['phase']=variant
  with (out/('index-'+variant+'.log')).open('w') as log:
   cmd=['powerprofilesctl','launch','--profile','performance','--reason','Matched durable 1M metadata-index build qualification','--appid','io.pacificdb.benchmark',str(root/'build-v1.2-release/benchmark-rc2-venv/bin/python'),str(out/'measure-index.py'),'--variant',variant,'--build',str(build)]
   p=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT);r['child_pid']=p.pid
   while p.poll() is None:r['heartbeat_at']=time.time();(out/'benchmark-controller.json').write_text(json.dumps(r,indent=2));time.sleep(5)
   r[variant]={'exit_code':p.returncode};assert p.returncode==0,(variant,p.returncode)
   result=json.loads((out/('index-'+variant)/'result.json').read_text());r[variant]['status']=result['status'];assert result['status']==('PASS' if variant=='candidate' else 'BASELINE_ACK_FAIL_REPRODUCED'),result
  assert subprocess.run(['powerprofilesctl','set',original]).returncode==0
 r['status']='PASS'
except BaseException:r['status']='FAIL';r['error']=traceback.format_exc()
finally:r['restore_exit_code']=subprocess.run(['powerprofilesctl','set',original]).returncode;r['finished_at']=time.time();(out/'benchmark-exit.json').write_text(json.dumps(r,indent=2)+'\n')
