# Independent whole-branch review — 2026-10-03

**Verdict: changes required.** Six confirmed medium-severity (P2) correctness, compatibility, or diagnostic-privacy findings. No confirmed P0/P1 finding. Both specification and engineering-quality verdicts are **Fail** for the listed boundaries; passing local checks do not cover these failures.

## Reviewed target and method

- Worktree: `/home/hitesh/pacificdb-database-first-20261003`.
- Public comparison base: `6e65e550fae2709d893781a9f15ad5990d0af480`.
- Initial reviewed HEAD: `d046ca2`; final inspected HEAD also includes `84d3182c48e317b74a68f194bc86934806896528`, the concurrently added Node launcher startup-race fix.
- Included imported Workbench/desktop/package baseline recovered in `ed53315cacf6900137f9685ec8a26aac7c207a12`, all database-first and SDK changes, package verification/publishing workflows, retained legacy callers, and the approved program/spec/component plans. Did not inspect the failed `/mnt` checkout or unrelated engine performance prototypes.
- Read-only product review using Ponytail and code-verification guidance. No product/test/config edits, no subagents, no Maven rebuild, no full engine gate, and no registry/tag/merge operations. Only this report and a fresh ignored artifact-inspection report were written.
- Local reproduction environment: Linux, Node 24.19.0, Python 3.12.3, JDK 17.0.20.1; repository-installed Playwright Chromium. Java 11/Python 3.10, Windows/macOS, actual GitHub protected-environment enforcement, and remote registries were not executed by this reviewer.

## Findings

### F1 — Native raw-request failures can print supplied passwords and tokens

**P2 / medium, high confidence; directly reproduced.** Location: `engine/src/shell.cpp:73` (`redact`), `:85` (`redactError`), and `:221` (`sendAndPrint`). The new redaction path knows only the URL password and automatically acquired authentication token. It neither learns sensitive strings from a raw command nor removes sensitive fields from an error response. Raw JSON requests remain a supported compatibility API.

Trigger: run the built native CLI against a local fake TCP peer with `--no-start request` and a JSON command containing `password: "review-only-password"` and `token: "review-only-token"`. Return `{"error":"denied","message":<request password>,"token":<request token>}`. The CLI exits 1 and prints both test secrets unchanged:

```json
{"error":"denied","message":"review-only-password","token":"review-only-token"}
```

Consequence: error output captured by terminals/logging exposes request credentials. This violates the workflow diagnostic-privacy requirement and differs from Node/Python/Java's outbound-sensitive-field handling. The peer reproduction demonstrates the missing defensive boundary; it does not assert the current engine normally echoes passwords.

Smallest fix direction: capture raw-request password/token/authorization strings before sending and recursively sanitize sensitive error-response keys as well as known secret values, while preserving successful raw response contracts. Add a native fake-peer regression through the real CLI entry point.

### F2 — Missing Workbench selection silently opens an unrelated database

**P2 / medium, high confidence; directly reproduced.** Location: `cli/workbench/app.js:422–428`; collection analogue at `:434–436`.

Trigger: return `connection.info.database = "requested-missing"` and `databases.list = ["unrelated-db"]`. Loading the existing browser UI sets the visible selected database to `unrelated-db`, requests its collections and summaries, and leaves the notice empty. This same fallback runs after the selected database disappears or is deleted; the collection fallback likewise selects a different collection after the current one disappears.

Observed browser result:

```json
{"requested":"requested-missing","actual":"unrelated-db","notice":""}
```

Consequence: a requested or previously selected scope is replaced without a deliberate selection; subsequent actions target another database/collection. Approved design line 67 explicitly requires a clear empty state on deletion/missing selections. The new URL-derived database initialization makes this particularly visible for a Workbench launched with a missing URL database.

Smallest fix direction: distinguish an initial unselected workspace from a previously selected/configured scope that disappeared. Clear missing selections and show the missing/empty state instead of assigning array element zero; preserve valid selections. Cover explicit missing URL scope and deletion while another database/collection exists.

### F3 — Media upload cannot address valid non-Latin namespaces

**P2 / medium, high confidence; directly reproduced.** Location: `cli/workbench/app.js:597–600`; receiving contract at `cli/src/workbench.js:252–253`. Imported Workbench behavior remains present on the reviewed branch.

Trigger: select engine-valid database `数据库` and collection `文件`, choose a nonempty `example.txt`, and click Upload in the actual browser UI. Database/collection names are inserted verbatim into HTTP headers. Fetch rejects characters outside the ISO-8859-1 byte range before any upload reaches the adapter:

```text
Failed to execute 'fetch' on 'Window': Failed to read the 'headers' property from 'RequestInit': String contains non ISO-8859-1 code point.
```

The engine's storage-name validation permits these UTF-8 identifiers; regular JSON operations work with them. Filename is already percent-encoded, but namespace headers are not.

Smallest fix direction: use matching strict percent encoding/decoding for database and collection headers, or carry namespace metadata in a Unicode-safe request representation. Validate decoded values and captured scope on the adapter. Add a browser upload using non-Latin database and collection names.

### F4 — Workbench rejects existing direct database names above 128 bytes

**P2 / medium, high confidence; directly reproduced at adapter boundary, engine limit traced in source.** Location: `cli/src/workbench.js:75–77`, default limit at `:19`; related upload validation at `:252`. The adapter retains the old 128-byte project-mapping-era limit for all databases.

The actual engine accepts database identifiers up to 255 bytes (`engine/include/storage_path.hpp:19`) and the new direct Node `createDatabase` intentionally applies 128 bytes only to explicit project mapping (`sdk/node/src/index.js:448–458`). A 129-byte direct database is therefore valid and appears in Workbench `databases.list`, but `collections.list` returns HTTP 400:

```json
{"error":"database must be 1-128 bytes"}
```

The same validator prevents deleting or using this existing database through the adapter; upload has its own default-128 check. This breaks the requirement to support accessible ordinary databases created through direct APIs.

Smallest fix direction: validate direct Workbench database names against the engine's database limit in every adapter path and align creation guidance if creation should expose the full supported range. Keep the legacy project-mapping limit only where mapping actually occurs. Cover a database name between 129 and 255 UTF-8 bytes.

### F5 — Python connect timeout restarts for every resolved address

**P2 / medium, high confidence; deterministic reproduction.** Location: `sdk/python/pacificdb/transport.py:49`.

`socket.create_connection(address, _remaining(deadline))` receives one remaining timeout. Python's helper reuses that full timeout for each address returned by `getaddrinfo`, so a multi-address host can consume that budget repeatedly before control returns to the pool. The next `_remaining` call is too late to bound connection establishment. This is distinct from the explicitly documented OS DNS limitation: the reproduction returns the address list immediately.

Narrow reproduction patched `getaddrinfo` to return three addresses and `socket.socket` with a socket whose `connect` sleeps for its configured timeout then raises `TimeoutError`. `ConnectionPool(..., timeout=.06).request(b'{}\n')` attempted all three and returned `request_timeout` after **0.181 seconds**, approximately three times the configured budget. All delays were in connect, none in DNS.

Consequence: caller latency and occupied pool leases exceed the declared complete-request deadline on ordinary multi-address connection failures. The helper also hides the connecting socket from the pool until it succeeds, so this structure does not permit pool shutdown to interrupt that connect.

Smallest fix direction: resolve once, create/track each candidate socket explicitly, recompute remaining time before each connect, and close/discard failed candidates without retrying any sent write. Preserve the stated DNS caveat. Test multiple resolved addresses and close during connect with controlled sockets.

### F6 — Python accepts scalar protocol replies as successful writes

**P2 / medium, high confidence; directly reproduced.** Location: `sdk/python/pacificdb/transport.py:99–107`; resulting context mutation at `sdk/python/pacificdb/client.py:253–255`.

After JSON decoding, transport returns any JSON type. A local fake TCP peer returned the complete frame `null\n` to `create_database("unconfirmed")`; the public call returned `None` and changed `db.database` from `original` to `unconfirmed`. Numbers, booleans, and strings are likewise accepted. Java correctly rejects non-object/non-array protocol replies in `ConnectionPool.read`.

Consequence: malformed/incompatible peer replies can be reported as successful mutations and alter client selection without a valid engine response, contrary to the malformed-response and failure-safe-context contracts. This is a new context-changing consequence in the expanded Python SDK, even though the old raw Python decoder was also permissive.

Smallest fix direction: reject JSON values that are neither objects nor arrays at the shared Python framing boundary, discard the connection, and raise a typed `invalid_response` error. Add scalar-frame tests through a context-changing public method and assert the previous selection remains intact.

## Requirement/evidence summary

| Risk/requirement | Evidence reviewed or executed | Result |
| --- | --- | --- |
| Direct database flows and retained raw/legacy APIs | Traced Node, both shells, Workbench request adapter, engine action contracts, SDK entry points | Partial: F2/F4; primary positive checks supplied in existing workflow evidence |
| Credential-private diagnostics | Traced all four client/CLI sanitizers; executed native fake-peer failure | Fail: F1 |
| Bounded pooling, deadlines, close, scope snapshots, no silent write replay | Traced Python/Java pools, lazy auth and scope serializers; executed controlled Python connect/scalar probes | Fail: F5/F6; no sent-write replay found |
| Bounded verified file APIs and preserved destination | Traced Python/Java hashing, manifest scope/progress, strict base64, size/checksum, sibling temporary and atomic rename paths; reviewed file tests | Partial: prior integration/test evidence; no additional confirmed file-transfer defect |
| Workbench origin/session/media containment and stale navigation | Traced HTTP Host/Origin/token checks, per-request clients, manifest scope checks, generation guards; executed real browser UI with local fake adapter | Fail: F2/F3/F4; ordinary scope/authorization checks retained |
| Complete named Community action coverage | Fresh `python3 scripts/test-sdk-capability-matrix.py` | Pass for named bindings: 135 dispatches; not a claim that all advanced operations are semantically integration-tested |
| Archive contents/runtime metadata | Fresh package negative checks and inspection of seven existing distributions | Pass for inspected archive contracts; installed behavior relies on prior supplied logs |
| Publishing guards and exact artifact handoff | Traced reusable workflow permissions, job-scoped OIDC, protected environment names, tag/revision/hash verification, exact-byte signing and USER_MANAGED uploader; fresh workflow tests | Partial: local guards pass; actual environments/ownership/remote acceptance remain external |
| Added launcher startup-race fix | Reviewed `84d3182`, ran `node --test cli/test/local-engine.test.js` | Pass: 3 tests, including lock owner before metadata; parent owns real autostart/full gate |

## Fresh check results

All listed commands returned exit 0 unless a reproduction intentionally expected a product failure:

- `python3 scripts/test-sdk-capability-matrix.py`: 135 named dispatch bindings.
- `python3 scripts/test-sdk-packages.py`: 5 passed (intentional duplicate-entry negative fixture emitted a zipfile warning).
- `python3 scripts/test-sdk-workflows.py`: 3 passed.
- `node --test cli/test/local-engine.test.js`: 3 passed.
- `python3 scripts/verify-sdk-packages.py --python-dist sdk/python/dist --java-target sdk/java/target --output .superpowers/sdd/2026-10-03-sdk-package-qualification/final-review-artifacts.json`: inspected 7 artifacts. This is fresh archive/metadata/hash inspection, not a rebuild, installed-artifact test, or fresh cryptographic verification with the removed temporary private keyring.
- Six narrow reproductions described above: two actual Chromium UI runs, one direct Workbench HTTP call, one built-native-CLI fake-peer run, one Python deadline seam, one Python public-client fake-peer run. Reproduction values were invented test strings; no real secrets, real user data, or existing database were accessed.
- Final product status was clean at inspected `84d3182`; reviewer writes are ignored evidence only.

The root implementation agent is running broader final qualification. This review neither supersedes that gate nor claims remote registry acceptance, other-platform certification, engine production certification, or absence of all defects. Rebuild/requalify affected final package bytes after resolving findings, as required by the approved package plan.
