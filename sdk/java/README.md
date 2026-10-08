![PacificDB](https://raw.githubusercontent.com/hitesh-reddy-k/pacificdb-community/main/site/assets/pacificdb-logo-symbol.png)

# PacificDB Java client

Version **1.1.2 — prerelease**. Download the matching package from the
[Community v1.1.2 release](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/1.1.2);
registry publication is pending.

A database-first client for the Community engine. Requires Java 11 or later;
Jackson is the only runtime dependency. These examples describe the 1.1.2
prerelease and require its published JAR or tagged-source build, not an older
registry release.

Start a PacificDB server, then connect to a database directly:

```java
import io.pacificdb.PacificDB;
import io.pacificdb.PacificDBException;
import java.util.Map;

String url = System.getenv().getOrDefault("PACIFICDB_URL", "pacificdb://localhost:9000/app");
try (var db = PacificDB.connect(url)) {
    db.createDatabase();             // create the URL's database and select it
    db.createCollection("users");
    db.insert("users", Map.of("id", "1", "name", "Ada"));
    System.out.println(db.findOne("users", Map.of("id", "1")));
    db.updateOne("users", Map.of("id", "1"), Map.of("$set", Map.of("name", "Grace")));
    System.out.println(db.count("users"));
}
```

Create calls are not implicit or idempotent: omit them when the database or
collection already exists. No project or separate selection call is required.
Existing `PacificDBClient(host, port, database)` and full
`PacificDBClient(host, port, userId, database, tls, timeoutMs)` constructors remain.
The client is now `AutoCloseable`.

`PacificDB.connect(url)` eagerly authenticates URL credentials and checks the
connection with `ping`; `PacificDB.fromUrl(url)` is lazy. Both accept an explicit
options map or immutable `ConnectionOptions.fromUrl(url).withOverrides(...)`.
URL credentials must contain both username and password. Keep private URLs in
environment variables, not source or logs. Use `pacificdbs://` for TLS with
verified hostname and default trust. A `caFile` option loads custom X.509 trust
certificates. TLS 1.2 or later is required. Authentication on plain TCP does not
encrypt traffic.

URL defaults: port 9000, user `system`, 30,000 ms deadline, pool size 16. Query
keys are `userId`, `timeoutMs`, `poolSize`, `caFile`; duplicate/unknown keys and
conflicting explicit options fail before connecting. Pools are limited to 32.
`maxResponseBytes` can lower the default 64 MiB response limit. Commands are
limited to 256 levels of JSON nesting. Idle connections are reused only when
the engine explicitly advertises keepalive.

Database methods: `createDatabase(name, dbType)` with no-name/type overloads,
`listDatabases()`, `useDatabase(name)`, `dropDatabase(name)` with a no-name
overload. Creation selects on success; switching checks the engine's authorized
listing. Failed create/switch/drop calls preserve selection; dropping the
selected database clears it. `createCollection`, `listCollections`, and
`dropCollection` operate on the selected database.

Document methods: `insert`, `insertMany`, `find`, `findOne`, `count`, `aggregate`,
`explain`, `updateOne`, `updateMany`, `deleteOne`, `deleteMany`, `bulkWrite`.
Use ordinary `Map<String,Object>` / `List` JSON values. `find` supports no-filter
and pagination overloads (`limit=-1`, `offset=0`). `findOne` returns one row or
`null`, and `listDatabases` returns names; other methods preserve original
protocol responses, including partial bulk results. `capabilities()` reports
server features. `request(Map)` retains its object response contract;
`requestValue(Map)` also accepts array responses. The engine remains responsible
for namespace rules and authorization.

Every operation snapshots selected scope and caller JSON before network I/O.
Do not mutate input while the method itself is copying it. Selection is shared
client state; use separate clients for independent concurrent selection.
A completed drop cannot clear a newer selection of another database.

Failures are typed `PacificDBException` (still an `IllegalStateException`) with
`getCode()` and sanitized `getResponse()`. TLS failures now use code `tls_error`
with no private underlying cause. Known credentials are redacted from error
diagnostics; successful values are unchanged. A disconnect/timeout after a
write send can have an unknown outcome: **no automatic write retry**. Deadlines
cover pool wait/connect/TLS/read and stalled writes; the OS DNS resolver can
exceed them. `close()` is idempotent, closes active sockets and wakes waiters.


## Media and backups

```java
import java.nio.file.Path;

// Small in-memory attachments; normal collection documents.
db.media().put("assets", "logo", new byte[] {0, (byte)255}, Map.of("contentType", "image/png"));
byte[] attachment = (byte[])db.media().get("assets", "logo").get("data");

var manifest = db.media().uploadFile("assets", Path.of("video.mp4"));
db.media().downloadFile((String)manifest.get("id"), Path.of("download.mp4"), "assets");
var backup = (Map<?, ?>)db.backups().create(Map.of("description", "before maintenance"));
db.backups().export((String)backup.get("backup_id"), Path.of("backup.json"));
```

For explicit resume, use `uploadFile(collection, path, contentType, chunkBytes,
resumeId)`. The simple overload chooses negotiated bounds; nullable content
type/chunk/resume arguments use defaults. Catch `MediaUploadException` and
inspect `getUploadId()`, `getNextChunk()`, `getReceivedChunks()`,
`getReceivedBytes()` and `isResumable()`. Resume with the same file, collection
and chunk setting. A lost acknowledgement may mean a chunk was stored; resume
asks the server which chunks exist and skips them. No automatic write replay.

Uploads reject empty/nonregular files and changed content. Downloads require
selected database and validate database/optional collection, size/index/base64,
chunk and whole SHA-256. File downloads and backup exports use unique private
sibling temporary files and verified atomic replacement; no unsafe copy
fallback when atomic replacement is unsupported. The parent directory must
exist. Backup export streams the existing `pacificdb-full-backup-v1` JSON format,
not an engine restore API. File errors use typed SDK exceptions. Each request
has a deadline; the whole transfer has no single overall deadline. In-memory
attachments allocate the supplied bytes; use file APIs for large media.

Apache-2.0. Stable-release and registry qualification remain separate.

Advanced named operations and wire aliases are documented in [SDK capabilities](https://github.com/hitesh-reddy-k/pacificdb-community/blob/main/docs/SDK_CAPABILITIES.md).
