import importlib.util,json,hashlib,tempfile,unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('staged',Path(__file__).with_name('verify-prerelease-staging.py'))
class Staging(unittest.TestCase):
 def test_hash_identity_revision_and_safe_paths(self):
  m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
  with tempfile.TemporaryDirectory() as t:
   p=Path(t);(p/'engine.tgz').write_bytes(b'example-test-artifact')
   data={'version':'1.2.0','releaseVersion':'1.2.0-rc.2','revision':'a'*40,'sourceClean':True,'prerelease':True,'productionReady':False,'artifacts':[{'filename':'engine.tgz','sha256':hashlib.sha256((p/'engine.tgz').read_bytes()).hexdigest()}]}
   (p/'RELEASE-MANIFEST.json').write_text(json.dumps(data));self.assertTrue(m.verify(p,'a'*40))
   data['publicationDecision']='PREVIEW_QUALIFIED'
   (p/'RELEASE-MANIFEST.json').write_text(json.dumps(data))
   with self.assertRaises(ValueError):m.verify(p,'a'*40,require_qualified=True)
   (p/'unexpected.json').write_text('{}')
   with self.assertRaises(ValueError):m.verify(p,'a'*40)
   (p/'unexpected.json').unlink()
   for key,bad in [('revision','b'*40),('sourceClean',False),('prerelease',False),('productionReady',True)]:
    broken={**data,key:bad};(p/'RELEASE-MANIFEST.json').write_text(json.dumps(broken))
    with self.assertRaises(ValueError):m.verify(p,'a'*40)
   for bad in ['../engine.tgz','/etc/passwd','.env']:
    broken={**data,'artifacts':[{'filename':bad,'sha256':'f'*64}]};(p/'RELEASE-MANIFEST.json').write_text(json.dumps(broken))
    with self.assertRaises(ValueError):m.verify(p,'a'*40)
   (p/'RELEASE-MANIFEST.json').write_text(json.dumps(data));(p/'engine.tgz').write_bytes(b'tamper')
   with self.assertRaises(ValueError):m.verify(p,'a'*40)
if __name__=='__main__':unittest.main()
