#!/usr/bin/env python3
"""Build and test local candidate distributions. Never publish or change Git refs."""
import argparse,hashlib,json,os,platform,shutil,subprocess,sys,tarfile
from pathlib import Path
from release_identity import load_release_identity

ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--sdk-staging',type=Path);p.add_argument('--workbench-staging',type=Path);p.add_argument('--allow-dirty-staging',action='store_true');a=p.parse_args()
 build=a.build.resolve(strict=True);out=a.output.resolve();identity=load_release_identity(ROOT)
 out.mkdir(parents=True,exist_ok=True)
 if any(out.iterdir()):p.error('output must be empty; preserve previous attempts')
 logs=out.parent/(out.name+'-logs');logs.mkdir(exist_ok=True)
 revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
 dirty=bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=all'],cwd=ROOT,text=True).strip())
 source_status=subprocess.check_output(['git','status','--porcelain','--untracked-files=all'],cwd=ROOT)
 if dirty and not a.allow_dirty_staging:p.error('source must be clean for candidate staging (or explicitly allow nonpublishable dirty rehearsal)')
 def run(name,command,env=None,cwd=ROOT):
  print('STAGING',name,flush=True)
  with (logs/(name+'.log')).open('w') as log:subprocess.run([str(x) for x in command],cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 engine=build/('db_engine.exe' if os.name=='nt' else 'db_engine')
 info=json.loads(subprocess.check_output([str(engine),'--build-info'],text=True))
 for k,v in [('engineVersion',identity['version']),('releaseVersion',identity['releaseVersion']),('releaseChannel','prerelease'),('releaseTag',identity['tag']),('gitCommit',revision)]:
  if info.get(k)!=v:raise ValueError('native identity mismatch:'+k)
 release=identity['releaseVersion'];checks=[];artifacts=[]
 def add(path):
  dest=out/path.name
  if dest.exists():raise ValueError('duplicate staged artifact:'+dest.name)
  shutil.copy2(path,dest);artifacts.append({'filename':dest.name,'sha256':hashlib.sha256(dest.read_bytes()).hexdigest(),'sizeBytes':dest.stat().st_size})
 if platform.system()!='Linux' or platform.machine() not in ('x86_64','AMD64'):raise ValueError('this staging driver qualifies Linux x64 only')
 prefix=logs/'native-install';run('native-install',['cmake','--install',build,'--prefix',prefix])
 archive=logs/f'pacificdb-community-{release}-linux-x64.tar.gz'
 with tarfile.open(archive,'w:gz') as t:t.add(prefix,arcname=f'pacificdb-community-{release}-linux-x64')
 run('archive-version',[prefix/'bin/pacificdb','--version']);add(archive)
 package=logs/'cpack';package.mkdir(exist_ok=True)
 run('native-deb',['cpack','--config',build/'CPackConfig.cmake','-G','DEB','-B',package])
 debs=list(package.glob('*.deb'))
 if len(debs)!=1:raise ValueError('expected one native Debian package')
 deb=logs/f'pacificdb-community-{release}-linux-x64.deb';shutil.copy2(debs[0],deb)
 run('native-package-smoke',['bash','scripts/test-native-package.sh',deb,release]);add(deb);checks.append({'name':'native-debian-installed','status':'Passed','platform':'linux-x64'})
 sdk=a.sdk_staging.resolve(strict=True) if a.sdk_staging else logs/'sdk'
 if not a.sdk_staging:
  for d in ('npm','python','java'):(sdk/d).mkdir(parents=True,exist_ok=True)
  for package_name in ('@pacificdb/client','@pacificdb/cli'):run('pack-'+package_name.split('/')[-1],['npm','pack','--workspace',package_name,'--ignore-scripts','--pack-destination',sdk/'npm'])
  run('python-distributions',[sys.executable,'-m','build','--outdir',sdk/'python','sdk/python'])
  run('java-distributions',['mvn','-B','-q','-f','sdk/java/pom.xml','package'])
  for name in (f'pacificdb-client-{release}.jar',f'pacificdb-client-{release}-sources.jar',f'pacificdb-client-{release}-javadoc.jar'):shutil.copy2(ROOT/'sdk/java/target'/name,sdk/'java'/name)
  shutil.copy2(ROOT/'sdk/java/pom.xml',sdk/'java'/f'pacificdb-client-{release}.pom')
 env={**os.environ,'PACIFICDB_TEST_ENGINE_BUILD':str(build),'PACIFICDB_EXPECTED_ENGINE_VERSION':identity['version'],'PACIFICDB_VECTOR_PREVIEW_ENABLED':'1','PACIFICDB_TEST_NATIVE_VECTOR_PREVIEW':'1','PACIFICDB_TEST_VECTOR_SCHEMA':'1','PACIFICDB_TEST_TYPED_VECTORS':'1'}
 run('installed-sdks',['bash','scripts/test-staged-sdk.sh',sdk,logs/'installed-sdks'],env)
 for folder,glob in [('npm','*.tgz'),('python','*.whl'),('python','*.tar.gz'),('java','*.jar'),('java','*.pom')]:
  for path in sorted((sdk/folder).glob(glob)):add(path)
 checks.append({'name':'installed-sdks','status':'Passed','platform':'linux-x64'})
 workbench_files=[]
 if a.workbench_staging:
  for file in sorted(a.workbench_staging.resolve(strict=True).iterdir()):
   if file.suffix in ('.deb','.gz') and file.is_file():add(file);workbench_files.append(file.name)
 # Workbench receipts are checked by the release owner; merely copying is never a Passed install.
 checks.append({'name':'workbench','status':'Not tested' if not workbench_files else 'Blocked','reason':'requires exact-artifact GUI/install receipt'})
 docs=['RELEASE_NOTES_v1.2.md','INSTALLATION.md','SDK_GUIDE.md','ARCHITECTURE.md','TESTING.md','COMPANY_TESTING_GUIDE.md','RELEASE_CHECKLIST.md','CHANGELOG.md','LICENSE','THIRD_PARTY_NOTICES.md','release.json']
 docarchive=logs/f'pacificdb-docs-{release}.tar.gz'
 with tarfile.open(docarchive,'w:gz') as t:
  for name in docs:t.add(ROOT/name,arcname=name)
  for file in sorted((ROOT/'docs').glob('V1.2_*.md')):t.add(file,arcname=file.relative_to(ROOT))
  for name in ['docs/releases/1.2.0-development.md','docs/v1.2-evidence/community-verified-cache-comparison-20261101/REPORT.md','docs/v1.2-evidence/community-verified-cache-comparison-20261101/results.json','docs/v1.2-evidence/community-verified-cache-comparison-20261101/METHODOLOGY_ADDENDUM.md']:t.add(ROOT/name,arcname=name)
 add(docarchive)
 if subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()!=revision or subprocess.check_output(['git','status','--porcelain','--untracked-files=all'],cwd=ROOT)!=source_status:
  raise RuntimeError('source changed during staging; preserve this attempt and restart from frozen source')
 manifest={'schemaVersion':1,'version':identity['version'],'releaseVersion':release,'releaseName':identity['name'],'tag':identity['tag'],'branch':identity['branch'],'revision':revision,'sourceClean':not dirty,'prerelease':True,'productionReady':False,'publicationDecision':'BLOCKED','artifacts':artifacts,'checks':checks,'unverified':['complete stable platform/upgrade/power/security qualification','registry publication and external accessibility'],'knownIssues':['ANN rebuild after restart','strict ANN write-time unavailability','ANN/ingestion/filtering performance gaps'],'buildInfo':info,'host':{'system':platform.system(),'machine':platform.machine(),'platform':platform.platform()}}
 (out/'RELEASE-MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
 sums={x['filename']:x['sha256'] for x in artifacts};sums['RELEASE-MANIFEST.json']=hashlib.sha256((out/'RELEASE-MANIFEST.json').read_bytes()).hexdigest()
 (out/'SHA256SUMS').write_text(''.join(h+'  '+name+'\n' for name,h in sorted(sums.items())))
 print('STAGING_COMPLETE',out,'publication BLOCKED until exact receipts and approval',flush=True)
if __name__=='__main__':main()
