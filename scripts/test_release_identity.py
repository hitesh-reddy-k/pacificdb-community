"""Release candidates retain one base version and native ecosystem identities."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseIdentity(unittest.TestCase):
    def test_candidate_and_pep440_mapping(self):
        from release_identity import load_release_identity
        value = load_release_identity(ROOT)
        self.assertEqual(value['version'], '1.2.0')
        self.assertEqual(value['releaseVersion'], '1.2.0-rc.1')
        self.assertEqual(value['tag'], 'v1.2.0-rc.1')
        self.assertEqual(value['pythonVersion'], '1.2.0rc1')
        self.assertEqual(value['npmVersion'], value['javaVersion'])
        self.assertEqual(value['channel'], 'prerelease')

    def test_conflicting_and_invalid_identity_rejected(self):
        from release_identity import load_release_identity
        original = load_release_identity(ROOT)
        for key, bad in [('version','1.3.0'), ('tag','v1.2.0'),
                         ('pythonVersion','1.2.0'), ('channel','stable'),
                         ('releaseVersion','1.2.0-rc.01')]:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as d:
                data = {**original,key:bad}
                (Path(d)/'release.json').write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    load_release_identity(Path(d))

    def test_machine_readable_field(self):
        result = subprocess.run(['python3',str(ROOT/'scripts/release_identity.py'),
                                 '--field','releaseVersion'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.strip(),'1.2.0-rc.1')


if __name__ == '__main__':
    unittest.main()
