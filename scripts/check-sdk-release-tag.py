#!/usr/bin/env python3
"""Refuse mismatched tags, ecosystem versions, revisions or unqualified manifests."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET
from release_identity import load_release_identity

ROOT=Path(__file__).resolve().parents[1]

def versions(tag):
    match=re.fullmatch(r'v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))(?:-(alpha|beta|rc)\.([1-9][0-9]*))?',tag)
    if not match:raise ValueError('invalid release tag')
    base,kind,number=match.groups()
    semver=tag[1:]
    python=base+({'alpha':'a','beta':'b','rc':'rc'}[kind]+number if kind else '')
    return semver,python

def validate(tag,version,manifest=None,revision=None,java_version=None):
    release,python=versions(tag)
    if version!=python or (java_version is not None and java_version!=release):raise ValueError('release tag/version mismatch')
    if manifest and (manifest.get('status')!='PASS' or manifest.get('version')!=release or manifest.get('git_revision')!=revision or manifest.get('source_clean') is not True):raise ValueError('package manifest is not qualified for this clean tagged revision')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('tag');parser.add_argument('--manifest',type=Path);parser.add_argument('--require-tag',action='store_true');args=parser.parse_args()
    version=re.search(r'^version\s*=\s*"([^"]+)"',(ROOT/'sdk/python/pyproject.toml').read_text(),re.M).group(1)
    ns={'m':'http://maven.apache.org/POM/4.0.0'};java=ET.parse(ROOT/'sdk/java/pom.xml').getroot().findtext('m:version',None,ns)
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    manifest=json.loads(args.manifest.read_text()) if args.manifest else None;validate(args.tag,version,manifest,revision,java)
    release,_=versions(args.tag)
    for file in ['sdk/node/package.json','cli/package.json']:
        package=json.loads((ROOT/file).read_text())
        if package['version']!=release:raise ValueError('npm version mismatch')
        if file.startswith('cli/') and package['dependencies']['@pacificdb/client']!=release:raise ValueError('CLI dependency mismatch')
    identity=load_release_identity(ROOT)
    if identity['tag']!=args.tag or identity['releaseVersion']!=release or identity['pythonVersion']!=version or identity['javaVersion']!=java:raise ValueError('canonical release identity mismatch')
    if args.require_tag:
        tagged=subprocess.check_output(['git','rev-parse','refs/tags/'+args.tag+'^{commit}'],cwd=ROOT,text=True).strip()
        if tagged!=revision:raise ValueError('checkout does not match release tag')
    print('PASS: SDK release tag/version/revision guard')
if __name__=='__main__':main()
