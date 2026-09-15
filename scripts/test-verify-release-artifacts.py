#!/usr/bin/env python3
import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("verify-release-artifacts.py")
SPEC = importlib.util.spec_from_file_location("verify_release_artifacts", SCRIPT)
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)

INSTALLERS = [
    "pacificdb-community-1.0.0-linux-amd64.deb",
    "pacificdb-community-1.0.0-windows-x64.exe",
    "pacificdb-community-1.0.0-macos-arm64.pkg",
    "pacificdb-community-1.0.0-macos-x86_64.pkg",
]
EVIDENCE = [
    "p0-evidence-linux-amd64.json",
    "p0-evidence-windows-x64.json",
    "p0-evidence-macos-arm64.json",
    "p0-evidence-macos-x86_64.json",
]


class VerifyReleaseArtifactsTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)

    def tearDown(self):
        self.tempdir.cleanup()

    def populate(self, names):
        for name in names:
            (self.root / name).write_text(name, encoding="utf-8")

    def test_missing_required_artifact_fails(self):
        self.populate(INSTALLERS[:-1] + EVIDENCE)
        with self.assertRaisesRegex(ValueError, "missing required release artifact"):
            verifier.build_manifest(self.root, "1.0.0")

    def test_empty_required_artifact_fails(self):
        self.populate(INSTALLERS + EVIDENCE)
        (self.root / INSTALLERS[0]).write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty release artifact"):
            verifier.build_manifest(self.root, "1.0.0")

    def test_complete_artifacts_have_stable_hashes(self):
        self.populate(INSTALLERS + EVIDENCE)
        manifest = verifier.build_manifest(self.root, "1.0.0")
        self.assertEqual(manifest["version"], "1.0.0")
        self.assertEqual(
            [item["name"] for item in manifest["artifacts"]],
            sorted(INSTALLERS + EVIDENCE),
        )
        self.assertTrue(all(len(item["sha256"]) == 64 for item in manifest["artifacts"]))


if __name__ == "__main__":
    unittest.main()
