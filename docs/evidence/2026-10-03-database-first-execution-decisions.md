# Database-first execution decisions — 3 October 2026

These are the recorded implementation decisions for the approved isolated program.
They do not authorize publication or waive release gates. Focused regressions and
the final qualification record cover the affected boundaries.

- **2026-10-03-database-first-program**: Ruling: execute at /home/hitesh/pacificdb-database-first-20261003 — backup device entered emergency_ro and reads returned EIO; normal clone failed, verified working files/Git objects/log recovery preserved identical ed53315 — cost if wrong: IDE must open new location; earlier local parents remain unavailable in this shallow recovery, original files not overwritten.

- **2026-10-03-database-first-workflow**: Ruling: verified recovered workspace used after source filesystem failure; no product file lost or changed by recovery.

- **2026-10-03-database-first-workflow**: Ruling: replace CLI tests requiring project commands with direct/context failure tests — those friendly commands are intentionally removed by the approved spec; legacy SDK tests remain — cost if wrong: users must use retained SDK/raw project APIs.

- **2026-10-03-database-first-workflow**: Ruling: URL lines and malformed JSON diagnostics are excluded/redacted in shell history/output; existing credential-bearing history lines are filtered when displayed — protects secrets without echoing invalid input.

- **2026-10-03-database-first-workflow**: Ruling: enable assertions in Release native parser tests — baseline CMake omitted UNDEBUG, so initial green executed no assertions; true baseline failed after enabling them and the corrected parser passes — cost if wrong: none to product, stricter verification.

- **2026-10-03-database-first-workflow**: Ruling: native --no-start bypasses the local discovery probe just as npm does; normal command transport validates its response — avoids an unauthenticated extra command before URL authentication; managed startup checks still pass.

- **2026-10-03-database-first-workflow**: Ruling: preserve supplied IPv6 spelling and reject non-ASCII DNS host text in shared URL grammar; add shared fixtures for ambiguous/invalid hosts — ensures language parsers agree without introducing an IDNA library.

- **2026-10-03-database-first-workflow**: Ruling: context writes use unique private directories and atomic replacement; generic JSON errors and value-only error redaction preserve successful response keys even for a one-character password — covered by both CLI integrations.

- **2026-10-03-database-first-workflow**: Ruling: retain only vetted optional desktop cliPath/platform in public connection metadata alongside the five required fields; bundled CLI examples already depend on them. Password, token, URL and all arbitrary fields are dropped.

- **2026-10-03-database-first-workflow**: Ruling: raw http.request verifies hostile Host because fetch normalizes it; streamed-download closure is observed with a bounded wait because response bytes can arrive before pipeline releases its file handle.

- **2026-10-03-database-first-workflow**: Ruling: previously used superpowers script paths are no longer installed in the current plugin cache; approved plan and native execution remain in effect, with task tracking recorded directly in this ledger.

- **2026-10-03-database-first-workflow**: Ruling: include the optional secret-free selected database in connection metadata and initialize navigation from it; otherwise --url would silently choose the first catalog database. Real-engine integration checks this selection.

- **2026-10-03-database-first-workflow**: Ruling: keep one four-worker summary loader active across navigation generations; waiting for obsolete in-flight jobs prevents a second refresh from doubling concurrency. This does not claim engine-side cancellation. Cache only current-database summaries; invalidate affected entries after writes and all entries on explicit overview refresh.

- **2026-10-03-database-first-workflow**: Ruling: verify documentation by executing its snippets and inspecting real request actions, rather than adding tests that merely grep prose; the old Node example failed the no-project action check before documentation was updated.

- **2026-10-03-database-first-workflow**: Ruling: retain old project SDK/engine contract tests; replace only obsolete friendly-project shell expectations. Authenticated Community e2e still covers six concurrent project creates and legacy catalog persistence, alongside the direct shell.

- **2026-10-03-java-python-sdk-capabilities**: Ruling: raw token/password fields are learned as private diagnostics inputs, without altering successful application responses. Raise sanitized Python errors outside the exception handler so __context__ cannot retain the original private engine message; the new chain regression failed before correction.

- **2026-10-03-java-python-sdk-capabilities**: Ruling: queued-scope test signals after serialization instead of relying on a sleep. Pool-wait timeout is tested with an independent longer active lease to avoid a race with the active request's own earlier deadline.

- **2026-10-03-java-python-sdk-capabilities**: Ruling: existing wrong-host test expects the approved typed tls_error with no private exception cause; verification still fails before protocol bytes. Stalled-write test sends prebuilt bytes directly through the transport so serialization time cannot consume the deadline before socket acceptance. Deadline cancellation is guarded before socket reuse. Java command nesting is capped at 256; OS blocking DNS remains outside socket interruption. No runtime dependency added.

- **2026-10-03-java-python-sdk-capabilities**: Task 3 in progress: exact common-operation wire contracts and database selection success/failure tests added in both languages. RED: Python 24 failed/6 passed on missing methods and old validation/selection; Java missing-method compilation errors. Beginner methods implemented without catalog requests. Ruling: existing fake Python peer incorrectly interpreted [] as an empty byte-fragment response; corrected helper to serialize empty JSON arrays. Tests also cover malformed list/row replies, engine failure response preservation and delayed drop preserving another selected database.

- **2026-10-03-java-python-sdk-capabilities**: Ruling: media raw get is named get_manifest/getManifest to distinguish the planned in-memory get; finalize_upload/finalizeUpload avoids Java Object.finalize return-type collision. Alias methods send canonical actions; raw requests can send verbatim aliases. Matrix source is actual main dispatch, not permission whitelist mentions without handlers. Wire samples corrected from source: B-tree fields object, top-level vector, digest fence/maxDocs, tenant mutation actor fields; optional restore target/index name/key role are not marked required. Advanced engine restrictions/maintenance scope are documented; no runtime or distributed certification implied.

- **2026-10-03-sdk-package-qualification**: Ruling: Central plugin 0.11.0 skip mode skipped staging in the actual installed plugin; retain safe default and pack cryptographically verified immutable artifacts locally instead — no endpoint contacted; cost if wrong: remote validation remains blocked, never implied by local ZIP.

- **2026-10-03-sdk-package-qualification**: Ruling: retain test logs and recovery evidence rather than deleting this ignored workspace — reproducibility requires them; cost if wrong: local disk use only.


- **2026-10-03-sdk-package-qualification**: Ruling: freeze qualified package and engine bytes at clean runtime commit e6781c9; final evidence-only commit must not replace them merely to change embedded metadata — keeps the exact installed/crash-tested bytes reviewable; cost if wrong: consumers must use the recorded qualification revision and future release-tag workflows rebuild/qualify their own exact revision. No release guard relaxed.
