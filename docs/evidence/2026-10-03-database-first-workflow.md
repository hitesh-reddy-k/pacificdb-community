# Database-first workflow evidence — 3 October 2026

Implementation is isolated on `ux/database-first-20261003` in
`/home/hitesh/pacificdb-database-first-20261003`. The original backup filesystem
became read-only with I/O errors; its tracked planning snapshot was recovered
and verified before implementation. The original checkout is untouched.

The workflow consists of Node direct database/URL APIs, npm and native CLI
context/URL changes, direct Workbench requests, and database-first UI. Existing
engine storage, authorization, namespace checks, Raft and sync behavior are
unchanged. Legacy SDK/engine project APIs and stored data remain available.

Fresh workflow checks:

- Node: 26 tests covering URL fixtures, direct/legacy behavior, authentication,
  error handling and captured transfer scope.
- CLI: 25 tests covering direct flow, old context, URL conflicts/credentials,
  Workbench protections, single-action CRUD, concurrent scopes and media access.
- Native Release parser and shared URL fixture targets, TCP/TLS/hostname/auth/
  timeout/context/CRUD/process-restart integration, and automatic startup tests.
- Real-engine Workbench API checks preserve mapped data and list unmapped data;
  ordinary find dispatches one engine request with no project/catalog probe.
- Browser checks cover CRUD, pagination, indexes, vectors, media/preview races,
  keyboard/mobile layout, unknown counts, delayed old database summaries,
  authenticated environment examples and at most four concurrent summary jobs.
- Linux unpacked desktop checks cover real window, sandbox/context isolation,
  bundled engine/CLI, CRUD/media, shutdown/restart persistence and preferences.
- Website quickstart/Node snippets are executed against a disposable engine;
  beginner API frames are checked for absence of project mapping requests.

Development evidence and screenshots are retained under
`.superpowers/sdd/2026-10-03-database-first-workflow/`. The Linux runtime is
Node 24.19.0 and OpenSSL 3.0.13. Windows/macOS runtime checks have not been run
here; native DNS uses the OS resolver, whose blocking resolution is not
interruptible by the socket deadline.

The Java/Python capability and artifact qualification plans remain separate
required work. This workflow evidence does not certify those packages, publish
any package, demonstrate production certification or promise zero defects.
