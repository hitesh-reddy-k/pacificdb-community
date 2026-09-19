# Physical power-loss qualification

This gate requires a dedicated, disposable test host and data device. A
`SIGKILL`, container stop, VM reset, normal shutdown, or reboot is useful
supplemental recovery evidence but does not satisfy the physical gate.

The harness never removes power and never invokes shutdown, reboot, IPMI, PDU,
or filesystem-destruction commands. The operator controls power only after the
harness prints an exact `POWER_CUT_NOW` marker.

## Safety prerequisites

Before preparing a run, record and independently verify all of the following:

- the host and data device are disposable and contain no customer or unrelated
  data;
- all unrelated data has a tested backup;
- the PacificDB source tree and OS boot device are not on the target data root;
- filesystem, block device, controller, drive model, firmware, and volatile
  write-cache policy are known;
- the cut mechanism removes physical host/storage power without a graceful
  guest shutdown; and
- the operator accepts possible filesystem, device, and OS damage.

Do not use the current development machine or a root under this repository.
The harness rejects `/`, the home directory, relative paths, repository-related
paths, symlinks, and pre-existing non-empty roots.

## Prepare

Run on the dedicated host, substituting its real inventory:

```sh
python3 scripts/power_loss_harness.py prepare \
  --root /mnt/disposable-pacificdb-power \
  --phase wal_sync \
  --revision "$(git rev-parse HEAD)" \
  --iterations 3 \
  --operator "operator-name" \
  --filesystem ext4 \
  --device /dev/nvme1n1p1 \
  --controller "controller identity" \
  --drive-model "drive identity" \
  --firmware "firmware revision" \
  --cache-policy volatile_write_cache_disabled \
  --cut-method managed_pdu
```

Preparation creates an ownership marker, immutable run identity, an
acknowledged-operation ledger, and `evidence.json`. Missing inventory remains
`BLOCKED`.

## Arm and cut

The workload must append and synchronize each acknowledged operation ID to the
path in `PACIFICDB_POWER_ACK_LEDGER`. Arm it without a shell wrapper:

```sh
python3 scripts/power_loss_harness.py arm \
  --root /mnt/disposable-pacificdb-power \
  --command /absolute/path/to/power-phase-workload --phase wal_sync
```

Do not remove power until the command prints:

```text
POWER_CUT_NOW run_id=... phase=... iteration=...
```

The marker means only that harness metadata is synchronized and the workload
is running. It does not claim database data is durable.

## Verify after reboot

The storage adapter performs read-only recovery and writes a small JSON result:

```json
{
  "power_restored_timestamp": "2026-09-19T10:01:00Z",
  "recovery_result": "PASS",
  "acknowledged_digest": "<64 lowercase hex characters>",
  "recovered_digest": "<64 lowercase hex characters>"
}
```

Then run:

```sh
python3 scripts/power_loss_harness.py verify \
  --root /mnt/disposable-pacificdb-power \
  --recovery-result /path/on/boot-device/recovery-result.json
```

Verification refuses an unchanged boot ID. Repeat prepare/arm/cut/verify for
every configured iteration and each phase. Validate the final record with:

```sh
python3 scripts/power_loss_harness.py validate \
  --evidence /mnt/disposable-pacificdb-power/evidence.json
```

Only complete physical evidence whose recovered digest equals the acknowledged
ledger digest returns `PASS`; missing facts return `BLOCKED`, and recovery or
digest errors return `FAIL`.
