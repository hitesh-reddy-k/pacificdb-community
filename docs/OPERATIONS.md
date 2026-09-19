# Production operations contract

This document distinguishes what PacificDB guarantees, what a specific test
observed, what each operator must choose, and what remains blocked. It is not a
universal SLA or an unlimited-capacity claim.

<!-- operations-contract:start -->
{
  "schema_version": 1,
  "require_classifications": ["guarantee", "observed", "operator_target", "blocked"],
  "claims": [
    {
      "id": "acknowledged_write_contract",
      "classification": "guarantee",
      "value": "An acknowledgement means the configured durability and quorum policy completed; weaker durability modes must be disclosed to the caller."
    },
    {
      "id": "release_specific_recovery_time",
      "classification": "observed",
      "value": "Recovery and failover durations are published only in the exact-commit release evidence."
    },
    {
      "id": "deployment_rpo_rto",
      "classification": "operator_target",
      "value": "The owner selects RPO, RTO, backup interval, alert thresholds, and resource headroom for each deployment."
    },
    {
      "id": "universal_capacity_limit",
      "classification": "blocked",
      "value": "No universal maximum database size or throughput is certified because long-duration capacity testing is excluded."
    },
    {
      "id": "physical_power_durability",
      "classification": "blocked",
      "value": "Physical power-loss durability remains blocked until complete dedicated-host evidence passes validation."
    }
  ]
}
<!-- operations-contract:end -->

## RPO and durability

- **Guarantee:** an acknowledged write has completed the durability mode and,
  for synchronous RF3, the quorum policy configured for that request. Operators
  must not advertise an RF3 guarantee while running standalone or asynchronous
  replication.
- **Observed:** SIGKILL/restart, WAL recovery, disk-full, and RF3 results belong
  to one release revision. Consult its release-evidence JSON; do not carry a
  prior release result forward.
- **Operator target:** choose the acceptable loss window before setting WAL
  fsync, replication mode, and backup frequency. A target of no acknowledged
  write loss requires durable WAL and synchronous quorum acknowledgement.
- **Blocked:** physical host/storage power loss is not covered until the
  physical-power evidence gate passes on representative hardware.

## RTO and failover

- **Observed:** publish measured restart, leader-election, and restore duration
  as release-specific evidence, including data size and hardware.
- **Operator target:** set alert and recovery objectives from a restore drill on
  the actual topology. Keep the target above the worst verified observation
  plus operational margin.
- **Blocked:** PacificDB does not currently promise a universal recovery or
  failover SLA.

## Supported deployment envelope

- **Guarantee:** production configuration requires TLS/mTLS, authentication,
  RBAC, audit logging, durable WAL, RF3 quorum, and dedicated writable data,
  backup, restore, temporary, and log roots.
- **Observed:** certification applies only to platforms, packages, containers,
  Kubernetes versions, artifacts, concurrency, and data volume recorded in the
  candidate evidence.
- **Operator target:** use Linux filesystems and storage whose flush/cache
  behavior is documented; maintain failure-domain separation between replicas.
- **Blocked:** no maximum database size, tenant count, vector count, or sustained
  throughput is certified by this program.

## Resource sizing

Treat all formulas as operator targets and validate them under representative
traffic:

- memory budget = engine working set + vector/index residency + compaction
  overhead + connection concurrency + operating-system headroom;
- usable disk = live data + WAL retention + compaction temporary space + local
  backup staging + free-space reserve;
- file descriptors = listener/replica sockets + client connections + WAL/SST
  files + backup streams + safety reserve; and
- network capacity = client traffic + synchronous replication + snapshots and
  backups at the desired completion window.

Start conservatively, alert before admission control activates, and change only
one resource limit at a time. If memory backpressure, write stalls, compaction
backlog, or replica lag persists, stop increasing load.

## Backup frequency and restore drills

Backup interval must be no greater than the operator's accepted backup RPO.
Export backups outside the database failure domain, encrypt them, retain a
documented generation set, and verify hashes. Perform isolated restores on the
same release before relying on a backup and after every storage-format upgrade.
See [Backup and restore](runbooks/BACKUP_RESTORE.md).

## Alert response

The rules in `deploy/monitoring/pacificdb-alerts.yaml` are starting points. Tune
thresholds from exact-release observations and deployment objectives. An absent
metric is a monitoring failure, not proof of health. Preserve logs, metrics,
revision, configuration, recent changes, storage diagnostics, and the operation
ledger before restarting or repairing anything.

## Emergency recovery

Stop automated mutation, identify the last acknowledged operation boundary,
preserve the original data roots read-only, and work on copies. Do not delete a
WAL, SST, manifest, snapshot, or backup to make startup succeed. Follow the
relevant runbook and escalate if quorum, integrity, containment, or format
compatibility cannot be proved.

## Upgrade support

Only paths listed in `COMPATIBILITY.md` and proven by exact-artifact evidence are
supported. Upgrade followers before the leader, verify convergence after every
node, and keep a verified pre-upgrade backup. See
[Upgrade and rollback](runbooks/UPGRADE_ROLLBACK.md).
