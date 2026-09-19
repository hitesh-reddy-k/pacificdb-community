import hashlib
import json
import re
import tempfile
import unittest
from pathlib import Path

from scripts import security_review


class SecurityReviewValidationTests(unittest.TestCase):
    def test_secret_scan_redacts_matched_value(self):
        secret = "AKIA" + "0123456789ABCDEF"
        findings = security_review.scan_text_for_secrets(
            "config.txt", f"token={secret}\n"
        )
        self.assertEqual(
            [{"path": "config.txt", "line": 1, "pattern": "aws_access_key"}],
            findings,
        )
        self.assertNotIn(secret, json.dumps(findings))

    def make_bundle(self):
        temporary = tempfile.TemporaryDirectory()
        bundle = Path(temporary.name)
        artifact = bundle / "artifacts" / "db_engine"
        artifact.parent.mkdir()
        artifact.write_bytes(b"release-engine")
        report = bundle / "review-report.pdf"
        report.write_bytes(b"independent report")
        revision = "0123456789abcdef0123456789abcdef01234567"
        manifest = {
            "schema_version": 1,
            "revision": revision,
            "artifacts": [
                {
                    "path": "artifacts/db_engine",
                    "sha256": hashlib.sha256(b"release-engine").hexdigest(),
                    "size_bytes": len(b"release-engine"),
                }
            ],
        }
        (bundle / "bundle-manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )
        review = {
            "schema_version": 1,
            "reviewer": {
                "name": "Alex External",
                "organization": "Independent Security Labs",
                "independent": True,
            },
            "review_date": "2026-09-19",
            "reviewed_revision": revision,
            "report": {
                "path": "review-report.pdf",
                "sha256": hashlib.sha256(b"independent report").hexdigest(),
            },
            "artifact_digests": {
                "artifacts/db_engine": hashlib.sha256(b"release-engine").hexdigest()
            },
            "findings": [
                {"id": "SEC-1", "severity": "high", "disposition": "resolved"},
                {"id": "SEC-2", "severity": "low", "disposition": "open"},
            ],
        }
        return temporary, bundle, review

    def assert_not_pass(self, bundle, review, reason):
        result = security_review.validate_review(bundle, review)
        self.assertNotEqual("PASS", result["status"])
        self.assertIn(reason, result["reasons"])

    def test_complete_external_review_passes(self):
        temporary, bundle, review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        self.assertEqual(
            {"status": "PASS", "reasons": []},
            security_review.validate_review(bundle, review),
        )

    def test_missing_reviewer_blocks_acceptance(self):
        temporary, bundle, review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        review["reviewer"]["name"] = ""
        self.assert_not_pass(bundle, review, "missing_reviewer_name")

    def test_mismatched_commit_fails_acceptance(self):
        temporary, bundle, review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        review["reviewed_revision"] = "f" * 40
        self.assert_not_pass(bundle, review, "reviewed_revision_mismatch")

    def test_mismatched_report_digest_fails_acceptance(self):
        temporary, bundle, review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        review["report"]["sha256"] = "0" * 64
        self.assert_not_pass(bundle, review, "report_digest_mismatch")

    def test_unresolved_critical_or_high_finding_blocks_acceptance(self):
        temporary, bundle, review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        review["findings"][0]["disposition"] = "open"
        self.assert_not_pass(bundle, review, "unresolved_finding:SEC-1")

    def test_missing_artifact_digest_blocks_acceptance(self):
        temporary, bundle, review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        review["artifact_digests"] = {}
        self.assert_not_pass(bundle, review, "missing_artifact_digest:artifacts/db_engine")

    def test_self_review_marker_blocks_acceptance(self):
        temporary, bundle, review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        review["reviewer"]["independent"] = False
        self.assert_not_pass(bundle, review, "reviewer_not_independent")

    def test_missing_review_result_is_blocked(self):
        temporary, bundle, _review = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        result = security_review.validate_review(bundle, None)
        self.assertEqual("BLOCKED", result["status"])
        self.assertIn("external_review_result_missing", result["reasons"])


class SecurityWorkflowContractTests(unittest.TestCase):
    root = Path(__file__).resolve().parent.parent

    def test_codeql_covers_cpp_and_javascript_on_pr_push_and_schedule(self):
        workflow = (self.root / ".github/workflows/codeql.yml").read_text(
            encoding="utf-8"
        )
        for value in (
            "pull_request:",
            "push:",
            "schedule:",
            "security-events: write",
            "cpp",
            "javascript-typescript",
            "github/codeql-action/init@v3",
            "github/codeql-action/analyze@v3",
        ):
            self.assertIn(value, workflow)

    def test_dependabot_covers_all_repository_package_ecosystems(self):
        config = (self.root / ".github/dependabot.yml").read_text(encoding="utf-8")
        for ecosystem in ("npm", "maven", "pip", "github-actions", "docker"):
            self.assertIn(f'package-ecosystem: "{ecosystem}"', config)

    def test_ci_runs_dependency_review_with_read_only_permissions(self):
        workflow = (self.root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("dependency-review:", workflow)
        self.assertIn("actions/dependency-review-action@v4", workflow)
        self.assertNotIn("secrets: inherit", workflow)

    def test_all_new_workflow_actions_use_reviewed_major_or_commit_ref(self):
        for path in (self.root / ".github/workflows").glob("*.yml"):
            for line in path.read_text(encoding="utf-8").splitlines():
                match = re.search(r"\buses:\s+[^@\s]+@([^\s]+)", line)
                if not match:
                    continue
                ref = match.group(1)
                self.assertRegex(
                    ref,
                    r"^(?:v\d+(?:\.\d+)*|[0-9a-f]{40})$|^\./",
                    f"{path}: unreviewed action ref {ref}",
                )


if __name__ == "__main__":
    unittest.main()
