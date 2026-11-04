# Unreleased ANN optimization: matched 1M trial

Read REPORT.txt for actual outcomes and limitations. This is a self-run Linux
x64, standalone GloVe25/cosine/top10 trial. Fixed PostgreSQL, Qdrant, PacificDB
order; one full build per system; three query rounds reuse the held-out queries.
Four server CPUs, two client CPUs, sampled 6 GiB PSS guard, native durable ACKs,
common M16/efConstruction200, disjoint tuning/held-out recall gates.
All three databases run under the same temporary performance power-profile hold;
the controller restores the original power-saver selection after the run.
Connections are local loopback without TLS or remote authentication (PostgreSQL
trust, PacificDB authentication disabled, Qdrant local HTTP). PacificDB audit
logging and adaptive admission are disabled for this fixed-client trial, as in
the historical baseline. WAL/fsync and query/index budgets remain enabled.
Separate authenticated TCP/TLS SDK checks establish functionality, not the
performance of a secured deployment.

The source candidate is unreleased. Published RC.2 packages are unchanged. The
current runtime identity is in methodology.json and provenance/benchmark-exit;
prior-baseline provenance describes the older measurement, not this candidate.
This benchmark does not certify production or cluster readiness and does not imply a
win on other dimensions, hardware, protocols or workloads. Before/after timings
come from separate host runs; this host rebooted between attempts. Use the fresh
within-run comparison for the current database comparison.

Every measured query, tuning curve, load ACK, mixed operation, lifecycle and
resource sample is retained as deterministic gzip JSON/JSONL. Selected diagnostic
and qualification receipts disclose the initial slower backfilled-neighbor
variant and host interruption. Their timings are not substituted into the final
trial. Server and individual test logs remain private and are hashed below.
No credentials, customer data or private security reports are distributed.
The public input manifest and source/runtime/driver hashes support reproduction.
The source patch includes added files and applies to its recorded base commit.
Verify all files with SHA256SUMS. Checkpoint disk is reported separately and in
total storage. A stale derived checkpoint is never trusted as committed data.
The separate post-trial unchanged-source restart test has its own receipt,
resource samples, independent reference queries and source under qualification/.
Its timing is not part of the database comparison. Its source contains redacted
fixture paths; adapt those to a new disposable trial when reproducing it.

Supplemental unchanged-source 1M checkpoint-reuse status: **PASS**.

[Implementation, compatibility decisions and readiness gates](OPTIMIZATION.md).
