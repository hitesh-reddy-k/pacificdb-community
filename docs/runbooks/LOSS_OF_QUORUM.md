# Loss of quorum runbook

## Triage

Stop writes that cannot meet the configured acknowledgement policy. Record
member reachability, term, commit/applied indexes, leader observations, network
state, disk state, and clocks. Distinguish a partition from process/storage
failure before changing membership.

## Recovery

Restore connectivity or recover an existing voting member before considering
replacement. Never force two partitions to accept writes. After quorum returns,
verify one leader, monotonic term/indexes, acknowledged IDs, and replica logical
digests.

## Escalation

Escalate when two members disagree on committed history, no verified backup is
available, or any proposed action would discard an acknowledged write.

## Evidence preservation

Retain topology, timelines, peer logs, terms/indexes, packet/proxy state,
storage diagnostics, operation receipts, and post-recovery digest evidence.
