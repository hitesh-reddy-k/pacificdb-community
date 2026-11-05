# Idle SDK connection safety

Scoped development evidence; published packages are unchanged. Both language clients replace observably expired idle connections before sending a new command, without replaying a sent write. Private TCP/TLS peer tests exercise EOF and queued idle-error frames, preserve healthy reuse, and verify exactly one transmitted write. A peer close racing a send remains an explicit unknown-outcome error.

Run `PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests`, `mvn -q -f sdk/java/pom.xml test`, and `node scripts/test-cross-sdk-e2e.mjs build-check` with the vector test feature flags from the development guide. The native engine and SDK source revisions are separate receipts; source fingerprints identify the tested SDK change.

The first latest1M runner omitted its original administrative-session reconnect and failed at post-load validation after a passing timed phase. That failure and its raw authority snapshot remain retained; the corrected full scenario is separate evidence.
