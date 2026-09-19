import json
import tempfile
import unittest
from pathlib import Path

from scripts import validate_operations


class OperationsContractTests(unittest.TestCase):
    def test_numeric_guarantee_without_evidence_is_rejected(self):
        contract = {
            "claims": [
                {
                    "id": "maximum_database_size",
                    "classification": "guarantee",
                    "value": "10 TB",
                }
            ]
        }
        errors = validate_operations.validate_claims(contract, Path("."))
        self.assertIn("maximum_database_size:numeric_claim_missing_evidence", errors)

    def test_numeric_observation_with_matching_json_pointer_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "qualification.json").write_text(
                json.dumps({"observations": {"restore_rto_seconds": 42}}),
                encoding="utf-8",
            )
            contract = {
                "claims": [
                    {
                        "id": "restore_rto",
                        "classification": "observed",
                        "value": 42,
                        "evidence": "qualification.json",
                        "json_pointer": "/observations/restore_rto_seconds",
                    }
                ]
            }
            self.assertEqual(
                [], validate_operations.validate_claims(contract, root)
            )

    def test_numeric_observation_must_equal_referenced_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "qualification.json").write_text(
                json.dumps({"observations": {"restore_rto_seconds": 41}}),
                encoding="utf-8",
            )
            contract = {
                "claims": [
                    {
                        "id": "restore_rto",
                        "classification": "observed",
                        "value": 42,
                        "evidence": "qualification.json",
                        "json_pointer": "/observations/restore_rto_seconds",
                    }
                ]
            }
            errors = validate_operations.validate_claims(contract, root)
            self.assertIn("restore_rto:evidence_value_mismatch", errors)

    def test_alert_runbook_link_must_resolve_to_a_real_heading(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runbooks = root / "docs" / "runbooks"
            runbooks.mkdir(parents=True)
            (runbooks / "WAL.md").write_text("# WAL\n\n## High latency\n", encoding="utf-8")
            alerts = """
groups:
  - name: test
    rules:
      - alert: PacificDBWalLatencyHigh
        annotations:
          runbook_url: docs/runbooks/WAL.md#missing-heading
"""
            errors = validate_operations.validate_alerts(
                alerts, root, required_alerts={"PacificDBWalLatencyHigh"}
            )
            self.assertIn(
                "PacificDBWalLatencyHigh:runbook_anchor_missing:missing-heading",
                errors,
            )


if __name__ == "__main__":
    unittest.main()
