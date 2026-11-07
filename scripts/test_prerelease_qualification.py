import unittest
from prerelease_qualification import assess

class PreviewQualification(unittest.TestCase):
    def baseline(self):
        return {'releaseVersion':'1.2.0-rc.2','revision':'a'*40,'sourceClean':True,
          'checks':[{'name':n,'status':'Passed'} for n in ('identity','engine','document-media','authentication','vector','cli','workbench','installed-sdks','artifact-integrity','security-blockers','disk-full-recovery')],
          'artifacts':[{'name':'engine.tgz','sha256':'f'*64,'tested':True,'platform':'linux-x64'}],
          'knownIssues':['ANN rebuild required after restart'],'productionReady':False}
    def test_preview_is_never_stable(self):
        result=assess(self.baseline());self.assertEqual(result['decision'],'PREVIEW_QUALIFIED');self.assertFalse(result['productionReady'])
        self.assertEqual(result['unverifiedStableGates'],['platform-matrix','mixed-version-upgrade','physical-power','independent-security','sustained-load'])
    def test_any_missing_failed_or_unverified_required_check_blocks(self):
        for status in ('Failed','Blocked','Not tested','Not applicable'):
            data=self.baseline();data['checks'][0]['status']=status
            self.assertNotEqual(assess(data)['decision'],'PREVIEW_QUALIFIED')
        data=self.baseline();data['checks'].pop();self.assertEqual(assess(data)['decision'],'BLOCKED')
    def test_source_artifact_and_release_identity_cannot_be_faked(self):
        for key,value in [('sourceClean',False),('productionReady',True),('revision','unknown'),('releaseVersion','1.2.0')]:
            data=self.baseline();data[key]=value
            self.assertEqual(assess(data)['decision'],'BLOCKED')
        data=self.baseline();data['artifacts'][0]['tested']=False;self.assertEqual(assess(data)['decision'],'BLOCKED')
        data=self.baseline();data['artifacts'][0]['sha256']='bad';self.assertEqual(assess(data)['decision'],'BLOCKED')
    def test_failed_optional_gate_and_unknown_status_fail_closed(self):
        data=self.baseline();data['checks'].append({'name':'dependency-audit','status':'Failed'})
        self.assertEqual(assess(data)['decision'],'FAIL')
        data=self.baseline();data['checks'][0]['status']='PASS'
        self.assertEqual(assess(data)['decision'],'BLOCKED')
    def test_unverified_additional_platform_does_not_became_verified(self):
        data=self.baseline();data['artifacts'].append({'name':'windows.exe','sha256':'e'*64,'tested':False,'platform':'windows-x64'})
        self.assertEqual(assess(data)['decision'],'BLOCKED')

if __name__=='__main__':unittest.main()
