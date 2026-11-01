#!/usr/bin/env python3
"""Seal owner-reviewed local receipts into exact prerelease staging; never publish."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from prerelease_qualification import REQUIRED, assess

spec = importlib.util.spec_from_file_location('staging', Path(__file__).with_name('verify-prerelease-staging.py'))
staging = importlib.util.module_from_spec(spec); spec.loader.exec_module(staging)

def qualify(directory, receipt_file, revision):
    root = Path(directory).resolve()
    receipt_file = Path(receipt_file).resolve(strict=True)
    manifest = staging.verify(root, revision)
    receipt = json.loads(receipt_file.read_text())
    for key in ('releaseVersion','revision'):
        if receipt.get(key) != manifest.get(key):
            raise ValueError('receipt identity mismatch: '+key)
    checks = receipt.get('checks',[])
    for check in checks:
        evidence = check.get('evidence',[])
        if check.get('status') == 'Passed' and not evidence:
            raise ValueError('missing execution evidence: '+str(check.get('name')))
        for item in evidence:
            name = item.get('path','')
            if not name or Path(name).is_absolute() or '..' in Path(name).parts:
                raise ValueError('unsafe receipt evidence path')
            path = receipt_file.parent / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
                raise ValueError('missing receipt evidence: '+name)
            if hashlib.sha256(path.read_bytes()).hexdigest() != item.get('sha256'):
                raise ValueError('changed execution evidence: '+name)
    declared = {(a['filename'],a['sha256']) for a in manifest['artifacts']}
    observed = {(a.get('name'),a.get('sha256')) for a in receipt.get('artifacts',[])}
    if declared != observed or len(observed) != len(receipt.get('artifacts',[])):
        raise ValueError('receipts must cover every exact staged artifact once')
    qualification = {key:manifest[key] for key in ('releaseVersion','revision','sourceClean','productionReady','knownIssues')}
    qualification.update(checks=checks, artifacts=receipt['artifacts'])
    result = assess(qualification)
    if result['decision'] != 'PREVIEW_QUALIFIED':
        raise ValueError('qualification refused: '+json.dumps(result['reasons']))
    manifest.update(checks=checks, qualification=qualification, publicationDecision=result['decision'])
    file = root/'RELEASE-MANIFEST.json'
    temporary = file.with_suffix('.tmp')
    temporary.write_text(json.dumps(manifest,indent=2)+'\n'); temporary.replace(file)
    sums = {a['filename']:a['sha256'] for a in manifest['artifacts']}
    sums[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
    (root/'SHA256SUMS').write_text(''.join(h+'  '+n+'\n' for n,h in sorted(sums.items())))
    staging.verify(root, revision, require_qualified=True)
    return manifest

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--directory',type=Path,required=True)
    p.add_argument('--receipts',type=Path,required=True)
    p.add_argument('--revision',required=True)
    a=p.parse_args();m=qualify(a.directory,a.receipts,a.revision)
    print('PREVIEW_QUALIFIED',m['revision'],len(m['artifacts']),'productionReady=false')

if __name__=='__main__':main()
