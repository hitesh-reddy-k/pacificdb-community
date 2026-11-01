from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
class Workflow(unittest.TestCase):
 def test_rc_tags_cannot_enter_stable_publish_jobs(self):
  s=(ROOT/'.github/workflows/release.yml').read_text()
  self.assertIn("!contains(github.ref_name, '-')",s)
  self.assertIn("if: startsWith(github.ref, 'refs/tags/v') && !contains(github.ref_name, '-')",s)
 def test_prerelease_has_explicit_publication_authority(self):
  s=(ROOT/'.github/workflows/prerelease.yml').read_text()
  for text in ('workflow_dispatch:','workflow_call:', 'branches: [v1.2]', 'prerelease-publish','publish:', '--prerelease --latest=false','Pre-release v1.2','v1.2.0-rc.1','SHA256SUMS'):
   self.assertIn(text,s)
  self.assertNotIn('--prerelease=false',s)
  self.assertIn("github.event_name == 'workflow_dispatch' && inputs.publish",s)
if __name__=='__main__':unittest.main()
