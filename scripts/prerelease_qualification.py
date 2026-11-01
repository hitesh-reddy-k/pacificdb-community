#!/usr/bin/env python3
"""Evaluate external-testing evidence, separately from stable release policy."""
import argparse
import json
import re
from pathlib import Path

REQUIRED=('identity','engine','document-media','authentication','vector','cli','workbench','installed-sdks','artifact-integrity','security-blockers')
STABLE_ONLY=['platform-matrix','mixed-version-upgrade','physical-power','independent-security','sustained-load']

def assess(data):
    reasons=[]
    if not re.fullmatch(r'\d+\.\d+\.\d+-rc\.[1-9]\d*',str(data.get('releaseVersion',''))):reasons.append('candidate_identity_required')
    if not re.fullmatch(r'[0-9a-f]{40}',str(data.get('revision',''))):reasons.append('exact_revision_required')
    if data.get('sourceClean') is not True:reasons.append('clean_source_required')
    if data.get('productionReady') is not False:reasons.append('preview_must_not_claim_production')
    if not data.get('knownIssues'):reasons.append('limitations_required')
    checks=data.get('checks',[])
    names=[c.get('name') for c in checks]
    allowed={'Passed','Failed','Blocked','Not applicable','Not tested'}
    if any(c.get('status') not in allowed for c in checks):reasons.append('invalid_check_status')
    if len(names)!=len(set(names)):reasons.append('duplicate_check')
    for name in REQUIRED:
        if not any(c.get('name')==name and c.get('status')=='Passed' for c in checks):reasons.append('required_check_not_passed:'+name)
    artifacts=data.get('artifacts',[])
    if not artifacts:reasons.append('tested_artifacts_required')
    for a in artifacts:
        if a.get('tested') is not True or not a.get('platform') or not a.get('name') or not re.fullmatch(r'[a-f0-9]{64}',str(a.get('sha256',''))):reasons.append('unverified_artifact:'+str(a.get('name')))
    failed=any(c.get('status')=='Failed' for c in checks)
    return {'decision':'FAIL' if failed else 'BLOCKED' if reasons else 'PREVIEW_QUALIFIED',
      'productionReady':False,'releaseVersion':data.get('releaseVersion'),'revision':data.get('revision'),
      'reasons':reasons,'unverifiedStableGates':STABLE_ONLY,'artifacts':artifacts,
      'scope':'external-testing prerelease only; stable policy remains unchanged'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    result=assess(json.loads(a.evidence.read_text()));a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n');print(result['decision'])
    raise SystemExit(result['decision']!='PREVIEW_QUALIFIED')
if __name__=='__main__':main()
