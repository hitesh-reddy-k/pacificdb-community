#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d /tmp/pacificdb-helm-contract-XXXXXX)
cleanup() { rm -rf -- "$temporary"; }
trap cleanup EXIT
cd "$repository_root"

helm lint deploy/helm/pacificdb
helm lint deploy/helm/pacificdb \
  -f deploy/helm/pacificdb/values-production.yaml \
  -f scripts/fixtures/helm-production-values.yaml

if helm template pacificdb deploy/helm/pacificdb --namespace pacificdb \
  -f deploy/helm/pacificdb/values-production.yaml >"$temporary/invalid.yaml" 2>&1; then
  echo "production placeholders unexpectedly rendered" >&2
  exit 1
fi
grep -q 'production image.digest must contain exactly 64' "$temporary/invalid.yaml"
printf 'PRODUCTION_PLACEHOLDER_FAIL_CLOSED\n'

helm template pacificdb deploy/helm/pacificdb --namespace pacificdb \
  >"$temporary/development.yaml"
helm template pacificdb deploy/helm/pacificdb --namespace pacificdb \
  -f deploy/helm/pacificdb/values-production.yaml \
  -f scripts/fixtures/helm-production-values.yaml >"$temporary/production.yaml"

! grep -q 'value: 0.0.0.0' "$temporary/production.yaml"
grep -q 'fieldPath: status.podIP' "$temporary/production.yaml"
grep -q 'ghcr.io/hitesh-reddy-k/pacificdb-community@sha256:' "$temporary/production.yaml"
grep -q 'kind: PodDisruptionBudget' "$temporary/production.yaml"
grep -q 'kind: NetworkPolicy' "$temporary/production.yaml"

kubeconform -strict -summary -kubernetes-version 1.32.0 \
  "$temporary/development.yaml" "$temporary/production.yaml"
kubeconform -strict -summary -kubernetes-version 1.32.0 \
  deploy/kubernetes/namespace.yaml deploy/kubernetes/engine-statefulset.yaml
printf 'HELM_DEPLOYMENT_PASS\n'
