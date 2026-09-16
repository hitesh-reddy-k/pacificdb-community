#!/usr/bin/env python3
import importlib.util
import json
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
    "p0-evidence-linux-amd64-container.json",
]


class VerifyReleaseArtifactsTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)

    def tearDown(self):
        self.tempdir.cleanup()

    def populate(self, names):
        for name in names:
            if name == "p0-evidence-linux-amd64-container.json":
                content = json.dumps({
                    "status": "PASS",
                    "version": "1.0.0",
                    "revision": "a" * 40,
                    "registry_digest": "sha256:" + "b" * 64,
                    "image": "ghcr.io/hitesh-reddy-k/pacificdb-community@sha256:" + "b" * 64,
                })
            else:
                content = name
            (self.root / name).write_text(content, encoding="utf-8")

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
        manifest = verifier.build_manifest(self.root, "1.0.0", "a" * 40)
        self.assertEqual(manifest["version"], "1.0.0")
        self.assertEqual(
            [item["name"] for item in manifest["artifacts"]],
            sorted(INSTALLERS + EVIDENCE),
        )
        self.assertTrue(all(len(item["sha256"]) == 64 for item in manifest["artifacts"]))

    def test_container_evidence_must_match_release_identity(self):
        self.populate(INSTALLERS + EVIDENCE)
        evidence = self.root / "p0-evidence-linux-amd64-container.json"
        evidence.write_text(json.dumps({
            "status": "PASS",
            "version": "0.9.0",
            "revision": "c" * 40,
            "registry_digest": "not-a-digest",
        }), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "container evidence"):
            verifier.build_manifest(self.root, "1.0.0", "a" * 40)


if __name__ == "__main__":
    unittest.main()
