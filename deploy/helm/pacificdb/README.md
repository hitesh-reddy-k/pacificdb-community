# PacificDB Helm deployment

The default values are for isolated development only. They use a convenience
image tag and do not enable authentication or TLS.

Production rendering is fail-closed. Supply `values-production.yaml` together
with a private operator values file containing:

- the immutable `sha256:` GHCR image digest;
- encrypted data and backup StorageClass names and requested sizes;
- a client TLS Secret with `tls.crt`, `tls.key`, `ca.crt`, `health.crt`, and
  `health.key`;
- a Raft TLS Secret with `tls.crt`, `tls.key`, and `ca.crt`;
- a bootstrap Secret with `username` and `password` keys; and
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
