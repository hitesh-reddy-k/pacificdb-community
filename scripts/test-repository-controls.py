#!/usr/bin/env python3
"""Negative production-control checks against effective GitHub rules."""
import copy
from pathlib import Path
import tempfile
import unittest
from check_repository_controls import REQUIRED_CHECKS, assess, load_review_policy


class Controls(unittest.TestCase):
    def setUp(self):
        self.repo = {'default_branch': 'main', 'security_and_analysis': {
            'secret_scanning': {'status': 'enabled'},
            'secret_scanning_push_protection': {'status': 'enabled'}}}
        self.rules = [{'type': 'deletion'}, {'type': 'non_fast_forward'},
                      {'type': 'pull_request', 'parameters': {
                          'required_approving_review_count': 1, 'dismiss_stale_reviews_on_push': True,
                          'required_review_thread_resolution': True}},
                      {'type': 'required_status_checks', 'parameters': {
                          'strict_required_status_checks_policy': True,
                          'required_status_checks': [{'context': c} for c in REQUIRED_CHECKS]}},
                      {'type': 'code_scanning', 'parameters': {'code_scanning_tools': [{
                          'tool': 'CodeQL', 'security_alerts_threshold': 'high_or_higher',
                          'alerts_threshold': 'errors'}]}}]

    def test_complete_policy(self):
        self.assertEqual(assess(self.repo, self.rules)['status'], 'PASS')

    def test_ruleset_presence_alone_cannot_pass(self):
        for rules in ([], [{'type': 'required_status_checks', 'parameters': {
                'strict_required_status_checks_policy': True, 'required_status_checks': []}}]):
            self.assertEqual(assess(self.repo, rules)['status'], 'BLOCKED')

    def test_no_approval_cannot_pass(self):
        self.rules[2]['parameters']['required_approving_review_count'] = 0
        self.assertEqual(assess(self.repo, self.rules)['status'], 'BLOCKED')

    def test_explicit_sole_maintainer_policy(self):
        self.rules[2]['parameters']['required_approving_review_count'] = 0
        result = assess(self.repo, self.rules, review_policy='sole-maintainer')
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['review_policy'], 'sole-maintainer')

    def test_sole_maintainer_still_requires_pull_requests(self):
        rules = [r for r in self.rules if r['type'] != 'pull_request']
        self.assertEqual(assess(self.repo, rules, review_policy='sole-maintainer')['status'], 'BLOCKED')

    def test_sole_maintainer_still_requires_discussion_resolution(self):
        self.rules[2]['parameters']['required_approving_review_count'] = 0
        self.rules[2]['parameters']['required_review_thread_resolution'] = False
        self.assertEqual(assess(self.repo, self.rules, review_policy='sole-maintainer')['status'], 'BLOCKED')

    def test_sole_maintainer_still_requires_each_native_check(self):
        self.rules[2]['parameters']['required_approving_review_count'] = 0
        for check in REQUIRED_CHECKS:
            rules = copy.deepcopy(self.rules)
            rules[3]['parameters']['required_status_checks'] = [
                c for c in rules[3]['parameters']['required_status_checks'] if c['context'] != check]
            self.assertEqual(assess(self.repo, rules, review_policy='sole-maintainer')['status'], 'BLOCKED')

    def test_sole_maintainer_still_requires_security_controls(self):
        self.rules[2]['parameters']['required_approving_review_count'] = 0
        for kind in ('deletion', 'non_fast_forward', 'code_scanning'):
            rules = [r for r in self.rules if r['type'] != kind]
            self.assertEqual(assess(self.repo, rules, review_policy='sole-maintainer')['status'], 'BLOCKED')
        self.repo['security_and_analysis']['secret_scanning_push_protection']['status'] = 'disabled'
        self.assertEqual(assess(self.repo, self.rules, review_policy='sole-maintainer')['status'], 'BLOCKED')

    def test_unknown_review_policy_is_rejected(self):
        with self.assertRaises(ValueError):
            assess(self.repo, self.rules, review_policy='disable-all')

    def test_missing_policy_file_keeps_independent_review(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(load_review_policy(Path(root)), 'independent')

    def test_policy_file_requires_known_explicit_profile(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / '.github'
            directory.mkdir()
            file = directory / 'repository-controls.json'
            file.write_text('{"schema_version":1,"review_policy":"sole-maintainer"}')
            self.assertEqual(load_review_policy(Path(root)), 'sole-maintainer')
            for content in ['{}', '{"schema_version":1,"review_policy":"disable-all"}', '{"schema_version":2,"review_policy":"sole-maintainer"}']:
                file.write_text(content)
                with self.assertRaises(ValueError):
                    load_review_policy(Path(root))

    def test_stale_approval_cannot_pass(self):
        self.rules[2]['parameters']['dismiss_stale_reviews_on_push'] = False
        self.assertEqual(assess(self.repo, self.rules)['status'], 'BLOCKED')

    def test_each_native_platform_check_is_required(self):
        for check in REQUIRED_CHECKS:
            rules = copy.deepcopy(self.rules)
            rules[3]['parameters']['required_status_checks'] = [
                c for c in rules[3]['parameters']['required_status_checks'] if c['context'] != check]
            self.assertIn('required_check_missing:' + check, assess(self.repo, rules)['reasons'])

    def test_non_strict_checks_cannot_pass(self):
        self.rules[3]['parameters']['strict_required_status_checks_policy'] = False
        self.assertEqual(assess(self.repo, self.rules)['status'], 'BLOCKED')

    def test_missing_security_metadata_cannot_pass(self):
        self.repo.pop('security_and_analysis')
        self.assertEqual(assess(self.repo, self.rules)['status'], 'BLOCKED')

    def test_each_secret_protection_is_required(self):
        for key in self.repo['security_and_analysis']:
            repo = copy.deepcopy(self.repo)
            repo['security_and_analysis'][key]['status'] = 'disabled'
            self.assertEqual(assess(repo, self.rules)['status'], 'BLOCKED')

    def test_weaker_scan_gate_cannot_pass(self):
        self.rules[4]['parameters']['code_scanning_tools'][0]['security_alerts_threshold'] = 'critical'
        self.assertEqual(assess(self.repo, self.rules)['status'], 'BLOCKED')


if __name__ == '__main__':
    unittest.main()
