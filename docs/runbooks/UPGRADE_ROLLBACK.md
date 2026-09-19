# Upgrade and rollback runbook

## Preconditions

Use only a path listed in `docs/COMPATIBILITY.md`. Hash old and candidate
artifacts, complete an isolated restore of a pre-upgrade backup, verify quorum
and replica integrity, and freeze schema/storage-format changes not in the plan.

## Rolling upgrade

Upgrade one follower, wait for health and catch-up, repeat for the other
follower, transfer leadership, then upgrade the final node. Verify acknowledged
IDs and canonical digests after every step and after a full restart.

## Interrupted upgrade

If interruption occurs before an irreversible format transition, restart the
same node with the intended compatible artifact and resume only after catch-up.
Do not skip ahead while membership or data convergence is uncertain.

## Rollback limits

Binary rollback is allowed only before a documented irreversible storage-format
transition. Afterwards, stop and restore the verified pre-upgrade backup into a
new isolated root. Never point an older binary at a newer-format data root.

## Evidence preservation

Retain artifact hashes, node order, terms/indexes, compatibility decisions,
backup/restore digests, interruption timeline, and every convergence result.
