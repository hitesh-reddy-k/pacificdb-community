# Node replacement runbook

## Preconditions

Confirm healthy quorum, identify the failed node unambiguously, preserve its
data root, verify a recent backup, and provision replacement storage and TLS
identity in the intended failure domain.

## Replacement

Join the replacement as a non-authoritative member, transfer snapshot/log state,
wait for applied-index catch-up, then compare canonical logical digests before
making it eligible for leadership or removing the old member.

## Replica integrity response

On `DIVERGENT`, stop promotion and mutation of the suspect replica. Preserve all
roots and compare at a common applied-index fence. On `LAGGING`, restore network
and capacity and wait for catch-up. Never auto-repair or copy files between live
members.

## Evidence preservation

Retain old/new node identities, membership history, snapshot/log ranges,
term/indexes, integrity results, storage diagnostics, and operator approvals.
