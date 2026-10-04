#!/usr/bin/env python3
"""Refuse mismatched tags, versions, revisions or unqualified package manifests."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]

def validate(tag,version,manifest=None,revision=None):
    if not re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)',tag) or tag!='v'+version:raise ValueError('release tag/version mismatch')
    if manifest and (manifest.get('status')!='PASS' or manifest.get('version')!=version or manifest.get('git_revision')!=revision or manifest.get('source_clean') is not True):raise ValueError('package manifest is not qualified for this clean tagged revision')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('tag');parser.add_argument('--manifest',type=Path);parser.add_argument('--require-tag',action='store_true');args=parser.parse_args()
    version=re.search(r'^version\s*=\s*"([^"]+)"',(ROOT/'sdk/python/pyproject.toml').read_text(),re.M).group(1)
    ns={'m':'http://maven.apache.org/POM/4.0.0'};java=ET.parse(ROOT/'sdk/java/pom.xml').getroot().findtext('m:version',None,ns)
    if java!=version:raise ValueError('SDK versions differ')
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    manifest=json.loads(args.manifest.read_text()) if args.manifest else None;validate(args.tag,version,manifest,revision)
    if args.require_tag:
        tagged=subprocess.check_output(['git','rev-parse','refs/tags/'+args.tag+'^{commit}'],cwd=ROOT,text=True).strip()
        if tagged!=revision:raise ValueError('checkout does not match release tag')
    print('PASS: SDK release tag/version/revision guard')

if __name__=='__main__':main()
