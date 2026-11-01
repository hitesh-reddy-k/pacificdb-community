# Pre-release v1.2 SDK guide

The engine base version is `1.2.0`; the candidate is `v1.2.0-rc.2` (Pre-release
v1.2). Node and Java packages use `1.2.0-rc.2`; Python uses PEP 440 `1.2.0rc2`.
RC.2 is public on GitHub with checksummed SDK packages, after correcting RC.1's
fresh-install short-window Workbench defect. RC.1 packages remain unchanged.
The RC.2 npm client and CLI are public under `next`; registry-installed consumers
passed the independent live verification. PyPI/Maven still require owner setup, confirmed
by the owner. The wheel/JAR/POM installation commands below work independently
of those registries. The release manifest identifies actual tested bytes.

| Client | Package | Minimum runtime | Runtime dependencies |
|---|---|---|---|
| Node | `@pacificdb/client@1.2.0-rc.2` | Node 18 | None |
| CLI/browser Workbench | `@pacificdb/cli@1.2.0-rc.2` | Node 18 | Matching Node client |
| Python | `pacificdb==1.2.0rc2` | Python 3.10 | None |
| Java | `io.pacificdb:pacificdb-client:1.2.0-rc.2` | Java 11 | Jackson Databind 2.22.3 and its transitive dependencies |

The npm CLI does not install the engine. Install the matching native package or
connect to an existing engine. The CLI and native shell both use the command
`pacificdb`; invoke the project-local npm binary explicitly when testing it.

## Install exact local assets

For the published npm packages, install the explicit candidate version:

```sh
npm install @pacificdb/client@1.2.0-rc.2 @pacificdb/cli@1.2.0-rc.2
./node_modules/.bin/pacificdb --version
```

RC.2's registry-installed consumers passed authenticated TCP/TLS, document/media
roundtrips and SIGKILL recovery in public-install run41324080656, attempt2.
The initial attempt's CLI-registry propagation failure is retained separately.
A native engine is still required.
`@latest` selects a different release.

After verifying the release checksums, create a clean consumer directory. Install
both npm archives together so the CLI resolves its exact candidate dependency:

```sh
npm install /absolute/path/pacificdb-client-1.2.0-rc.2.tgz /absolute/path/pacificdb-cli-1.2.0-rc.2.tgz
./node_modules/.bin/pacificdb --version
python3 -m venv .venv
.venv/bin/python -m pip install /absolute/path/pacificdb-1.2.0rc2-py3-none-any.whl
```

Java's binary, source and Javadoc JARs and dependency-bearing POM are staged
separately. Install the candidate into a local Maven repository; this does not
publish it remotely:

```sh
mvn org.apache.maven.plugins:maven-install-plugin:3.1.2:install-file \
  -Dfile=/absolute/path/pacificdb-client-1.2.0-rc.2.jar \
  -DpomFile=/absolute/path/pacificdb-client-1.2.0-rc.2.pom
```

Then declare this dependency in the consumer's POM:

```xml
<dependency>
  <groupId>io.pacificdb</groupId>
  <artifactId>pacificdb-client</artifactId>
  <version>1.2.0-rc.2</version>
</dependency>
```

The POM resolves Jackson from Maven Central. Offline consumers must provision
those dependencies separately. Central publication and public repository
availability have not been established. The optional Central validation workflow
requires namespace ownership, an owner token and GPG identity; its USER_MANAGED
upload does not publish. Registry workflows are manual and protected by their
configured environments. npm candidates publish only to `next`.

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

## Native vector preview

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
