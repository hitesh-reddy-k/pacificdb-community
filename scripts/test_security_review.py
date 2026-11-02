import hashlib
import json
import re
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from scripts import security_review


class SourceBundleTests(unittest.TestCase):
    def make_repository(self, base):
        repo = base / "repo"
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)

        def git(*args):
            return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()

        git("config", "user.name", "Bundle Test")
        git("config", "user.email", "bundle-test@example.invalid")
        workflow = repo / ".github/workflows/fixture.yml"
        workflow.parent.mkdir(parents=True)
        for version in ("1.0.0", "2.0.0"):
            (repo / "package-lock.json").write_text(json.dumps({
                "packages": {"node_modules/example": {"version": version}}}))
            workflow.write_text("steps:\n  - uses: example/action@v" + version[0] + "\n")
            git("add", ".")
            git("commit", "-qm", "Dependencies " + version)
            if version == "1.0.0":
                previous = git("rev-parse", "HEAD")
        return repo, previous

    def test_historical_bundle_uses_archived_dependencies_and_excludes_unrelated_dist(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo, previous = self.make_repository(root)
            (repo / "dist").mkdir()
            (repo / "dist/stale-engine").write_bytes(b"unrelated executable")
            # Neither current tracked dependencies nor dirty worktree changes
            # may become evidence for the requested earlier revision.
            (repo / "package-lock.json").write_text('{"packages":{}}')
            manifest = security_review.build_bundle(repo, root / "bundle", previous)
            self.assertEqual(previous, manifest["revision"])
            sbom = json.loads((root / "bundle/sbom.cdx.json").read_text())
            self.assertEqual({("example", "1.0.0"), ("example/action", "v1")},
                             {(c["name"], c["version"]) for c in sbom["components"]})
            with tarfile.open(root / "bundle/source.tar") as archive:
                lock = archive.extractfile("package-lock.json").read()
            item = next(i for i in manifest["inputs"] if i["path"] == "package-lock.json")
            self.assertEqual(hashlib.sha256(lock).hexdigest(), item["sha256"])
            self.assertFalse((root / "bundle/artifacts").exists())
            self.assertEqual({"source.tar", "sbom.cdx.json"}, {a["path"] for a in manifest["artifacts"]})
            self.assertIn("executable artifacts are not included", manifest["scope"])

    def test_existing_evidence_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo, revision = self.make_repository(root)
            output = root / "bundle"
            output.mkdir()
            original = output / "source.tar"
            original.write_bytes(b"preserved previous evidence")
            with self.assertRaisesRegex(ValueError, "must be empty"):
                security_review.build_bundle(repo, output, revision)
            self.assertEqual(b"preserved previous evidence", original.read_bytes())

    def test_dependency_symlink_is_rejected_without_reading_its_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "unsafe.tar"
            with tarfile.open(archive, "w") as output:
                link = tarfile.TarInfo("package-lock.json")
                link.type = tarfile.SYMTYPE
                link.linkname = "/outside-review-snapshot"
                output.addfile(link)
            with self.assertRaisesRegex(ValueError, "unsafe dependency archive member"):
                security_review.archived_dependency_inventory(archive)


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
            "github/codeql-action/init@v4",
            "github/codeql-action/analyze@v4",
        ):
            self.assertIn(value, workflow)

    def test_dependabot_covers_all_repository_package_ecosystems(self):
        config = (self.root / ".github/dependabot.yml").read_text(encoding="utf-8")
        for ecosystem in ("npm", "maven", "pip", "github-actions", "docker"):
            self.assertIn(f'package-ecosystem: "{ecosystem}"', config)

    def test_codeql_covers_the_active_release_branch(self):
        workflow = (self.root / ".github/workflows/codeql.yml").read_text(encoding="utf-8")
        for event in ("pull_request", "push"):
            self.assertRegex(workflow, rf"(?m)^  {event}:\n    branches: \[main, v1\.2\]$")
        self.assertIn("  workflow_dispatch:", workflow)

    def test_ci_runs_dependency_review_with_read_only_permissions(self):
        workflow = (self.root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("dependency-review:", workflow)
        self.assertIn("actions/dependency-review-action@v5", workflow)
        dependency_job = workflow.split("  dependency-review:", 1)[1].split("  source-linux:", 1)[0]
        self.assertRegex(dependency_job, r"permissions:\n\s+contents: read\n")
        self.assertNotIn("contents: write", dependency_job)
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
