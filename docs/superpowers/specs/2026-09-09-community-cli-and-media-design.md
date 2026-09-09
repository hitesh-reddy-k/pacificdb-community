# PacificDB Community CLI and media design

## Goal

Provide one honest Community shell whose displayed commands execute against the
local PacificDB engine. It must cover authentication identity, local projects,
databases, document queries, manual backups, API keys, media files, vector
search, and shell utilities. Video uploads have no PacificDB total-size cap;
available disk space remains a physical limit.

The Community repository must not import organization billing, fleet
management, autoscaling, automatic sharding/rebalancing, geo replication,
enterprise identity providers, scheduled/PITR backup orchestration, KMS/HSM,
or other Enterprise/Cloud control-plane code.

## Considered approaches

1. Copy the private Enterprise shell and Node control plane. This has the most
   command names, but many are help-only, it brings paid features into the
   Community tree, and it requires an HTTP service that Community does not
   otherwise need. Rejected.
2. Add every requested concept as a new C++ subsystem. This keeps all behavior
   in the engine but duplicates existing CRUD and persistence machinery.
   Rejected.
3. Extend the existing direct-engine Community CLI and reuse normal replicated
   documents for project and chunk metadata. Add C++ actions only where the
   engine owns the invariant: aggregation/explain, API-key verification, and
   backup lifecycle. Selected.

## Public interface

`pacificdb shell` displays a compact PacificDB wave banner and categorized
help. It accepts friendly commands and retains raw JSON requests through
`request <json>` for compatibility.

### Authentication

- `login <username>` prompts for a password, calls `security_authenticate`, and
  keeps the returned token in the current shell context.
- `whoami` validates the current token and displays username and role.
- `logout` clears the token from the shell context.

Passwords and full API-key values must never enter command history or normal
diagnostic output.

### Projects

- `create project <name>`
- `list projects`
- `use project <id>`
- `show project`
- `delete project`

A Community project is a local organizational label, not a SaaS organization,
billing boundary, quota, or fleet-management object. Project records use the
existing replicated document path in a reserved `pacificdb_meta.projects`
collection. The selected project is client context and is attached as metadata
to subsequent database creation where supported.

### Databases and queries

- `create database <name>`, `list databases`, `use <name>`,
  `drop database <name>`, and `show database`.
- Mongo-style shell commands for `insert`, `find`, `findOne`, `update`,
  `delete`, `aggregate`, `count`, and `explain`.
- Existing raw JSON commands remain available.

`findOne` is `find` with a limit of one. Aggregation supports the already
documented bounded Community subset: `$match`, `$project`, `$sort`, `$skip`,
`$limit`, and `$count`. Unsupported stages return an explicit error. Explain
reports the actual engine plan and must not claim an index that the engine did
not select.

### Backups

- `create backup [--name <name>]`
- `list backups`
- `restore backup <id>`
- `list restores`
- `delete backup <id>`
- `backup verify <id>`
- `backup export <id> [file]`

Create, list, verify, delete, and restore call `BackupManager`. Restore outcomes
are persisted in a small engine-side journal so `list restores` reports real
attempts. Export writes the portable backup manifest to the requested local
file; it does not pretend to copy data from a remote engine. The CLI describes
this accurately.

### API keys

- `create api-key [--name <name>]`
- `list api-keys`
- `show api-key <id>`
- `revoke api-key <id>`

The engine generates `pdb_` keys, stores only SHA-256 hashes plus metadata, and
shows the secret once. Active keys authenticate through the same request
authorization boundary as session tokens. List and show never return the full
secret. Mutating key commands require an administrator identity.

### Media and vectors

- `upload image <path>`, `upload video <path>`, and `upload media <path>`
- `list media`, `find media <query>`, `show media <id>`, and
  `delete media <id>`
- `put vector <collection> <id> <json-vector>` and
  `query vector <collection> <json-vector> [--k <n>] [--metric <name>]`

The existing `put-media`, `get-media`, `put-vector`, and `query-vector`
non-interactive commands remain compatible.

Media upload reads a file sequentially in 4 MiB chunks. Every chunk is stored
in reserved collections inside the selected database as a normal replicated
document through the existing Raft write path, followed by a manifest containing
the user's logical collection, filename, content type, total bytes, chunk count,
and SHA-256 checksum. There is no total-size comparison or video-specific cap.
Each request stays below the engine request limit. Download reads chunks in
order and writes directly to the destination file. A manifest becomes `ready`
only after all chunks commit; interrupted uploads remain resumable and are not
shown as ready media.

The implementation must not read the entire media file into memory. Disk full,
quorum failure, checksum mismatch, missing chunks, invalid vectors, and unknown
metrics return explicit errors. Deleting media removes its manifest and known
chunks.

### System commands

- `help [topic]`, `context show`, `context clear`, `status`, `history`,
  `clear`, and `exit`.

Context and history live under the user's standard application-data directory
with owner-only permissions. `context clear` removes selected project/database
and credentials. Normal query output strips internal MVCC, Raft, tracing, and
tenant fields; raw administrative requests retain their diagnostic payload.

## Components

- `cli/src/cli.js`: command-line entry and compatibility commands.
- `cli/src/shell.js`: parser, categorized help, context, history, and command
  dispatch. Keep parsing explicit; do not add a command framework dependency.
- `sdk/node/src/index.js`: direct-engine methods and sequential file/chunk
  transfer.
- `engine/src/server.cpp`: narrowly scoped aggregate/explain, backup, identity,
  and API-key actions.
- Existing query, backup, security, and database-engine files hold their own
  persistence and validation logic.

## Compatibility and failure behavior

- Existing JSON-over-TCP, package names, ports, flags, and raw shell input
  continue to work.
- Chunk documents use reserved collections and deterministic IDs, so ordinary
  user-collection queries never expose them.
- Failed uploads never return success and never publish a ready manifest.
- A duplicate committed chunk with the same checksum is accepted for resume;
  a checksum mismatch fails.
- Authentication is optional only when the engine is configured for
  development without authentication, matching existing behavior.

## Verification

1. CLI unit tests cover categorized help, context, history, friendly commands,
   clean query output, media resume, and error messages.
2. Node client tests upload and download a file larger than two chunks without
   buffering it as one request, and verify the final checksum.
3. C++ focused tests cover bounded aggregation, honest explain, API-key secret
   handling/revocation, backup delete/journal/export, and media document
   visibility rules.
4. Build the engine, run retained Community tests, package the CLI, and run a
   local engine end-to-end session exercising every displayed command.

Publishing, npm release, GitHub push, and installer publication remain outside
this change until the user completes testing and explicitly requests them.
