# Database-first Packages Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Deliver direct database workflows and simple Java/Python clients with verified local distribution artifacts.

**Architecture:** Use existing engine actions; simplify client/UI context without migrating stored catalog data. Execute three independently reviewable plans in order: workflow/connection contract, SDK capabilities, then package qualification. Each implementation task has its own commit and fresh checks; publication remains a separate final action.

**Tech Stack:** Existing JavaScript/Node, C++/OpenSSL, Python standard library, Java/Jackson, Maven, setuptools, Playwright and Electron.

**Spec:** [Approved design](../specs/2026-10-03-database-first-cli-workbench-design.md). The user's 3 October instruction to implement the active written spec approves that spec. Native execution is retained from the earlier execution preference; this program and its component plans still require written-plan review before product edits.

## Global Constraints

- “No databases, collections, documents, project records, settings or user data are deleted or moved by this change.”
- “No engine format, durable sync, Raft apply/acknowledgement order, RBAC or namespace-containment change is authorized here.”
- “Python uses only the standard library at runtime; Java retains Jackson as its only runtime dependency.”
- “Write requests are not silently retried.”
- “No package is published, no release tag is created, and no installer is deployed during implementation.”
- Work only in `.worktrees/database-first-20261003` on `ux/database-first-20261003`. Imported baseline is `b44284f`; implementation begins after `a025218`. Leave original checkout changes alone.

## Review Focus

- Credential-bearing URLs: reject malformed/conflicting inputs before networking; never put credentials in context, diagnostics or copied examples (workflow tasks 1–3; SDK tasks 1–2).
- Changing database while a request/transfer runs: its captured scope remains fixed (workflow tasks 1, 4–5; SDK tasks 1–5).
- Failed creation, selection, authentication or deletion: preserve valid prior selection, close failed authentication resources, never silently retry writes (workflow tasks 1–4; SDK tasks 1–4).
- Interrupted/corrupt large transfers: bounded buffers, resumable upload state, and unchanged destination on failed download/export (SDK task 5).
- A package built locally but unusable after installation: inspect artifacts and run installed-artifact integration separately from source tests (release tasks 1–3).

---

### Task 1: Direct workflow and shared connection contract

**Plan:** [Workflow implementation](2026-10-03-database-first-workflow.md).

**Interfaces:** Produces URL fixtures, direct Node API, both database-first shells, Workbench navigation and URL examples. SDK plan consumes the URL fixtures.

- [x] Complete workflow tasks 1–6, with their checks and commits.
- [x] Record request-count evidence and native/npm/browser/desktop results, including any unavailable platform checks.

### Task 2: Safe Java and Python capability parity

**Plan:** [SDK implementation](2026-10-03-java-python-sdk-capabilities.md).

**Interfaces:** Consumes the shared URL fixture. Produces `PacificDB` entry points, transport/error contracts, capability matrix, bounded transfer APIs and real-engine tests; release plan consumes these public APIs.

- [x] Complete SDK tasks 1–6, with their checks and commits.
- [x] Verify matrix coverage against actual server dispatch and real engine behavior, rather than claiming a named wrapper alone proves a capability works.

### Task 3: Package qualification and publication preparation

**Plan:** [Release implementation](2026-10-03-sdk-package-qualification.md).

**Interfaces:** Consumes completed workflow and SDK APIs. Produces Python wheel/sdist, Java JAR/source/Javadoc/Central bundle, installed-artifact evidence and manually gated workflows.

- [x] Complete release tasks 1–4, with their checks and commits.
- [x] Run one fresh whole-branch review under native execution; resolve findings and repeat only affected checks.
- [x] Produce an evidence report tied to exact commit/artifact hashes. Distinguish imported Workbench baseline from new changes, local package checks from external registry/platform checks, and client readiness from database-engine release certification.

## Execution and handoff

After written-plan approval, execute natively in this isolated branch. Do not reopen an already approved design decision or pause between routine tasks. If evidence requires changing a public contract or expanding beyond the spec, document that concrete change before dependent work. Prepare reviewable artifacts before asking for final publication approval. Registry ownership, release version, signing credentials and protected-environment configuration are external prerequisites, not locally passing tests.

## Planning evidence

- Python source baseline: `PYTHONPATH=sdk/python python3 -m pytest -q sdk/python/tests` — 1 passed.
- Java source baseline: `mvn -B -q -f sdk/java/pom.xml test` — 3 passed, 0 failed/skipped; JDK 17.0.20.1, Maven 3.8.7.
- These checks passed on the imported clean branch before implementation. They do not establish the additional capabilities requested here.

## Plan self-review

- Spec acceptance 1–2 maps to workflow tasks 1–3 and SDK tasks 1–3; acceptance 3–6 to workflow tasks 4–6; acceptance 7 to SDK tasks 1–6; acceptance 8 to release tasks 1–3; acceptance 9 to release task 4 and each component's qualification task.
- The URL fixture is produced once and consumed by all language parsers. Java's existing object-returning `request` remains source-compatible; new list operations use `requestValue`. Family accessor names and transfer/error fields are consistent between their producing and consuming tasks.
- Each Review Focus item has an owning test task. Local bundle validation does not depend on registry credentials or upload; remote validation and release-owner signatures remain separate.
- Headers, placeholders and local links were checked. The three component plans are independently testable and do not copy implementation bodies. Nothing authorizes engine format/auth/containment changes or publication during implementation.


## Executed qualification

All three component plans are complete. Filesystem recovery required execution in
`/home/hitesh/pacificdb-database-first-20261003`; the failed original checkout was
not changed. Clean runtime revision `e6781c9` passed the final Community gate,
installed SDK TCP/TLS/SIGKILL checks and packaged Linux desktop tests. Six confirmed
independent review findings were fixed with regression evidence. Local artifacts,
exact hashes and remaining external prerequisites are in the
[readiness report](../../evidence/2026-10-03-database-first-package-readiness.md).
No publication, tag, merge, push or deployment was performed.
