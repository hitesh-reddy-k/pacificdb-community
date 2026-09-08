#!/usr/bin/env bash
set -euo pipefail
YCSB_HOME="${YCSB_HOME:?set YCSB_HOME to a YCSB checkout}"
PACIFICDB_URL="${PACIFICDB_URL:-127.0.0.1:9000}"
RECORDS="${RECORDS:-10000}"
THREADS="${THREADS:-16}"
OUT="${OUT:-benchmarks/results/$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT"
CP="$YCSB_HOME/core/target/core-0.17.0.jar:benchmarks/ycsb/pacificdb-binding/target/pacificdb-binding-0.1.0.jar"
COMMON=(-cp "$CP" site.ycsb.Client -db site.ycsb.db.PacificDBClient -p "pacificdb.url=$PACIFICDB_URL" -p "recordcount=$RECORDS" -threads "$THREADS" -s)
if [[ -n "${PACIFICDB_TOKEN_FILE:-}" ]]; then COMMON+=(-p "pacificdb.authTokenFile=$PACIFICDB_TOKEN_FILE"); fi
java "${COMMON[@]}" -P benchmarks/ycsb/workloads/workloada -load >"$OUT/load.txt" 2>&1
for workload in a b c d e f; do
  java "${COMMON[@]}" -P "benchmarks/ycsb/workloads/workload$workload" -t >"$OUT/workload$workload.txt" 2>&1
done
printf 'results=%s\n' "$OUT"
