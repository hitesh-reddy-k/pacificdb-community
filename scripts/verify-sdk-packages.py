#!/usr/bin/env python3
"""Inspect SDK archives locally. This tool never signs or publishes artifacts."""
import argparse
import base64
import csv
import email
import hashlib
import io
import json
import struct
import tarfile
import zipfile
from pathlib import Path, PurePosixPath

PYTHON_MODULES=('__init__','client','connection','transport','errors','operations','files')

def require(condition,message):
    if not condition:raise ValueError(message)

def names_valid(names):
    require(len(set(names))==len(names),'duplicate archive entries')
    for name in names:
        parts=PurePosixPath(name).parts
        require(not name.startswith('/') and '\\' not in name and '..' not in parts,'unsafe archive path')
        require(not any(part in ('.env','__pycache__','.pytest_cache','node_modules','.git','target') or part.endswith(('.key','.pem','.p12','.pyc','.token','.secret')) for part in parts),'private/build file included')

def inventory(path,names):
    return {'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'size_bytes':path.stat().st_size,'entries':sorted(names)}

def python_wheel(path,version):
    path=Path(path)
    with zipfile.ZipFile(path) as archive:
        names=archive.namelist();names_valid(names)
        for module in PYTHON_MODULES:require(f'pacificdb/{module}.py' in names,'missing Python public module')
        prefix=f'pacificdb-{version}.dist-info/'
        require(prefix+'METADATA' in names and prefix+'RECORD' in names and prefix+'WHEEL' in names,'missing wheel metadata')
        metadata=email.message_from_bytes(archive.read(prefix+'METADATA'))
        require(metadata['Name']=='pacificdb' and metadata['Version']==version,'Python name/version mismatch')
        require(metadata['Requires-Python']=='>=3.10','Python runtime floor mismatch')
        require(metadata['License-Expression']=='Apache-2.0','Python license mismatch')
        require(not metadata.get_all('Requires-Dist'),'unexpected Python runtime dependency')
        require(metadata['Description-Content-Type']=='text/markdown' and len(metadata.get_payload().strip())>5,'missing Markdown README')
        require(metadata['Author'] or metadata['Author-email'],'missing author')
        require(any('github.com/hitesh-reddy-k/pacificdb-community' in url for url in metadata.get_all('Project-URL',[])),'missing repository URL')
        license=prefix+'licenses/LICENSE';require(license in names and b'Apache License' in archive.read(license),'missing Apache license file')
        init=archive.read('pacificdb/__init__.py').decode()
        require(all(symbol in init for symbol in ('PacificDBClient','PacificDB =','PacificDBError','MediaUploadError')),'missing Python public export')
        records=list(csv.reader(io.StringIO(archive.read(prefix+'RECORD').decode())))
        require(len(records)==len(names) and {r[0] for r in records}==set(names),'wheel RECORD coverage mismatch')
        for name,digest,size in records:
            if name==prefix+'RECORD':continue
            data=archive.read(name)
            expected='sha256='+base64.urlsafe_b64encode(hashlib.sha256(data).digest()).decode().rstrip('=')
            require(digest==expected and size==str(len(data)),'wheel RECORD hash/size mismatch')
    return inventory(path,names)

def python_sdist(path,version):
    path=Path(path)
    with tarfile.open(path,'r:gz') as archive:
        members=archive.getmembers();names=[m.name for m in members];names_valid(names)
        require(all(m.isfile() or m.isdir() for m in members),'linked/special file in source archive')
        prefix=f'pacificdb-{version}/'
        require(all(n==prefix[:-1] or n.startswith(prefix) for n in names),'sdist root/version mismatch')
        for file in ['README.md','LICENSE','pyproject.toml',*[f'pacificdb/{m}.py' for m in PYTHON_MODULES]]:
            require(prefix+file in names and archive.getmember(prefix+file).isfile(),'missing source package file: '+file)
    return inventory(path,names)

JAVA_CLASSES=('PacificDB','PacificDBClient','ConnectionOptions','Operations','PacificDBException','MediaUploadException')

def java_pom(path,version):
    import xml.etree.ElementTree as ET
    root=ET.parse(path).getroot();ns={'m':'http://maven.apache.org/POM/4.0.0'}
    def text(field):return root.findtext('m:'+field,None,ns)
    require(text('groupId')=='io.pacificdb' and text('artifactId')=='pacificdb-client' and text('version')==version,'Java coordinates mismatch')
    for field in ['name','description','url','licenses/m:license/m:name','licenses/m:license/m:url','licenses/m:license/m:distribution','developers/m:developer/m:id','developers/m:developer/m:name','developers/m:developer/m:url','scm/m:connection','scm/m:developerConnection','scm/m:url']:
        require(text(field),'missing Java metadata: '+field)
    require(text('properties/m:maven.compiler.release')=='11','Java release floor mismatch')
    require(text('properties/m:central.skipPublishing')=='true','unsafe local publication default')
    dependencies=root.findall('m:dependencies/m:dependency',ns)
    runtime=[d for d in dependencies if d.findtext('m:scope','compile',ns) not in ('test','provided')]
    require(len(runtime)==1 and runtime[0].findtext('m:groupId',None,ns)=='com.fasterxml.jackson.core' and runtime[0].findtext('m:artifactId',None,ns)=='jackson-databind','unexpected Java runtime dependency')
    central=root.find("m:profiles/m:profile[m:id='central']/m:build/m:plugins/m:plugin[m:artifactId='central-publishing-maven-plugin']",ns)
    require(central is not None,'missing safe Central profile')
    require(central.findtext('m:version',None,ns)=='0.11.0','Central plugin version mismatch')
    for key,value in [('autoPublish','false'),('waitUntil','validated'),('checksums','all'),('skipPublishing','${central.skipPublishing}')]:
        require(central.findtext('m:configuration/m:'+key,None,ns)==value,'unsafe/missing Central option: '+key)

def java_jars(binary,sources,docs):
    results=[]
    for path,suffix in [(Path(binary),'.class'),(Path(sources),'.java'),(Path(docs),'.html')]:
        require(path.is_file(),'missing Java archive')
        with zipfile.ZipFile(path) as archive:
            names=archive.namelist();names_valid(names)
            for cls in JAVA_CLASSES:require('io/pacificdb/'+cls+suffix in names,'missing public Java API: '+cls+suffix)
            if suffix=='.class':
                require('META-INF/LICENSE' in names and b'Apache License' in archive.read('META-INF/LICENSE'),'missing Java license')
                for name in names:
                    if name.endswith('.class'):
                        data=archive.read(name);require(len(data)>=8 and data[:4]==b'\xca\xfe\xba\xbe' and struct.unpack('>H',data[6:8])[0]==55,'Java 11 bytecode mismatch')
            results.append(inventory(path,names))
    return results

def java_artifacts(target,version):
    target=Path(target);base='pacificdb-client-'+version
    artifacts=java_jars(target/(base+'.jar'),target/(base+'-sources.jar'),target/(base+'-javadoc.jar'))
    pom=target/(base+'.pom')
    require(pom.is_file(),'missing packaged Java POM')
    java_pom(pom,version)
    artifacts.append(inventory(pom,[]))
    bundles=list((target/'central-publishing').glob('*.zip'));require(len(bundles)==1,'exactly one signed Central bundle required')
    bundle=bundles[0]
    with zipfile.ZipFile(bundle) as archive:
        names=archive.namelist();names_valid(names);prefix='io/pacificdb/pacificdb-client/'+version+'/'
        for suffix in ['.pom','.jar','-sources.jar','-javadoc.jar']:
            name=prefix+base+suffix;require(name in names,'missing bundled artifact')
            data=archive.read(name)
            signature=name+'.asc';require(signature in names and b'BEGIN PGP SIGNATURE' in archive.read(signature),'missing detached bundle signature')
            for algorithm in ('md5','sha1','sha256','sha512'):
                require(name+'.'+algorithm in names and archive.read(name+'.'+algorithm).decode().strip()==hashlib.new(algorithm,data).hexdigest(),'bundle checksum mismatch')
            if suffix!='.pom':require(hashlib.sha256(data).hexdigest()==next(a['sha256'] for a in artifacts if Path(a['path']).name==base+suffix),'bundle differs from inspected JAR')
    artifacts.append(inventory(bundle,names))
    return artifacts

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python-dist',type=Path);parser.add_argument('--java-target',type=Path)
    parser.add_argument('--version',default='1.1.2');parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--hash-manifest',type=Path)
    args=parser.parse_args();artifacts=[]
    if args.python_dist:
        wheels=list(args.python_dist.glob('*.whl'));sources=list(args.python_dist.glob('*.tar.gz'))
        require(len(wheels)==len(sources)==1,'exactly one wheel and source archive required')
        artifacts.extend([python_wheel(wheels[0],args.version),python_sdist(sources[0],args.version)])
    if args.java_target:
        artifacts.extend(java_artifacts(args.java_target,args.version))
    require(artifacts,'no artifacts inspected')
    if args.hash_manifest:
        recorded=json.loads(args.hash_manifest.read_text())['artifacts']
        expected={Path(a['path']).name:a['sha256'] for a in recorded}
        require(all(expected.get(Path(a['path']).name)==a['sha256'] for a in artifacts),'artifact hash changed')
    import subprocess
    revision=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
    clean=not subprocess.check_output(['git','status','--porcelain'],text=True).strip()
    report={'status':'PASS','version':args.version,'git_revision':revision,'source_clean':clean,'publication':'not performed','artifacts':artifacts}
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS: inspected {len(artifacts)} SDK artifacts locally')

if __name__=='__main__':main()
