# Security

Do not open a public issue for a suspected vulnerability. Until a dedicated
security address is published, use GitHub's private vulnerability reporting for
the repository.

Never commit credentials, private keys, certificates, tokens, production
addresses, data directories, or customer data. Production startup requires
TLS/mTLS, authentication, RBAC, audit logging, durable WAL settings, RF3 quorum,
and reviewed encrypted-storage evidence.
