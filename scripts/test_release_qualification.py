import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts import release_qualification as qualification


class ReleaseQualificationTests(unittest.TestCase):
    def make_repository(self):
        temp = tempfile.TemporaryDirectory()
        repo = Path(temp.name)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(
            ["git", "-C", str(repo), "config", "user.email", "test@example.invalid"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(repo), "config", "user.name", "Qualification Test"],
            check=True,
        )
        (repo / "tracked.txt").write_text("baseline\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(repo), "add", "tracked.txt"], check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-qm", "baseline"], check=True)
        return temp, repo

    def test_upgrade_requires_exact_old_artifact_and_passes_arguments(self):
        from unittest.mock import patch
        with patch.dict("os.environ", {}, clear=True):
            gate = next(g for g in qualification.default_gates() if g["name"] == "mixed_version_upgrade")
            self.assertEqual(gate["status"], "BLOCKED")
            self.assertNotIn("command", gate)
        with patch.dict("os.environ", {"PACIFICDB_OLD_BUILD": "/old/build", "PACIFICDB_CANDIDATE_BUILD": "build"}):
            gate = next(g for g in qualification.default_gates() if g["name"] == "mixed_version_upgrade")
            self.assertIn("--old-build", gate["command"])
            self.assertIn("/old/build", gate["command"])
            self.assertIn("--evidence", gate["command"])

    def test_dirty_tree_is_not_release_eligible(self):
        temp, repo = self.make_repository()
        self.addCleanup(temp.cleanup)
        (repo / "tracked.txt").write_text("changed\n", encoding="utf-8")

        evidence = qualification.collect_repository_state(repo)

        self.assertTrue(evidence["dirty"])
        self.assertFalse(evidence["release_eligible"])

    def test_clean_tree_is_bound_to_head_revision(self):
        temp, repo = self.make_repository()
        self.addCleanup(temp.cleanup)
        expected = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

        evidence = qualification.collect_repository_state(repo)

        self.assertEqual(expected, evidence["revision"])
        self.assertFalse(evidence["dirty"])
        self.assertTrue(evidence["release_eligible"])

    def test_windows_checkout_keeps_cli_entry_script_lf(self):
        temp, repo = self.make_repository()
        self.addCleanup(temp.cleanup)
        source = Path(__file__).resolve().parents[1]
        (repo / ".gitattributes").write_bytes((source / ".gitattributes").read_bytes())
        entry = repo / "cli/bin/pacificdb.js"
        entry.parent.mkdir(parents=True)
        expected = (source / "cli/bin/pacificdb.js").read_bytes()
        entry.write_bytes(expected)
        subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-qm", "CLI entry"], check=True)
        checkout_temp = tempfile.TemporaryDirectory()
        self.addCleanup(checkout_temp.cleanup)
        checkout = Path(checkout_temp.name) / "checkout"
        subprocess.run(["git", "-c", "core.autocrlf=true", "clone", "-q",
                        str(repo), str(checkout)], check=True)
        subprocess.run(["git", "-C", str(checkout), "config", "core.autocrlf", "true"],
                       check=True)
        self.assertEqual(expected, (checkout / "cli/bin/pacificdb.js").read_bytes(),
                         "npm must not need to rewrite the CLI shebang after checkout")
        self.assertFalse(qualification.collect_repository_state(checkout)["dirty"])

    def test_required_blocked_gate_prevents_stable_release(self):
        self.assertEqual(
            "BLOCKED",
            qualification.decide(
                [
                    {"name": "source", "status": "PASS", "required": True},
                    {"name": "physical_power", "status": "BLOCKED", "required": True},
                ],
                stable=True,
            ),
        )

    def test_excluded_soak_allows_only_controlled_candidate(self):
        self.assertEqual(
            "CONTROLLED_CANDIDATE",
            qualification.decide(
                [
                    {
                        "name": "long_duration_load",
                        "status": "EXCLUDED",
                        "required": False,
                    }
                ],
                stable=False,
            ),
        )

    def test_failure_always_fails_decision(self):
        self.assertEqual(
            "FAIL",
            qualification.decide(
                [{"name": "source", "status": "FAIL", "required": True}],
                stable=False,
            ),
        )

    def test_invalid_gate_status_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "invalid gate status"):
            qualification.decide(
                [{"name": "source", "status": "SKIPPED", "required": True}],
                stable=False,
            )

    def test_blocked_development_report_can_be_written_successfully(self):
        self.assertEqual(
            0,
            qualification.exit_code_for(
                "BLOCKED", allow_dirty_development=True, dirty=True
            ),
        )

    def test_blocked_release_qualification_fails_closed(self):
        self.assertNotEqual(
            0,
            qualification.exit_code_for(
                "BLOCKED", allow_dirty_development=False, dirty=False
            ),
        )

    def test_artifact_digest_uses_relative_path_and_sha256(self):
        temp, repo = self.make_repository()
        self.addCleanup(temp.cleanup)
        artifact = repo / "dist" / "candidate.bin"
        artifact.parent.mkdir()
        artifact.write_bytes(b"candidate bytes")

        record = qualification.collect_artifact(repo, artifact)

        self.assertEqual("dist/candidate.bin", record["path"])
        self.assertEqual(
            hashlib.sha256(b"candidate bytes").hexdigest(), record["sha256"]
        )
        self.assertEqual(len(b"candidate bytes"), record["size_bytes"])


if __name__ == "__main__":
    unittest.main()
