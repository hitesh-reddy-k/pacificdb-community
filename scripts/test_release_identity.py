"""Release candidates retain one base version and native ecosystem identities."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseIdentity(unittest.TestCase):
    def fixture(self):
        return dict(schemaVersion=1, version='1.2.0', releaseVersion='1.2.0-rc.2',
                    tag='v1.2.0-rc.2', pythonVersion='1.2.0rc2', npmVersion='1.2.0-rc.2',
                    javaVersion='1.2.0-rc.2', channel='prerelease', name='Pre-release v1.2', branch='v1.2')

    def test_candidate_and_stable_mapping(self):
        from release_identity import load_release_identity
        candidate = self.fixture()
        stable = {**candidate, 'releaseVersion':'1.2.0', 'tag':'v1.2.0',
                  'pythonVersion':'1.2.0', 'npmVersion':'1.2.0', 'javaVersion':'1.2.0',
                  'channel':'stable', 'name':'PacificDB 1.2.0'}
        for identity in (candidate, stable):
            with self.subTest(channel=identity['channel']), tempfile.TemporaryDirectory() as d:
                (Path(d)/'release.json').write_text(json.dumps(identity))
                self.assertEqual(load_release_identity(Path(d)), identity)

    def test_conflicting_and_invalid_identity_rejected(self):
        from release_identity import load_release_identity
        original = self.fixture()
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
        self.assertEqual(result.stdout.strip(),json.loads((ROOT/'release.json').read_text())['releaseVersion'])


if __name__ == '__main__':
    unittest.main()
