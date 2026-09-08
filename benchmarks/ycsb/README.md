# Reproducible YCSB benchmark

This directory contains the PacificDB YCSB binding and standard workloads A-F.
It publishes the workload, client code, and runner needed to reproduce results;
it contains no private data, credentials, machine addresses, or internal reports.

Set `YCSB_HOME`, `PACIFICDB_URL`, and optionally `PACIFICDB_TOKEN_FILE`, then run:

```sh
benchmarks/ycsb/test_binding.sh
benchmarks/ycsb/run_ycsb.sh
```

Results are written under `benchmarks/results/`, which is ignored by Git. Publish
machine specifications, configuration, raw output, errors, and repetition count
with every performance claim.
