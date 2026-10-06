import hashlib
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts import workbench_release_qualification as qualification


class WorkbenchReleaseQualificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        subprocess.run(["git", "-C", str(self.repo), "config", "user.email",
                        "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "config", "user.name",
                        "Qualification Test"], check=True)
        (self.repo / ".gitignore").write_text("dist/\nevidence/\n", encoding="utf-8")
        (self.repo / "tracked.txt").write_text("candidate\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(self.repo), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.repo), "commit", "-qm", "candidate"], check=True)
        self.revision = subprocess.run(
            ["git", "-C", str(self.repo), "rev-parse", "HEAD"], check=True,
            capture_output=True, text=True).stdout.strip()
        self.artifacts = []
        for name in ("linux.deb", "windows.exe", "mac-arm64.dmg", "mac-x64.dmg"):
            path = self.repo / "dist" / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes((name + " candidate").encode())
            self.artifacts.append(path)

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def platform_results(self):
        names = ("linux-x64", "windows-x64", "macos-arm64", "macos-x64")
        return [qualification.build_platform_result(
            self.repo, name, "1.1.2", artifact) for name, artifact in zip(names, self.artifacts)]

    def physical(self, revision=None):
        return {
            "schema_version": 1, "run_id": "power-1",
            "revision": revision or self.revision, "dedicated_root": "/power-test",
            "operator": "Operator", "phase": "wal_sync", "iterations_required": 1,
            "boot_id_before": "boot-before", "hardware": {
                "host": "host", "filesystem": "ext4", "device": "nvme0n1",
                "controller": "nvme", "drive_model": "model", "firmware": "1",
                "cache_policy": "write-through", "cut_method": "hardware_power_switch"},
            "iterations": [{"number": 1, "ledger_sha256": "a" * 64,
                "marker_timestamp": "2026-10-01T00:00:00Z",
                "power_restored_timestamp": "2026-10-01T00:01:00Z",
                "boot_id_after": "boot-after", "recovery_result": "PASS",
                "acknowledged_digest": "b" * 64, "recovered_digest": "b" * 64}],
        }

    def load(self, **updates):
        value = {"schema_version": 1, "revision": self.revision, "status": "PASS",
            "duration_seconds": 28800, "records": 500000, "operations": 1,
            "errors": 0, "peak_resident_bytes": 1}
        value.update(updates)
        return value

    def security(self):
        bundle = self.repo / "evidence" / "security"
        bundle.mkdir(parents=True)
        candidate = bundle / "candidate.bin"
        report = bundle / "report.txt"
        candidate.write_bytes(b"reviewed candidate")
        report.write_text("independent review", encoding="utf-8")
        candidate_digest = self.digest(candidate)
        report_digest = self.digest(report)
        manifest = {"revision": self.revision, "secret_scan": {"status": "PASS"},
            "artifacts": [{"path": "candidate.bin", "sha256": candidate_digest}]}
        (bundle / "bundle-manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        review = {"schema_version": 1,
            "reviewer": {"name": "Reviewer", "organization": "Independent", "independent": True},
            "review_date": "2026-10-06", "reviewed_revision": self.revision,
            "report": {"path": "report.txt", "sha256": report_digest},
            "artifact_digests": {"candidate.bin": candidate_digest}, "findings": []}
        return bundle, review

    def audit(self, total=0):
        return {"metadata": {"vulnerabilities": {"total": total}}}

    def aggregate(self, **updates):
        bundle = updates.pop("security_bundle", None)
        review = updates.pop("security_review", None)
        if bundle is None or review is None:
            bundle, review = self.security()
        values = dict(repo=self.repo, version="1.1.2", artifacts=self.artifacts,
            platform_results=self.platform_results(), physical_evidence=self.physical(),
            security_bundle=bundle, security_review=review,
            load_evidence=self.load(), audit_report=self.audit())
        values.update(updates)
        return qualification.aggregate_evidence(**values)

    def test_complete_exact_revision_evidence_passes(self):
        self.assertEqual("PASS", self.aggregate()["decision"])

    def test_dirty_revision_blocks(self):
        (self.repo / "tracked.txt").write_text("dirty\n", encoding="utf-8")
        self.assertEqual("BLOCKED", self.aggregate()["decision"])

    def test_missing_platform_blocks(self):
        self.assertEqual("BLOCKED", self.aggregate(
            platform_results=self.platform_results()[:-1])["decision"])

    def test_platform_artifact_digest_mismatch_fails(self):
        results = self.platform_results()
        results[0]["artifact"]["sha256"] = "0" * 64
        self.assertEqual("FAIL", self.aggregate(platform_results=results)["decision"])

    def test_physical_revision_mismatch_fails(self):
        self.assertEqual("FAIL", self.aggregate(
            physical_evidence=self.physical("f" * 40))["decision"])

    def test_missing_security_digest_blocks(self):
        bundle, review = self.security()
        review["artifact_digests"] = {}
        result = self.aggregate(security_bundle=bundle, security_review=review)
        self.assertEqual("BLOCKED", result["decision"])

    def test_open_high_security_finding_blocks(self):
        bundle, review = self.security()
        review["findings"] = [{"id": "SEC-1", "severity": "high", "disposition": "open"}]
        result = self.aggregate(security_bundle=bundle, security_review=review)
        self.assertEqual("BLOCKED", result["decision"])

    def test_load_errors_fail(self):
        self.assertEqual("FAIL", self.aggregate(
            load_evidence=self.load(errors=1))["decision"])

    def test_short_load_blocks(self):
        self.assertEqual("BLOCKED", self.aggregate(
            load_evidence=self.load(duration_seconds=28799))["decision"])

    def test_runtime_audit_advisory_fails(self):
        self.assertEqual("FAIL", self.aggregate(audit_report=self.audit(1))["decision"])


if __name__ == "__main__":
    unittest.main()
