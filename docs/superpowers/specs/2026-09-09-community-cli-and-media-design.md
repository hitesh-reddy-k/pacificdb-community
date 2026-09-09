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
- `delete project <id>`

A Community project is a local organizational label, not a SaaS organization,
billing boundary, quota, fleet-management object, or security boundary. Project
records use the existing replicated document path in the reserved
`pacificdb_meta.projects` collection. Database authorization remains the
security boundary.

Creating a database while a project is selected always records the relation in
`pacificdb_meta.database_projects` as `{project_id, database_name}`. The command
fails if either database creation or relation persistence fails; it never
silently omits the relation.

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
- `show backup <id>`
- `restore backup <id>`
- `list restores`
- `delete backup <id>`
- `backup verify <id>`
- `backup export <id> [file]`

Create, list, show, verify, delete, and restore call `BackupManager`. Restore is
synchronous and local. Outcomes are persisted in
`pacificdb_meta.restore_journal`, so `list restores` reports completed and
failed real attempts rather than queued workflow metadata. Export writes the
portable backup manifest to the requested local file; it does not pretend to
copy data from a remote engine. The CLI describes this accurately.

Community does not include scheduled backups, retention policies, approvals,
point-in-time restore, cloud-object-storage backup, continuous backup, or
cross-region backup orchestration.

### API keys

- `create api-key [--name <name>] [--role read|readwrite|admin]`
- `list api-keys`
- `show api-key <id>`
- `revoke api-key <id>`

The engine generates keys in the form `pdb_<key-id>_<secret>` and stores only
the key ID, name, role, SHA-256 secret hash, creator, creation time, last-use
time, and revocation time. Verification parses the ID, loads one record, hashes
the supplied high-entropy secret, compares it in constant time, and checks the
revocation state. Active keys authenticate through the same request
authorization boundary as session tokens. Roles are `read`, `readwrite`, and
`admin`; the normal engine permission checks enforce them. List and show never
return the full secret. Mutating key commands require an administrator
identity. Full keys must never enter restore journals, audit logs, history,
exceptions, or debug output.

### Media and vectors

- `upload image <path>`, `upload video <path>`, and `upload media <path>`
- `download media <id> [destination]`
- `list media`, `find media <query>`, `show media <id>`, and
  `delete media <id>`
- `list media --all` and `media cleanup`
- `put vector <collection> <id> <json-vector>` and
  `query vector <collection> <json-vector> [--k <n>] [--metric <name>]`

The existing `put-media`, `get-media`, `put-vector`, and `query-vector`
non-interactive commands remain compatible.

Media upload reads a file sequentially. The engine exposes its configured
request limit, and the client derives a source-chunk size whose Base64-encoded
JSON envelope remains below that limit, reserving 64 KiB for command metadata.
The source chunk is capped at 4 MiB and reduced when the configured request
limit requires it. Every chunk is stored in reserved collections inside the
selected database as a normal replicated document through the existing Raft
write path, followed by a manifest containing the user's logical collection,
filename, content type, total bytes, chunk size, chunk count, and SHA-256
checksum.

PacificDB Community imposes no application-level total media or video file-size
limit. Upload capacity is bounded by available storage, configured engine and
request limits, replication requirements, filesystem limits, and host
resources. Download reads chunks in order and writes directly to the
destination file. A manifest becomes `ready` only after all chunks commit.

The implementation must not read the entire media file into memory. Upload
returns a media ID before transferring chunks. `upload media <path> --resume
<media-id>` resumes an interrupted upload. Reusing a committed chunk ID with
the same checksum succeeds; a different checksum fails. `list media` shows only
ready objects, while `list media --all` includes uploading and failed objects.
`delete media` removes ready or incomplete uploads, and `media cleanup` removes
incomplete uploads selected by the user without adding a scheduler.

Disk full, quorum failure, checksum mismatch, missing chunks, an envelope that
cannot fit the configured request limit, invalid vectors, and unknown metrics
return explicit errors.

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
- `engine/src/shell.cpp`: dependency-free native installer CLI with the same
  command names and sequential chunk transfer behavior.
- `engine/src/server.cpp`: narrowly scoped aggregate/explain, backup, identity,
  API-key, project metadata, and media metadata/chunk actions.
- Existing query, backup, security, and database-engine files hold their own
  persistence and validation logic.

## Compatibility and failure behavior

- Existing JSON-over-TCP, package names, ports, flags, and raw shell input
  continue to work.
- `pacificdb_meta` and its collections are reserved by the C++ engine. Only
  authorized internal actions may access them; privileged raw administrative
  requests must opt in explicitly. Users cannot create or spoof reserved
  collections through ordinary database actions.
- Chunk documents use deterministic IDs, so retry and resume are idempotent.
- Failed uploads never return success and never publish a ready manifest.
- A duplicate committed chunk with the same checksum is accepted for resume;
  a checksum mismatch fails.
- Authentication is optional only when the engine is configured for
  development without authentication, matching existing behavior.

## Verification

1. CLI unit tests cover categorized help, context, history, friendly commands,
   clean query output, media download/resume/cleanup, adaptive chunk sizing,
   and error messages.
2. Node client tests upload and download a file larger than two chunks without
   buffering it as one request, and verify the final checksum.
3. C++ focused tests cover bounded aggregation, honest explain, API-key secret
   handling/revocation, backup delete/journal/export, and media document
   visibility rules.
4. Build the engine, run retained Community tests, package the CLI, and run a
   local engine end-to-end session exercising every displayed command.

Publishing, npm release, GitHub push, and installer publication remain outside
this change until the user completes testing and explicitly requests them.
