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
            elif name == "p0-evidence-windows-x64.json":
                content = json.dumps({
                    "status": "PASS",
                    "version": "PacificDB 1.0.0",
                    "revision": "a" * 40,
                    "windows_signing": {
                        "status": "PASS",
                        "installer": "PASS",
                        "installed_pacificdb": "PASS",
                        "installed_db_engine": "PASS",
                    },
                })
            elif name.startswith("p0-evidence-macos-"):
                architecture = "arm64" if "arm64" in name else "x86_64"
                content = json.dumps({
                    "status": "PASS",
                    "version": "PacificDB 1.0.0",
                    "revision": "a" * 40,
                    "release_architecture": architecture,
                    "macos_signing": {
                        "status": "PASS",
                        "signature": "PASS",
                        "notarization": "PASS",
                        "stapling": "PASS",
                        "gatekeeper": "PASS",
                    },
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
        self.assertEqual(manifest["native_signing"]["windows"]["status"], "PASS")
        self.assertEqual(manifest["native_signing"]["macos-arm64"]["status"], "PASS")
        self.assertEqual(manifest["native_signing"]["macos-x86_64"]["status"], "PASS")

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

    def test_missing_windows_signing_evidence_fails(self):
        self.populate(INSTALLERS + EVIDENCE)
        path = self.root / "p0-evidence-windows-x64.json"
        evidence = json.loads(path.read_text(encoding="utf-8"))
        del evidence["windows_signing"]
        path.write_text(json.dumps(evidence), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Windows signing evidence"):
            verifier.build_manifest(self.root, "1.0.0", "a" * 40)

    def test_non_passing_macos_notarization_fails(self):
        self.populate(INSTALLERS + EVIDENCE)
        path = self.root / "p0-evidence-macos-arm64.json"
        evidence = json.loads(path.read_text(encoding="utf-8"))
        evidence["macos_signing"]["notarization"] = "NOT_APPLICABLE"
        path.write_text(json.dumps(evidence), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "macOS signing evidence"):
            verifier.build_manifest(self.root, "1.0.0", "a" * 40)

    def test_explicit_unsigned_release_accepts_consistent_evidence(self):
        self.populate(INSTALLERS + EVIDENCE)
        windows_path = self.root / "p0-evidence-windows-x64.json"
        windows = json.loads(windows_path.read_text(encoding="utf-8"))
        for field in windows["windows_signing"]:
            windows["windows_signing"][field] = "NOT_APPLICABLE"
        windows_path.write_text(json.dumps(windows), encoding="utf-8")
        for architecture in ("arm64", "x86_64"):
            path = self.root / f"p0-evidence-macos-{architecture}.json"
            evidence = json.loads(path.read_text(encoding="utf-8"))
            for field in evidence["macos_signing"]:
                evidence["macos_signing"][field] = "NOT_APPLICABLE"
            path.write_text(json.dumps(evidence), encoding="utf-8")

        manifest = verifier.build_manifest(
            self.root, "1.0.0", "a" * 40, allow_unsigned=True
        )
        self.assertEqual(
            manifest["native_signing"]["windows"]["status"],
            "NOT_APPLICABLE",
        )
        self.assertEqual(
            manifest["native_signing"]["macos-arm64"]["status"],
            "NOT_APPLICABLE",
        )

    def test_native_signing_revision_must_match_release(self):
        self.populate(INSTALLERS + EVIDENCE)
        path = self.root / "p0-evidence-windows-x64.json"
        evidence = json.loads(path.read_text(encoding="utf-8"))
        evidence["revision"] = "c" * 40
        path.write_text(json.dumps(evidence), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Windows signing evidence"):
            verifier.build_manifest(self.root, "1.0.0")


if __name__ == "__main__":
    unittest.main()
