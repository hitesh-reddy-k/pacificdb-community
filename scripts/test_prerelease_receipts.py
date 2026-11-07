import importlib.util
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from prerelease_qualification import REQUIRED

spec = importlib.util.spec_from_file_location('receipts', Path(__file__).with_name('qualify-prerelease.py'))

class Receipts(unittest.TestCase):
    def test_exact_receipts_are_required_and_tampering_is_rejected(self):
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); out = root / 'staged'; out.mkdir()
            payload = out / 'engine.tgz'; payload.write_bytes(b'tested bytes')
            artifact = {'filename':payload.name,'sha256':hashlib.sha256(payload.read_bytes()).hexdigest(),'sizeBytes':payload.stat().st_size}
            manifest = {'version':'1.2.0','releaseVersion':'1.2.0-rc.2','revision':'a'*40,'sourceClean':True,'prerelease':True,'productionReady':False,'publicationDecision':'BLOCKED','knownIssues':['ANN rebuild after restart'],'artifacts':[artifact]}
            (out/'RELEASE-MANIFEST.json').write_text(json.dumps(manifest))
            log = root/'observed.log'; log.write_text('test receipt')
            evidence = {'path':log.name,'sha256':hashlib.sha256(log.read_bytes()).hexdigest()}
            receipt = {'releaseVersion':'1.2.0-rc.2','revision':'a'*40,'checks':[{'name':name,'status':'Passed','evidence':[evidence]} for name in REQUIRED], 'artifacts':[{'name':payload.name,'sha256':artifact['sha256'],'platform':'linux-x64','tested':True}]}
            file = root/'receipt.json'; file.write_text(json.dumps(receipt))
            result = module.qualify(out,file,'a'*40)
            self.assertEqual(result['publicationDecision'],'PREVIEW_QUALIFIED')
            log.write_text('changed')
            with self.assertRaises(ValueError):module.qualify(out,file,'a'*40)
            log.write_text('test receipt')
            for change in ('revision','failed','missing','artifact','escape'):
                bad = json.loads(json.dumps(receipt))
                if change=='revision':bad['revision']='b'*40
                if change=='failed':bad['checks'][0]['status']='Failed'
                if change=='missing':bad['checks'][0]['evidence']=[]
                if change=='artifact':bad['artifacts'][0]['sha256']='f'*64
                if change=='escape':bad['checks'][0]['evidence'][0]['path']='../secret'
                file.write_text(json.dumps(bad))
                with self.assertRaises(ValueError):module.qualify(out,file,'a'*40)

if __name__=='__main__':unittest.main()
