import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts import power_loss_harness as harness


class PowerLossValidationTests(unittest.TestCase):
    def complete_evidence(self):
        return {
            "schema_version": 1,
            "run_id": "power-test-001",
            "revision": "0123456789abcdef0123456789abcdef01234567",
            "dedicated_root": "/mnt/disposable-pacificdb-power-test",
            "hardware": {
                "host": "power-host-01",
                "filesystem": "ext4",
                "device": "/dev/nvme1n1p1",
                "controller": "NVMe controller 01",
                "drive_model": "TestDrive 1TB",
                "firmware": "1.2.3",
                "cache_policy": "volatile_write_cache_disabled",
                "cut_method": "managed_pdu",
            },
            "operator": "external-operator",
            "phase": "wal_sync",
            "iterations_required": 2,
            "boot_id_before": "boot-before",
            "iterations": [
                {
                    "number": 1,
                    "ledger_sha256": "a" * 64,
                    "marker_timestamp": "2026-09-19T10:00:00Z",
                    "power_restored_timestamp": "2026-09-19T10:01:00Z",
                    "boot_id_after": "boot-after-1",
                    "recovery_result": "PASS",
                    "acknowledged_digest": "b" * 64,
                    "recovered_digest": "b" * 64,
                },
                {
                    "number": 2,
                    "ledger_sha256": "c" * 64,
                    "marker_timestamp": "2026-09-19T10:02:00Z",
                    "power_restored_timestamp": "2026-09-19T10:03:00Z",
                    "boot_id_after": "boot-after-2",
                    "recovery_result": "PASS",
                    "acknowledged_digest": "d" * 64,
                    "recovered_digest": "d" * 64,
                },
            ],
        }

    def assert_blocked(self, evidence, reason):
        result = harness.validate_evidence(evidence)
        self.assertEqual("BLOCKED", result["status"])
        self.assertIn(reason, result["reasons"])

    def test_complete_physical_evidence_passes(self):
        result = harness.validate_evidence(self.complete_evidence())
        self.assertEqual({"status": "PASS", "reasons": []}, result)

    def test_missing_hardware_identity_is_blocked(self):
        evidence = self.complete_evidence()
        evidence["hardware"]["drive_model"] = ""
        self.assert_blocked(evidence, "missing_hardware:drive_model")

    def test_unknown_cache_mode_is_blocked(self):
        evidence = self.complete_evidence()
        evidence["hardware"]["cache_policy"] = "unknown"
        self.assert_blocked(evidence, "unknown_cache_policy")

    def test_absent_cut_timestamp_is_blocked(self):
        evidence = self.complete_evidence()
        evidence["iterations"][0]["marker_timestamp"] = ""
        self.assert_blocked(evidence, "iteration_1:missing_marker_timestamp")

    def test_fewer_iterations_than_required_is_blocked(self):
        evidence = self.complete_evidence()
        evidence["iterations"] = evidence["iterations"][:1]
        self.assert_blocked(evidence, "insufficient_iterations:1/2")

    def test_digest_mismatch_fails(self):
        evidence = self.complete_evidence()
        evidence["iterations"][1]["recovered_digest"] = "e" * 64
        result = harness.validate_evidence(evidence)
        self.assertEqual("FAIL", result["status"])
        self.assertIn("iteration_2:digest_mismatch", result["reasons"])

    def test_vm_or_process_only_cut_is_blocked(self):
        for method in ("vm_reset", "sigkill"):
            with self.subTest(method=method):
                evidence = self.complete_evidence()
                evidence["hardware"]["cut_method"] = method
                self.assert_blocked(evidence, "non_physical_cut_method")

    def test_unchanged_boot_id_is_blocked(self):
        evidence = self.complete_evidence()
        evidence["iterations"][0]["boot_id_after"] = "boot-before"
        self.assert_blocked(evidence, "iteration_1:boot_id_unchanged")

    def test_repository_related_root_is_rejected(self):
        repository = Path(__file__).resolve().parent.parent
        with self.assertRaisesRegex(ValueError, "repository"):
            harness.validate_disposable_root(repository / "power-test", repository)

    def test_empty_absolute_temporary_root_is_accepted(self):
        with tempfile.TemporaryDirectory() as parent:
            root = Path(parent) / "power-test"
            validated = harness.validate_disposable_root(
                root, Path(__file__).resolve().parent.parent
            )
            self.assertEqual(root.resolve(), validated)

    def test_external_probe_accepts_only_validated_physical_evidence(self):
        repository = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            evidence_path = Path(directory) / "physical.json"
            evidence_path.write_text(
                json.dumps(self.complete_evidence()), encoding="utf-8"
            )
            environment = os.environ.copy()
            environment["PACIFICDB_PHYSICAL_POWER_EVIDENCE"] = str(evidence_path)
            completed = subprocess.run(
                ["bash", "scripts/probe-external-certification.sh"],
                cwd=repository,
                env=environment,
                check=True,
                capture_output=True,
                text=True,
            )
            result = json.loads(completed.stdout)
            self.assertEqual(
                "PASS", result["physical_power_storage_controller"]["status"]
            )


if __name__ == "__main__":
    unittest.main()
