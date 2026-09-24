# Raw Kubernetes example

`engine-statefulset.yaml` is a development-only example pinned to the published
`1.0.0` container image digest. Before applying it, create a `pacificdb-bootstrap`
Secret in the `pacificdb` namespace with `username` and `password` keys. The
example requires authentication, but it does not satisfy PacificDB's production
TLS, encrypted-storage evidence, or backup isolation
requirements.

For production, use the Helm chart with `values-production.yaml`. Operators
must provide an immutable image digest, client/health and Raft TLS Secrets, a
bootstrap credential Secret, reviewed encrypted-volume evidence, and explicit
data and backup StorageClasses. Do not promote the raw example into production.
