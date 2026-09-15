#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    path = ROOT / relative
    if not path.is_file():
        raise AssertionError(f"missing deployment contract file: {relative}")
    return path.read_text(encoding="utf-8")


def require(relative: str, *needles: str) -> None:
    text = read(relative)
    for needle in needles:
        if needle not in text:
            raise AssertionError(f"{relative} must contain {needle!r}")


def forbid(relative: str, *needles: str) -> None:
    text = read(relative)
    for needle in needles:
        if needle in text:
            raise AssertionError(f"{relative} must not contain {needle!r}")


require(
    "deploy/helm/pacificdb/values.yaml",
    "production:",
    "enabled: false",
    "ghcr.io/hitesh-reddy-k/pacificdb-community",
)
require(
    "deploy/helm/pacificdb/values-production.yaml",
    "enabled: true",
    "replicaCount: 3",
    "digest: sha256:",
    "clientTlsSecret:",
    "raftTlsSecret:",
    "bootstrapSecret:",
    "encryptionEvidenceConfigMap:",
    "className:",
)
require(
    "scripts/fixtures/helm-production-values.yaml",
    "digest: sha256:",
    "clientTlsSecret: pacificdb-client-tls",
    "raftTlsSecret: pacificdb-raft-tls",
    "bootstrapSecret: pacificdb-bootstrap",
    "encryptionEvidenceConfigMap: pacificdb-at-rest-evidence",
)
require(
    "deploy/helm/pacificdb/templates/engine.yaml",
    'fail "production replicaCount must be exactly 3"',
    'hasPrefix "sha256:"',
    "PACIFICDB_ENVIRONMENT",
    "TLS_REQUIRE_CLIENT_CERT",
    "RAFT_TLS_ENABLED",
    "PACIFICDB_AT_REST_ENCRYPTION_EVIDENCE_PATH",
    "status.podIP",
    "readOnlyRootFilesystem: true",
    "startupProbe:",
    "readinessProbe:",
    "livenessProbe:",
    "/usr/local/bin/pacificdb-healthcheck",
    "name: data",
    "name: backup",
)
require(
    "deploy/helm/pacificdb/templates/poddisruptionbudget.yaml",
    "minAvailable: 2",
)
require(
    "deploy/helm/pacificdb/templates/networkpolicy.yaml",
    "kind: NetworkPolicy",
    "policyTypes:",
    "Ingress",
    "Egress",
)
require(
    "deploy/kubernetes/engine-statefulset.yaml",
    "pacificdb.io/profile: development",
    "ghcr.io/hitesh-reddy-k/pacificdb-community:beta",
)
forbid("deploy/kubernetes/engine-statefulset.yaml", "pacificdb/community:beta")
require(
    "deploy/kubernetes/README.md",
    "development-only",
    "values-production.yaml",
)

print("DEPLOYMENT_CONTRACT_PASS")
