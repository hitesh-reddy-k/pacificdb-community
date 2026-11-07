#!/usr/bin/env python3
"""Reject dirty, mismatched or tampered external-testing release assets."""
import argparse,hashlib,json,re
from pathlib import Path
from release_identity import load_release_identity
from prerelease_qualification import assess

def verify(directory,revision,allow_dirty=False,require_qualified=False):
 root=Path(directory).resolve();manifest=root/'RELEASE-MANIFEST.json';m=json.loads(manifest.read_text());identity=load_release_identity()
 for name in ('version','releaseVersion'):
  if m.get(name)!=identity[name]:raise ValueError('identity mismatch:'+name)
 if m.get('revision')!=revision or not re.fullmatch('[a-f0-9]{40}',revision):raise ValueError('revision mismatch')
 if m.get('sourceClean') is not True and not allow_dirty:raise ValueError('dirty staging is not publishable')
 if m.get('prerelease') is not True or m.get('productionReady') is not False:raise ValueError('candidate must remain prerelease')
 names=set()
 if not m.get('artifacts'):raise ValueError('no artifacts')
 for artifact in m['artifacts']:
  name=artifact.get('filename','');path=root/name
  if not name or Path(name).name!=name or name in names or name.startswith('.') or name.endswith(('.key','.pem','.env','.token','.secret')):raise ValueError('unsafe or duplicate artifact path')
  names.add(name)
  if path.is_symlink() or not path.is_file():raise ValueError('artifact missing/symlink:'+name)
  expected=artifact.get('sha256','')
  if not re.fullmatch('[a-f0-9]{64}',expected) or hashlib.sha256(path.read_bytes()).hexdigest()!=expected:raise ValueError('checksum mismatch:'+name)
 actual={p.name for p in root.iterdir()}
 if actual != names | {'RELEASE-MANIFEST.json','SHA256SUMS'} and actual != names | {'RELEASE-MANIFEST.json'}:
  raise ValueError('unexpected or missing staged files')
 if require_qualified:
  evidence=m.get('qualification',{})
  result=assess(evidence)
  declared={(a['filename'],a['sha256']) for a in m['artifacts']}
  qualified={(a.get('name'),a.get('sha256')) for a in evidence.get('artifacts',[])}
  if m.get('publicationDecision')!='PREVIEW_QUALIFIED' or result['decision']!='PREVIEW_QUALIFIED' or evidence.get('revision')!=revision or evidence.get('releaseVersion')!=m['releaseVersion'] or declared!=qualified:
   raise ValueError('publication requires complete matching exact-artifact qualification')
 return m

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directory',type=Path,required=True);p.add_argument('--revision',required=True);p.add_argument('--allow-dirty-staging',action='store_true');p.add_argument('--require-qualified',action='store_true');a=p.parse_args();m=verify(a.directory,a.revision,a.allow_dirty_staging,a.require_qualified)
 print('STAGED_ARTIFACTS_PASS',len(m['artifacts']),'prerelease',m['releaseVersion'])
if __name__=='__main__':main()
