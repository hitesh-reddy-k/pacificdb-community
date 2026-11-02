#!/usr/bin/env python3
"""Negative production-control checks against effective GitHub rules."""
import copy
import unittest
from check_repository_controls import REQUIRED_CHECKS, assess


class Controls(unittest.TestCase):
    def setUp(self):
        self.repo = {'default_branch': 'main', 'security_and_analysis': {
            'secret_scanning': {'status': 'enabled'},
            'secret_scanning_push_protection': {'status': 'enabled'}}}
        self.rules = [{'type': 'deletion'}, {'type': 'non_fast_forward'},
                      {'type': 'pull_request', 'parameters': {
                          'required_approving_review_count': 1, 'dismiss_stale_reviews_on_push': True}},
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
