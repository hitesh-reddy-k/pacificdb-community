#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR="${1:-build}"
cmake -S engine -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"${BUILD_JOBS:-2}"
for test in \
  v11_4_apply_exact_boundary_failpoints_test storage_path_security_test \
  db_engine_vector_correctness_test db_engine_lsm_compaction_test \
  db_engine_strong_read_memtable_test db_engine_comprehensive_test \
  db_engine_timeout_test db_engine_query_limit_test db_engine_memory_test \
  db_engine_snapshot_e2e db_engine_snapshot_bundle_test \
  db_engine_wal_group_commit_test db_engine_wal_segment_recovery_test \
  db_engine_lsm_manifest_test db_engine_lsm_checkpoint_test \
  db_engine_lsm_bounded_recovery_test db_engine_index_bounded_recovery_test \
  db_engine_raft_binary_log_test db_engine_raft_health_recovery_test \
  db_engine_raft_snapshot_metadata_test db_engine_storage_format_v2_test \
  db_engine_storage_root_migration_test db_engine_partitioned_queue_test \
  db_engine_work_stealing_test db_engine_community_manual_operations_test; do
  "$BUILD_DIR/$test"
done
node intelligence/test.js
npm --prefix sdk/node test
npm --prefix cli test
PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests
mvn -q -f sdk/java/pom.xml test
benchmarks/ycsb/test_binding.sh
legacy_product=basta
legacy_product+=base
paid_tier=enter
paid_tier+=prise
if rg -n -i "$legacy_product|$paid_tier" . --glob '!.git/**' --glob '!**/target/**'; then
  echo 'excluded branding found' >&2; exit 1
fi
for forbidden in auto_scaler geo_replication gossip_protocol cluster_manager \
  "shard_manager_$paid_tier"; do
  if find engine -iname "*$forbidden*" | grep -q .; then
    echo "excluded source found: $forbidden" >&2; exit 1
  fi
done
printf 'PacificDB Community checks passed\n'
