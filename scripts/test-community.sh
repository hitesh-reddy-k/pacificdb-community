#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR="${1:-build}"
cmake -S engine -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DPACIFICDB_ENGINE_VERSION=1.0.1
cmake --build "$BUILD_DIR" -j"${BUILD_JOBS:-2}"
for test in \
  v11_4_apply_exact_boundary_failpoints_test storage_path_security_test \
  db_engine_vector_correctness_test db_engine_lsm_compaction_test \
  db_engine_strong_read_memtable_test db_engine_comprehensive_test \
  db_engine_timeout_test db_engine_query_limit_test db_engine_memory_test \
  db_engine_snapshot_e2e db_engine_snapshot_bundle_test \
  db_engine_wal_group_commit_test db_engine_wal_segment_recovery_test \
  db_engine_lsm_manifest_test db_engine_lsm_checkpoint_test \
  db_engine_replica_digest_test \
  db_engine_lsm_bounded_recovery_test db_engine_index_bounded_recovery_test \
  db_engine_raft_binary_log_test db_engine_raft_health_recovery_test \
  db_engine_raft_snapshot_metadata_test db_engine_storage_format_v2_test \
  db_engine_storage_root_migration_test db_engine_partitioned_queue_test \
  db_engine_work_stealing_test db_engine_community_manual_operations_test; do
  "$BUILD_DIR/$test"
done
"$BUILD_DIR/db_engine_community_catalog_test"
"$BUILD_DIR/db_engine_community_api_key_test"
"$BUILD_DIR/db_engine_community_query_test"
"$BUILD_DIR/db_engine_native_shell_parser_test"
"$BUILD_DIR/db_engine_socket_runtime_test"
for checkpoint_failpoint in \
  FP_LSM_CHECKPOINT_AFTER_SST_SYNC \
  FP_LSM_CHECKPOINT_AFTER_ARTIFACT_RENAME \
  FP_LSM_CHECKPOINT_AFTER_MANIFEST_SYNC \
  FP_LSM_CHECKPOINT_AFTER_MANIFEST_RENAME \
  FP_LSM_CHECKPOINT_BEFORE_WAL_RECLAIM \
  FP_LSM_CHECKPOINT_AFTER_WAL_RECLAIM; do
  "$BUILD_DIR/db_engine_lsm_checkpoint_crash_driver" --run-one "$checkpoint_failpoint"
done
test "$("$BUILD_DIR/pacificdb" --version)" = "PacificDB 1.0.1"
test "$("$BUILD_DIR/pacificdb" -V)" = "PacificDB 1.0.1"
node intelligence/test.js
python3 scripts/test-release-consistency.py
python3 scripts/test-workflow-contract.py
python3 scripts/test-deployment-contract.py
python3 scripts/test-native-signing-contract.py
test -s site/assets/pacificdb-logo-symbol.png
node scripts/test-site-docs.mjs
if command -v rg >/dev/null 2>&1; then
  ! rg -n 'pacificdb-local(?:\.cmd)?' README.md cli/README.md site/index.html
else
  ! grep -En 'pacificdb-local(\.cmd)?' README.md cli/README.md site/index.html
fi
npm install --ignore-scripts --no-audit --no-fund
npm run test:npm
scripts/test-community-autostart.sh "$BUILD_DIR"
node scripts/test-community-e2e.mjs "$BUILD_DIR"
node scripts/test-p0-protocol-errors.mjs "$BUILD_DIR"
node scripts/test-community-contract-matrix.mjs "$BUILD_DIR"
node --test scripts/test-replica-integrity-monitor.mjs
node scripts/test-replica-integrity-rf3.mjs "$BUILD_DIR"
node scripts/test-community-restart-matrix.mjs "$BUILD_DIR"
scripts/test-community-disk-full.sh "$BUILD_DIR"
node scripts/test-community-rf3.mjs "$BUILD_DIR"
node scripts/test-community-rf3-partition.mjs "$BUILD_DIR"
node --test scripts/test-rf3-upgrade-contract.mjs
if test -n "${PACIFICDB_OLD_BUILD:-}"; then
  node scripts/test-community-rf3-upgrade.mjs \
    --old-build "$PACIFICDB_OLD_BUILD" --candidate-build "$BUILD_DIR" \
    --evidence "${PACIFICDB_UPGRADE_EVIDENCE:-/tmp/pacificdb-rf3-upgrade-evidence.json}"
else
  echo 'Mixed-version RF3 test skipped: set PACIFICDB_OLD_BUILD to an exact prior artifact' >&2
fi
PACIFICDB_RF3_DURATION_SECONDS="${PACIFICDB_RF3_SMOKE_SECONDS:-10}" \
PACIFICDB_RF3_REPEATS=1 PACIFICDB_RF3_CLIENTS=64,128 \
  node scripts/test-community-rf3-sustained.mjs "$BUILD_DIR"
PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests
mvn -q -f sdk/java/pom.xml test
legacy_product=basta
legacy_product+=base
paid_tier=enter
paid_tier+=prise
if command -v rg >/dev/null 2>&1; then
  branding_match=$(rg -n -i "$legacy_product|$paid_tier" . --glob '!.git/**' \
    --glob '!**/target/**' --glob '!docs/superpowers/**' || true)
else
  branding_match=$(grep -RInI -E "$legacy_product|$paid_tier" . \
    --exclude-dir=.git --exclude-dir='build*' --exclude-dir=node_modules \
    --exclude-dir=target --exclude-dir=superpowers || true)
fi
if test -n "$branding_match"; then
  printf '%s\n' "$branding_match"
  echo 'excluded branding found' >&2; exit 1
fi
for forbidden in auto_scaler geo_replication gossip_protocol cluster_manager \
  "shard_manager_$paid_tier"; do
  if find engine -iname "*$forbidden*" | grep -q .; then
    echo "excluded source found: $forbidden" >&2; exit 1
  fi
done
printf 'PacificDB Community checks passed\n'
