# Database-first CLI, Workbench and SDK qualification — 3 October 2026

The isolated `ux/database-first-20261003` branch implements the approved direct
**database → collection → document** workflow. Ordinary CLI/Workbench flows need
no project. Existing project SDK/engine APIs and stored metadata are retained.
This is a locally qualified release candidate; nothing was uploaded, tagged,
merged, pushed or deployed. It is not a zero-defect or production certification.

The healthy recovered repository is
`/home/hitesh/pacificdb-database-first-20261003`. The original backup filesystem
failed during the program and was left untouched. Recovery preserved the checked
source snapshot; see the [imported baseline](../superpowers/baselines/2026-10-03-workbench-desktop-package-baseline.md).

## Behavior and simple syntax

CLI commands create/select databases directly, persist only safe database context,
and support strict `pacificdb://` / `pacificdbs://` URLs. Workbench presents accessible
databases and their collections directly. Explicit URL selections are respected;
missing/deleted selections clear safely. Navigation bounds summary concurrency
and ignores stale responses. Browser Host/origin/session-token protections remain.
The connection dialog provides secret-free CLI/Node/Python/Java examples.

Python and Java support verified TLS, bounded pooled JSON connections, captured
request scope, authentication before queued writes, deadlines and explicit close.
No sent write is automatically retried. Python has no runtime dependencies; Java
retains Jackson as its only direct runtime dependency. Named operation families
cover all 135 Community dispatches (117 canonical actions and 18 aliases), including
indexes, vectors, media, backups, security, admin and legacy project APIs. That
coverage count is not semantic integration proof for every advanced operation.

Both packages provide bounded file upload/resume, checksum-verified atomic downloads
and streamed backup export. Temporary files are privately owned; an invalid or
interrupted transfer preserves an existing destination. See their READMEs for
negotiated limits, typed progress errors and resume semantics.

With a running engine, the basic workflow is:

```python
from pacificdb import PacificDB

with PacificDB.connect("pacificdb://localhost:9000/app") as db:
    db.create_database()
    db.create_collection("users")
    db.insert("users", {"id": "1", "name": "Ada"})
    print(db.find_one("users", {"id": "1"}))
```

```java
import io.pacificdb.PacificDB;
import java.util.Map;

try (var db = PacificDB.connect("pacificdb://localhost:9000/app")) {
    db.createDatabase();
    db.createCollection("users");
    db.insert("users", Map.of("id", "1", "name", "Ada"));
    System.out.println(db.findOne("users", Map.of("id", "1")));
}
```

Each example assumes its own fresh `app` database. Omit create calls for existing
resources. Use an environment variable for private URLs and `pacificdbs://` with
verified trust for encrypted connections. These new APIs require this candidate's
built package, not an older registry version.

## Fresh evidence and review

Runtime source qualification is bound to
`e6781c9e1cc5a8722df09c9e7a9f10242a5f34b9`. Subsequent evidence/plan commits change
no runtime or SDK packaging input. The artifact manifest records its own exact
clean build revision and hashes, rather than treating documentation as an artifact.

Local platform: Linux x86-64, Node 24.19.0, Python 3.12.3,
JDK 17.0.20.1 and Maven 3.8.7. Declared package floors are Python 3.10 and Java 11;
all produced Java class files were checked for Java 11 bytecode.

| Check | Local result and scope |
| --- | --- |
| `bash scripts/test-community.sh build` | PASS after review fixes: API/auth, storage/Raft/recovery, disk-full and RF3 checks plus SDK integration. Optional mixed-version RF3 skipped: no exact older artifact supplied. |
| Python / Java / Node SDK / npm CLI suites | 133 / 29 / 28 / 27 tests passed, respectively. Shared URL fixtures: 8 valid, 39 invalid. |
| Native/npm database-first integration | PASS: authenticated TCP/TLS, context/error/privacy contracts, CRUD and restart. |
| Workbench API/browser | PASS: direct database flows, real CRUD/index/vector/media, scope races, empty states, 129-byte database creation and Unicode media upload. |
| Linux packaged desktop | PASS: freshly built unpacked Electron app, sandbox, bundled engine/CLI, CRUD/media and restart persistence. Bundled binaries match the qualified build hashes. |
| Installed wheel and Maven consumer | Exact artifact installation checked independently of source-directory imports. Both installed clients passed authenticated TCP, verified TLS and actual SIGKILL recovery. |
| Cross-SDK exact-value integration | Each client verified every expected application field of 160 acknowledged documents and 700,000 media bytes before/after SIGKILL and over TLS. Also exercised backup export/restore, vectors/indexes, API-key refusals and retained legacy project APIs. |
| Archive/release checks | Wheel and sdist pass Twine and inventory/metadata/RECORD checks; Java binary/source/Javadoc/POM, signatures and checksums are inspected. Workflow contracts and actionlint pass. |

The [independent whole-branch review](2026-10-03-database-first-independent-review.md)
found six P2 issues on its reviewed revision. All were addressed in one fix pass:

| Finding | Verified correction |
| --- | --- |
| Native raw-request credentials in errors | Capture sensitive outbound values; sanitize sensitive error fields recursively. Real CLI fake-peer regression failed before and passed after. Success responses retain their contract. |
| Missing database/collection silently switches scope | Clear missing selections; display explicit empty state. Browser deletion and missing URL tests pass without an unrelated collection request. Initial unconfigured selection and explicit navigation remain available. |
| Non-Latin media header names | Percent-encode/decode captured database and collection once; validate decoded names. Adapter regression and real Unicode browser upload pass. |
| Old 128-byte direct database limit | Align adapter/create guidance to engine's 255-byte database limit. 255-byte adapter boundary, 256-byte refusal and 129-byte real browser creation pass. |
| Python per-address timeout reset | Track each connecting socket; recompute one request budget per address. Controlled multiple-address and close-during-connect regressions failed before and pass after. OS DNS remains outside socket interruption. |
| Python scalar responses accepted as writes | Reject non-object/non-array protocol frames, discard connection, retain previous selection. Six scalar-frame public create-database regressions failed before and pass after. |

The broad gate initially stopped at a separate Node concurrent-launcher race.
A deterministic test reproduced root ownership before discovery metadata.
The Node launcher now follows the native launcher's bounded wait only under the
startup lock; no-start and unrelated-owner behavior remain covered. The later
full gate passed. A stale startup lock can still delay refusal until the existing
bounded timeout. All failed attempts and red/green traces remain retained.

## Artifacts, publication and remaining gates

[Artifact hashes and signing status](2026-10-03-sdk-package-artifacts.json) identify
`pacificdb==1.0.1` wheel/sdist and `io.pacificdb:pacificdb-client:1.0.1` binary,
source, Javadoc and POM plus a local Central ZIP. Exact copies are also frozen under
`release-sdk/qualified-e6781c9/` so later target-directory builds cannot replace
the qualified bytes; each manifest entry includes its frozen path. The ZIP is signed by an explicitly
**test-only** disposable identity. Cryptographic checks reject a wrong fingerprint
and a modified signed JAR. Its private keyring was deleted. This is not a release
owner signature or remote Central acceptance.

The default-safe Central plugin skip mode skipped staging locally, despite the
documented bundle-only behavior. The minimal local packer therefore verifies and
packs existing artifact bytes without rebuilding/uploading. Prepared release jobs
consume inspected bytes, verify tag/version/revision/hashes, and require protected
environments. PyPI uses job-scoped OIDC; Central upload stops at validation with
manual owner publication. See [publication instructions](../SDK_PUBLISHING.md).

These requirements remain **not run / blocked externally**:

- Python 3.10, Java 11/21 and Windows CI runtime execution; macOS desktop qualification,
  platform signing/notarization and installed release installers.
- Exact older-build mixed-version RF3 evidence.
- Registry version availability and ownership, Central namespace/signing setup,
  PyPI trusted-publisher registration, actual protected-environment enforcement,
  remote validation and final release-owner approval.
- Required independent security and physical power-interruption evidence from
  [release evidence](../RELEASE_EVIDENCE.md). Process SIGKILL does not model power loss.

No performance ranking or engine certification follows from this UX/SDK work.
The [execution decisions](2026-10-03-database-first-execution-decisions.md) retain
implementation rulings. Full logs/manifests are retained locally under the ignored
`.superpowers/sdd/2026-10-03-sdk-package-qualification/` directory; they are not
implicitly public registry evidence. Nothing in this document waives an existing
release gate.
