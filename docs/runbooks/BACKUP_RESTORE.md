# Backup and restore runbook

## Preconditions

Record revision, storage format, cluster identity, encryption/key ownership,
backup destination, retention policy, and operator-selected RPO/RTO. Confirm the
destination is outside the database failure domain.

## Backup procedure

Start a supported backup, retain its manifest and digests, export it, and verify
the exported bytes. A command returning success without a verified manifest is
not a completed backup.

## Restore drill

Restore into a new isolated root with networking disabled. Verify manifest,
logical counts/digests, authorization boundaries, and application-level sample
queries before recording the drill. Never restore over the only source copy.

## Alert response

For stale or failed backups, pause destructive maintenance, preserve the last
valid generation, diagnose capacity/permissions/encryption first, and rerun an
isolated restore before clearing the alert.

## Evidence preservation

Retain source revision, command timestamps, manifest and artifact digests,
storage diagnostics, logs, restore root identity, logical digest, and operator.
