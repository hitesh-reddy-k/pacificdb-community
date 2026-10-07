# PacificDB Security Remediation Design

**Date:** 2026-10-07

**Baseline:** `89735a9b6467094e0b9a679b9812ad514284e223`

**Scope:** Remediate all four high- and five medium-severity findings in
`build/security-scan-89735a9/report.md`, then regenerate exact-revision release
evidence.

## Intent and success criteria

PacificDB must enforce database ownership at the engine boundary, keep managed
local listeners local, reject unbounded protocol work before allocation, and
make configured security controls match effective controls. The fixes must
preserve legitimate local Workbench use, clustered production operation,
resumable media, supported SDK behavior, and existing stored data.

The work is complete when:

1. Every reported exploit path has a focused regression test that failed on the
   baseline and passes on the remediated revision.
2. Valid owner, grantee, API-key, superadmin, local Workbench, media, Raft,
   lockout, SDK, and query workflows still pass.
3. Required repository suites, packaging checks, dependency audit, and the
   repository-owned security scan pass on one immutable revision.
4. Eight-hour load and physical-power evidence are regenerated for that same
   revision.
5. An exact-revision external-review bundle is generated. Release status does
   not claim the independent-review gate is satisfied until a separate person
   or organization signs the result and the repository validator accepts it.

## Chosen approach

Use targeted enforcement at the existing shared boundaries. Extend database
metadata with ownership and grants, route authenticated requests through one
scope authorizer, and reuse existing protocol/configuration limits. Add RE2
because the C++ standard regex engine cannot provide a reliable execution
bound. Do not move user data or introduce a separate policy service.

Rejected alternatives:

- Binding `userId` directly to the login username would break clients that use
  the existing `system` namespace and would not provide owner-managed sharing.
- Disabling regex would close the denial-of-service path but unnecessarily
  remove supported query behavior.
- A separate authorization service and new token protocol would substantially
  expand this release's migration and operational risk.

## Database identity, ownership, and grants

A database security identity is the tuple `(userId, dbName)`. `system` is only
a storage namespace; it never grants access.

`db.meta` gains a versioned security object:

```json
{
  "security": {
    "version": 1,
    "owners": ["alice"],
    "grants": {
      "bob": "read-only",
      "carol": "read-write"
    }
  }
}
```

When engine authentication is enabled, the authenticated creator becomes the
first owner. API keys use their creator as principal but retain the API key's
role as a ceiling. A database grant also cannot exceed the principal's global
account role. Ownership grants ACL-management authority; the owner's data
operations remain capped by the global account role. Granting `owner` adds a
co-owner, and revoking the final owner is refused. An atomic transfer operation
replaces the owner set with the selected principal.

`SUPERADMIN` may access any database for recovery and ownership repair. Every
superadmin override, owner assignment or transfer, grant, and revocation emits
an attributable audit event. When authentication is disabled, current managed
local behavior remains unchanged.

Existing metadata without the security object is `unassigned`. It remains on
disk unchanged and is accessible only to `SUPERADMIN` until explicitly
assigned. New authenticated production startup requires an existing
superadmin or operator-supplied `PACIFICDB_ENGINE_ADMIN_USERNAME` and
`PACIFICDB_ENGINE_ADMIN_PASSWORD`; there is no built-in credential and an
unbootstrappable first production start fails closed.

ACL mutations use replicated Raft operations and atomically replace `db.meta`
on every node. The public administrative surface provides operations to:

- list unassigned databases as `SUPERADMIN`;
- assign an initial owner as `SUPERADMIN`;
- inspect a database ACL as an owner or `SUPERADMIN`;
- grant or revoke `read-only`, `read-write`, or `owner` access as an owner or
  `SUPERADMIN`; and
- transfer ownership as an owner or `SUPERADMIN`.

## Shared authorization boundary

One engine request authorizer resolves the authenticated principal, effective
namespace/database, requested action, global role, and database ACL before any
catalog, queue, replication, cache, or storage work. It validates each nested
bulk operation independently because nested operations may override scope and
may require different permissions. Authentication-disabled trusted local and
Raft/recovery paths remain separate from client authorization.

Ordinary unauthorized database requests return `permission_denied`. ID-only
media requests return the existing not-found error for foreign resources so
they do not disclose resource existence.

Media authorization is resource-derived:

- begin authorizes the requested database;
- resume, put-chunk, finalize, get, get-chunk, delete, and targeted cleanup
  resolve the stored manifest and authorize its immutable database;
- list returns only manifests from authorized databases and treats `db` and
  `dbName` consistently; and
- namespace-wide cleanup processes only resources the caller may delete.

Ready-resume idempotency, out-of-order chunks, duplicate retries, and cleanup
state transitions remain supported.

## Raft and effective configuration

Desktop, Node CLI, and native CLI managed engines explicitly set
`ENGINE_BIND_HOST=127.0.0.1` and `RAFT_BIND_HOST=127.0.0.1`. Invalid listener
addresses fail startup rather than falling back to a wildcard. Explicit
cluster deployments retain non-loopback binds and production Raft mTLS.

Validation, listener creation, engine-auth enforcement, and startup status use
the same resolved `EnvConfig` source. Listener settings are startup snapshots;
configuration reload does not claim to rebind a live socket.

The outer Raft frame reader rejects zero-length frames and frames larger than
the existing 64 MiB `kMaxRaftPayloadBytes` limit before allocation. Receive
accounting uses size-safe types, and the sender rejects frames above the same
limit before its 32-bit length conversion. Legal fragmented frames, snapshot
chunks, persistent connections, and supported legacy codecs remain valid.

## Bounded media lifecycle

Media discovery and deletion enumerate actual stored chunk records in bounded
pages. No operation performs work proportional to a caller-declared sparse
`chunk_count`. The implementation reuses the catalog's existing paginated
deletion pattern and preserves bounded materialization, out-of-order uploads,
restart reconciliation, legacy state versions, and files larger than ordinary
query-result limits.

Admission validation continues rejecting invalid sizes and counts. Persisted
legacy manifests remain inspectable and removable even when their declared
count is impractical.

## Node response framing

The Node transport counts raw bytes before decoding or appending. A response
above 64 MiB closes and retires that connection and rejects the current
request; queued requests may use a fresh connection and writes are never
automatically replayed. Counters reset on request completion, connection
failure, reconnect, and close. Split UTF-8 sequences and legal chunked media or
backup responses remain supported. The limit matches the existing Python
transport default.

## Account lockout

`User` persists `lockedUntil` as an epoch timestamp. Reaching the failed-login
threshold records `now + lockoutDuration`; attempts before that instant remain
locked and do not shorten the deadline. Successful authentication and an
administrator password reset clear the attempts, lock flag, and timestamp.

Legacy unlocked accounts default to no deadline. Legacy records marked locked
without a timestamp remain locked until a superadmin resets them, avoiding an
unsafe automatic unlock.

## Bounded regex queries

Client `$regex` evaluation uses RE2 for guaranteed linear-time matching. The
engine retains search semantics and the `i` option. Patterns are limited to
4 KiB. Invalid patterns, unsupported options, and unsupported RE2 constructs
such as backreferences or lookarounds produce an explicit invalid-query error;
they are not silently treated as no match.

The shared matcher covers find, count, vector filters, update/delete filters,
aggregation matching, nested logical operators, and `$elemMatch`. Linux,
macOS, Windows/vcpkg, container, and package build definitions include RE2.

## Verification strategy

Each finding follows red-green TDD through the narrowest real boundary:

1. cross-database media access, the `db` alias, ready resume, foreign chunks,
   and filtered/global cleanup;
2. loopback binds in all three launchers and fail-closed invalid addresses;
3. oversized/zero Raft headers without declared allocation;
4. foreign namespaces, nested bulk overrides, owner/grantee/global-role
   intersections, API keys, and audited superadmin access;
5. file-only effective binds/auth and truthful startup status;
6. huge sparse declared counts with zero and high-index stored chunks through
   get, delete, cleanup, and restart reconciliation;
7. unterminated and newline-terminated oversized Node responses while
   preserving fragmented UTF-8 and pool recovery;
8. immediate post-lock attempts, expiry, restart persistence, and legacy locked
   records; and
9. adversarial regex input in an externally bounded test process plus valid
   and explicitly unsupported syntax.

After focused checks, run the complete Community C++ suite, Node SDK and CLI
suites, Workbench checks, packaging checks, dependency audit, and repository
security scan. Review the integrated diff for bypasses and regressions before
creating the immutable evidence revision.

## Release evidence and limitations

The earlier load and physical-power artifacts remain valid historical evidence
for `89735a9`, but they cannot certify changed code. The fixed revision receives
a new eight-hour load run and a new physical-power run with operator-assisted
cuts. Package evidence is regenerated for available platforms; unavailable
platform or signing evidence remains explicitly blocked rather than inferred.

`scripts/security_review.py bundle` creates the exact-revision external-review
package. Repository-owned review, automated scans, or another agent in the
same development process do not count as independent external review. Only a
separate reviewer or organization may sign `review-result.json`; the repository
validator must then verify its revision, report digest, artifact digests,
reviewer identity, and absence of unresolved high or critical findings.

## Non-goals

- Moving databases between namespaces.
- Replacing existing account roles or authentication protocols.
- Building a separate policy service.
- Claiming independent review without an external signer.
- Claiming platform, signing, power, or endurance evidence that was not run on
  the final immutable revision.
