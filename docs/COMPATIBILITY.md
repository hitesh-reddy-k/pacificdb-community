# PacificDB upgrade compatibility

PacificDB supports rolling upgrades only when the exact source and destination
artifacts appear in this document and the release evidence contains their
SHA-256 hashes. Matching version strings are not sufficient.

## Supported rolling path

| From | To | Topology | Rollback boundary |
|---|---|---|---|
| `0.1.0-beta.14` | `1.0.0` built from the qualified revision | RF3, one node at a time | Binary rollback is permitted only before any declared storage-format transition. |

Upgrade followers first, waiting for replication convergence after each node.
Transfer or re-elect leadership before upgrading the final node. Preserve and
verify a pre-upgrade backup in an isolated restore root.

## Unsupported paths

- Arbitrary upgrades from older, unqualified releases are unsupported.
- Arbitrary downgrades are unsupported.
- An older binary must never be started on a data root after an irreversible
  storage-format transition. Restore the verified pre-upgrade backup into a new
  root instead.
- A mixed-version cluster must not remain in that state after a failed or
  interrupted upgrade. Resume the documented path or follow the rollback
  decision in the captured evidence.

An unsupported protocol or storage version must fail closed. Missing old
artifacts block rollback; they do not justify an untested downgrade.
