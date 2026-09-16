# Raw Kubernetes example

`engine-statefulset.yaml` is a development-only example. It deliberately uses
the mutable `beta` convenience tag and does not satisfy PacificDB's production
TLS, authentication, encrypted-storage evidence, immutable-image, or backup
isolation requirements.

For production, use the Helm chart with `values-production.yaml`. Operators
must provide an immutable image digest, client/health and Raft TLS Secrets, a
bootstrap credential Secret, reviewed encrypted-volume evidence, and explicit
data and backup StorageClasses. Do not promote the raw example into production.
