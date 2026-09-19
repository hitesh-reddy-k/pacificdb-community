# Certificate rotation runbook

## Preconditions

Inventory server, client, and CA certificates; SANs; expiry; key locations; and
every trust store. Prepare overlap between old and new trust chains.

## Rotation procedure

Distribute trust first, rotate one replica at a time, verify authenticated
client and peer traffic after each node, then remove old trust only after every
consumer has migrated. Never log or copy private-key contents into evidence.

## Alert response

For expiry alerts, freeze unrelated configuration changes, confirm clock health,
rotate through the overlap procedure, and test both client and replica mTLS.

## Rollback

Restore the prior certificate reference while its chain is still trusted. If
the old certificate is expired or compromised, do not roll back; complete an
emergency forward rotation.

## Evidence preservation

Retain certificate fingerprints, issuer/subject/SAN metadata, validity dates,
deployment revision, node order, verification results, and operator identity.
