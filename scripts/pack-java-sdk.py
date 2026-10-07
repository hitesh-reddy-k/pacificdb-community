#!/usr/bin/env python3
"""Verify/sign exact already-built Java artifacts and create a local Central ZIP; no upload."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

spec=importlib.util.spec_from_file_location('inspect_sdk',Path(__file__).with_name('verify-sdk-packages.py'))
inspect_sdk=importlib.util.module_from_spec(spec);spec.loader.exec_module(inspect_sdk)

def bundle(target,version,output,fingerprint,manifest=None,sign=False):
    base='pacificdb-client-'+version
    files=[target/(base+suffix) for suffix in ('.pom','.jar','-sources.jar','-javadoc.jar')]
    inspect_sdk.java_pom(files[0],version);inspect_sdk.java_jars(*files[1:])
    if manifest:
        expected={Path(a['path']).name:a['sha256'] for a in json.loads(manifest.read_text())['artifacts']}
        for file in files:inspect_sdk.require(expected.get(file.name)==hashlib.sha256(file.read_bytes()).hexdigest(),'tested Java artifact hash changed')
    inspect_sdk.require(fingerprint and len(fingerprint)==40 and all(c in '0123456789abcdefABCDEF' for c in fingerprint),'exact GPG primary fingerprint required')
    with tempfile.TemporaryDirectory() as temporary:
        staged={}
        for file in files:
            signature=file.with_name(file.name+'.asc')
            if sign:
                signature=Path(temporary)/(file.name+'.asc')
                command=['gpg','--batch','--pinentry-mode','loopback','--local-user',fingerprint,'--armor','--detach-sign','--output',str(signature)]
                phrase=os.environ.get('GPG_PASSPHRASE')
                if phrase is not None:command+=['--passphrase-fd','0']
                command.append(str(file))
                result=subprocess.run(command,input=(phrase+'\n').encode() if phrase is not None else None,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                inspect_sdk.require(result.returncode==0,'GPG signing failed')
            inspect_sdk.require(signature.is_file(),'missing Java detached signature')
            result=subprocess.run(['gpg','--batch','--status-fd','1','--verify',str(signature),str(file)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            status=result.stdout.decode(errors='replace')
            inspect_sdk.require(result.returncode==0 and any(line.startswith('[GNUPG:] VALIDSIG ') and fingerprint.upper() in line.split() for line in status.splitlines()),'GPG signature or primary fingerprint mismatch')
            for artifact in [file,signature]:
                data=artifact.read_bytes();staged[artifact.name]=data
                for algorithm in ('md5','sha1','sha256','sha512'):staged[artifact.name+'.'+algorithm]=hashlib.new(algorithm,data).hexdigest().encode()
        output.parent.mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED) as archive:
            prefix='io/pacificdb/pacificdb-client/'+version+'/'
            for name,data in sorted(staged.items()):archive.writestr(prefix+name,data)
    print('PASS: local signed Java bundle; no upload performed')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',type=Path,required=True);parser.add_argument('--version',default='1.1.2')
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--fingerprint',required=True)
    parser.add_argument('--manifest',type=Path);parser.add_argument('--sign',action='store_true')
    args=parser.parse_args();bundle(args.target,args.version,args.output,args.fingerprint,args.manifest,args.sign)

if __name__=='__main__':main()
