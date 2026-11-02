#!/usr/bin/env python3
"""Read effective GitHub rules; fail closed against the production policy."""
import json
import os
import subprocess
from urllib.parse import quote

REQUIRED_CHECKS = {
    'source-linux', 'packages / linux', 'packages / windows',
    'packages / macos (macos-15, arm64)',
    'packages / macos (macos-15-intel, x86_64)',
}


def assess(repository, rules):
    reasons = []
    security = repository.get('security_and_analysis') or {}
    for feature in ('secret_scanning', 'secret_scanning_push_protection'):
        if security.get(feature, {}).get('status') != 'enabled':
            reasons.append(feature + '_not_enabled')
    types = {rule.get('type') for rule in rules}
    for required in ('deletion', 'non_fast_forward'):
        if required not in types:
            reasons.append(required + '_protection_missing')
    reviews = [rule.get('parameters', {}) for rule in rules
               if rule.get('type') == 'pull_request']
    if not any(p.get('required_approving_review_count', 0) >= 1 and
               p.get('dismiss_stale_reviews_on_push') is True for p in reviews):
        reasons.append('independent_approval_and_stale_review_protection_missing')
    checks = set()
    for rule in rules:
        if rule.get('type') == 'required_status_checks':
            p = rule.get('parameters', {})
            if p.get('strict_required_status_checks_policy') is True:
                checks.update(c.get('context') for c in p.get('required_status_checks', []))
    reasons.extend('required_check_missing:' + c for c in sorted(REQUIRED_CHECKS - checks))
    if not any(t.get('tool') == 'CodeQL' and
               t.get('security_alerts_threshold') in ('high_or_higher', 'medium_or_higher', 'all') and
               t.get('alerts_threshold') in ('errors', 'errors_and_warnings', 'all')
               for rule in rules if rule.get('type') == 'code_scanning'
               for t in rule.get('parameters', {}).get('code_scanning_tools', [])):
        reasons.append('codeql_high_severity_gate_missing')
    return {'status': 'BLOCKED' if reasons else 'PASS', 'reasons': reasons,
            'default_branch': repository.get('default_branch'),
            'required_checks': sorted(REQUIRED_CHECKS),
            'scope': 'Effective default-branch rules; not artifact or production certification'}


def main():
    repo = os.environ.get('GITHUB_REPOSITORY', 'hitesh-reddy-k/pacificdb-community')

    def fetch(path):
        return json.loads(subprocess.check_output(['gh', 'api', path], text=True))

    try:
        repository = fetch('repos/' + repo)
        branch = repository.get('default_branch')
        if not isinstance(branch, str) or not branch:
            raise ValueError('default_branch_unavailable')
        rules = fetch('repos/' + repo + '/rules/branches/' + quote(branch, safe=''))
        result = assess(repository, rules)
        result['repository'] = repo
        # A successful empty response is acceptable; lack of access is not.
        fetch('repos/' + repo + '/dependabot/alerts?per_page=1')
        analyses = fetch('repos/' + repo + '/code-scanning/analyses?per_page=1')
        if not analyses:
            result['status'] = 'BLOCKED'
            result['reasons'].append('code_scanning_analysis_missing')
    except (subprocess.CalledProcessError, ValueError, TypeError, KeyError) as error:
        result = {'status': 'BLOCKED', 'reasons': ['repository_controls_could_not_be_verified'],
                  'error_type': type(error).__name__}
    print(json.dumps(result, indent=2))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
