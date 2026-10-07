# PacificDB Helm deployment

The default values are for isolated development only. They pin the published
`1.0.0` image digest and require authentication. Create a `pacificdb-bootstrap`
Secret with `username` and `password` keys in the release namespace before
installing. The default profile does not enable TLS.

Production rendering is fail-closed. Supply `values-production.yaml` together
with a private operator values file containing:

- the immutable `sha256:` GHCR image digest;
- encrypted data and backup StorageClass names and requested sizes;
- a client TLS Secret with `tls.crt`, `tls.key`, `ca.crt`, `health.crt`, and
  `health.key`;
- a Raft TLS Secret with `tls.crt`, `tls.key`, and `ca.crt`;
- a bootstrap Secret with `username` and `password` keys;
- an audit-monitor Secret named by `security.auditMonitorSecret`, with a
  metrics-only API key in `<pod-name>.token` for each replica; and
- a ConfigMap whose `evidence.json` is a named review of encryption for the
  exact `/var/lib/pacificdb/data` mount.

Example validation (the checked-in fixture contains names and a dummy digest,
never credentials):

```sh
helm lint deploy/helm/pacificdb \
  -f deploy/helm/pacificdb/values-production.yaml \
  -f scripts/fixtures/helm-production-values.yaml
helm template pacificdb deploy/helm/pacificdb --namespace pacificdb \
  -f deploy/helm/pacificdb/values-production.yaml \
  -f /path/to/private-values.yaml
```

Label authorized client Pods with `pacificdb.io/client: "true"`. The production
NetworkPolicy permits client mTLS only from those Pods and Raft TLS only among
members of the release.

Production probes target the actual pod IP, verify the TLS hostname/CA and use
the dedicated client certificate. Readiness additionally requires audit logging
enabled and healthy using the pod's metrics-only token. Startup/liveness only
check responsiveness, so an audit sink outage removes readiness rather than
creating a restart loop. Probe replies have a 64 KiB ceiling and four-second
deadline; Kubernetes allows five seconds for each probe process.

API-key security stores are node-local: do not assume a key issued by one pod
works on all replicas. For initial provisioning, create the named Secret with
per-pod placeholder token files first. Pods can start but remain unready. Use
the already enabled headless service's `publishNotReadyAddresses` or a private
pod-IP connection with verified mTLS to issue a `metrics` key on each pod;
replace each placeholder with that pod's key. Never disable readiness, auth,
TLS or audit logging to bootstrap. After projected Secret files update, all
three readiness checks must pass. Keep bootstrap/password and monitor tokens
in private Secret manifests, never source control or command-line arguments.

The production template already drops capabilities, prevents escalation,
uses a non-root UID, runtime seccomp, read-only root filesystem, explicit
resource limits, RF3 quorum, anti-affinity, encrypted-storage evidence and
allowlisted network traffic. Namespace Pod Security enforcement, CNI policy
support, actual encrypted volumes, certificates and registry digest provenance
still need operator verification on the real cluster. An external monitor must
have an explicitly allowed network path; do not widen ingress for public access.
See [audit monitoring](../../../docs/OPERATIONS.md#security-audit-monitor).
