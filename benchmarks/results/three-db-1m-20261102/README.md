# Fresh 1M-vector three-database benchmark

Self-run standalone GloVe-25 cosine/top10 study. Read REPORT.txt and REPRODUCTION.md
before comparing values. Every system uses the same frozen input/query rows,
four server CPUs, native synchronous WAL acknowledgements, M16 and
efConstruction200, with independently calibrated efSearch.

This is a development candidate, not a new published RC.2 binary or production
certification. Fixed sequential order and active workstation load, native
protocol differences,25Dinputs and single full build per system limit conclusions.

Compressed JSON/JSONL retains every measured query, tuning curve, lifecycle,
resource sample and per-worker attempted operation. Local paths are redacted;
raw private originals remain retained. Public vector/metadata fixture data only.
Server logs are retained privately and hashed below, not distributed. No
credentials, private security-finding reports or actual customer data are included.
The interrupted original1M trial is INCOMPLETE; none of its timings were substituted
into this fresh run. Earlier smoke attempts and controls remain disclosed.

Use SHA256SUMS to verify this folder. Gzip streams have deterministic timestamps.
See provenance and inventory.json for actual runtime/script/input hashes. Sources
and raw inputs originate from the repository and the dataset URLs in the manifest.
