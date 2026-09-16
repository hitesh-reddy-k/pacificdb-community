#!/usr/bin/env bash
set -euo pipefail
repo=${GITHUB_REPOSITORY:-hitesh-reddy-k/pacificdb-community}

gh api "repos/$repo/branches/main/protection" >/dev/null
test "$(gh api "repos/$repo/rulesets" --jq 'length')" -gt 0
gh api "repos/$repo/code-scanning/analyses?per_page=1" --jq 'length > 0' | grep -qx true
gh api "repos/$repo" --jq '.security_and_analysis.secret_scanning.status' | grep -qx enabled
gh api "repos/$repo" --jq '.security_and_analysis.secret_scanning_push_protection.status' | grep -qx enabled
gh api "repos/$repo/dependabot/alerts?per_page=1" >/dev/null
printf 'REPOSITORY_CONTROLS_PASS\n'
