# PacificDB v1.2 SDK guide

Use matching **1.2.0** client packages with the 1.2.0 engine. Node requires Node18+,
Python requires Python3.10+, and Java requires Java11+. Clients use the engine's
native JSON protocol. The Node client works in a Next.js **Node server runtime**;
keep credentials on the server. Use the Python client from FastAPI or other Python
services. Neither client is a browser database server or an engine installer.

Java coordinates are `in.pacificdb:pacificdb-client:1.2.0`; imports remain
`io.pacificdb.*`. Install the release's JAR and matching POM into a local Maven
repository as shown below. Maven Central publication is deferred.

| Client | Package | Minimum runtime | Runtime dependencies |
|---|---|---|---|
| Node | `@pacificdb/client@1.2.0` | Node 18 | None |
| CLI/browser Workbench | `@pacificdb/cli@1.2.0` | Node 18 | Matching Node client |
| Python | `pacificdb==1.2.0` | Python 3.10 | None |
| Java | `in.pacificdb:pacificdb-client:1.2.0` | Java 11 | Jackson Databind 2.22.4 and its transitive dependencies |

The npm CLI does not install the engine. Install the matching native package or
connect to an existing engine. The CLI and native shell both use the command
`pacificdb`; invoke the project-local npm binary explicitly when testing it.

## Install release assets

For the published npm packages, install the explicit version:

```sh
npm install @pacificdb/client@1.2.0 @pacificdb/cli@1.2.0
./node_modules/.bin/pacificdb --version
```

After verifying the release checksums, create a clean consumer directory. Install
both npm archives together so the CLI resolves its exact matching dependency:

```sh
npm install /absolute/path/pacificdb-client-1.2.0.tgz /absolute/path/pacificdb-cli-1.2.0.tgz
./node_modules/.bin/pacificdb --version
python3 -m venv .venv
.venv/bin/python -m pip install /absolute/path/pacificdb-1.2.0-py3-none-any.whl
```

Java's binary, source and Javadoc JARs and dependency-bearing POM are distributed
separately. Install the matching package into a local Maven repository; this does not
publish it remotely:

```sh
mvn org.apache.maven.plugins:maven-install-plugin:3.1.2:install-file \
  -Dfile=/absolute/path/pacificdb-client-1.2.0.jar \
  -DpomFile=/absolute/path/pacificdb-client-1.2.0.pom
```

Then declare this dependency in the consumer's POM:

```xml
<dependency>
  <groupId>in.pacificdb</groupId>
  <artifactId>pacificdb-client</artifactId>
  <version>1.2.0</version>
</dependency>
```

The POM resolves Jackson from Maven Central. Offline consumers must provision
those dependencies separately. PacificDB's own Maven Central publication is
deferred; the release JAR/POM can be installed locally without an owner GPG key.
Java API imports remain `io.pacificdb.*`.

## Equivalent database and document operations

Set `PACIFICDB_URL` to a protected credential-bearing URL such as
`pacificdb://USERNAME:PASSWORD@localhost:9000/app` (URL-encode credentials).
Use `pacificdbs://` for certificate-verified TLS. Never commit the private URL.
Start with a new database name; create operations are not idempotent.

Node:

```js
import { PacificDB, releaseInfo } from '@pacificdb/client';
const db = await PacificDB.connect(process.env.PACIFICDB_URL);
try {
  await db.createDatabase();
  await db.createCollection('users');
  await db.insert('users', { id: '1', name: 'Ada' });
  console.log((await db.find('users', { id: '1' })).data[0]);
  await db.updateOne('users', { id: '1' }, { $set: { name: 'Grace' } });
  await db.deleteOne('users', { id: '1' });
  console.log(releaseInfo);
} finally { db.close(); }
```

Python:

```python
import os
from pacificdb import PacificDB, __version__
with PacificDB.connect(os.environ['PACIFICDB_URL']) as db:
    db.create_database()
    db.create_collection('users')
    db.insert('users', {'id': '1', 'name': 'Ada'})
    print(db.find_one('users', {'id': '1'}))
    db.update_one('users', {'id': '1'}, {'$set': {'name': 'Grace'}})
    db.delete_one('users', {'id': '1'})
    print(__version__)
```

Java (inside an application's `main` method):

```java
import io.pacificdb.PacificDB;
import io.pacificdb.ReleaseInfo;
import java.util.Map;

try (var db = PacificDB.connect(System.getenv("PACIFICDB_URL"))) {
    db.createDatabase();
    db.createCollection("users");
    db.insert("users", Map.of("id", "1", "name", "Ada"));
    System.out.println(db.findOne("users", Map.of("id", "1")));
    db.updateOne("users", Map.of("id", "1"), Map.of("$set", Map.of("name", "Grace")));
    db.deleteOne("users", Map.of("id", "1"));
    System.out.println(ReleaseInfo.VERSION);
}
```

All clients preserve engine authorization and error responses and support bounded
connection pools and media streaming. Python exposes typed `PacificDBError`;
Java exposes `PacificDBException`; Node rejects requests with sanitized errors.
After a disconnected write its outcome can be unknown: automatic write retry is
not provided. Close clients after use.

<a id="native-vector-preview"></a>

## Native vector operations

Native flat/HNSW index lifecycle and lexical/hybrid search remain opt-in through
`PACIFICDB_VECTOR_PREVIEW_ENABLED=1` on the engine. Standalone preview performance
and distributed qualification limits remain documented in
[the vector readiness notes](docs/V1.2_VECTOR_READINESS.md).

| Operation | Node | Python | Java |
|---|---|---|---|
| Insert dense/sparse vector | `putVector(collection,id,vector,metadata)` | `vectors.insert(collection=...,data=...)` | `vectors().insert(Map)` |
| Query | `queryVector(collection,vector,options)` | `vectors.query(**wire_fields)` | `vectors().query(Map)` |
| Create index | `createVectorIndex(collection,options)` | `vectors.create_index(**wire_fields)` | `vectors().createIndex(Map)` |
| List/status/rebuild/drop | `listVectorIndexes`, `vectorIndexStatus`, `rebuildVectorIndex`, `dropVectorIndex` | `vectors.list_indexes`, `index_status`, `rebuild_index`, `drop_index` | `vectors().listIndexes`, `indexStatus`, `rebuildIndex`, `dropIndex` |
| Hybrid | `queryHybrid(collection,vector,text,options)` | `vectors.query_hybrid(**wire_fields)` | `vectors().queryHybrid(Map)` |

Python/Java option maps retain wire names such as `index_name`, `search_mode`,
`require_index`, `vector_name`, `ef_search`, `timeout_ms`. Node maps corresponding
camelCase options. Index creation uses `name`, `algorithm`, `dimensions`, `metric`
and admission budgets inside its `options` argument. Wait for status `READY`
before requiring the index. Collection schema is `vectorSchema` for Node,
`vector_schema` for Python, and the second `createCollection` argument for Java.
Dense vectors are arrays; sparse vectors use the checked `sparse-v1` object.
Named vectors can be inserted using ordinary document `insert` with a `vectors`
object. Exact fallback and ANN execution diagnostics come from the engine.

## Reproduce staged qualification

From a checkout with build tools installed, `scripts/build-sdk-staging.sh` creates
npm tarballs, wheel/sdist and Java artifacts under
`build-v1.2-release/staging/sdk`. It refuses a nonempty destination. No registry
write or signing occurs. Inspect hashes in
`build-v1.2-release/evidence/sdk/inspection.json`.

Run `scripts/test-node-installed-client.sh STAGING/npm`,
`scripts/test-python-installed-client.sh STAGING/python/*.whl` and
`scripts/test-java-installed-client.sh STAGING/java`. For live qualification set
`PACIFICDB_TEST_ENGINE_BUILD` to a fresh engine build, and
`PACIFICDB_TEST_NODE_MODULE` to an installed client entry URL for the Python/Java
controllers. Installed tests use isolated consumers and never import repository
SDK code. The live tests exercise authenticated TCP/TLS and SIGKILL recovery;
preview checks additionally require `PACIFICDB_TEST_TYPED_VECTORS=1`,
`PACIFICDB_TEST_NATIVE_VECTOR_PREVIEW=1`, `PACIFICDB_TEST_VECTOR_SCHEMA=1` and the
engine preview environment flag. Linux evidence does not establish Windows/macOS
installation or minimum-runtime compatibility; CI has explicit runtime matrices.
