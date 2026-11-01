# Vector usage — v1.2.0-rc.2

Native vectors are an opt-in standalone preview. Authoritative records share
PacificDB's WAL/LSM storage. ANN and lexical indexes are derived structures.
Full production qualification and distributed vector completeness are incomplete.

## Enable the preview

Before starting a separately managed engine set
`PACIFICDB_VECTOR_PREVIEW_ENABLED=1`. For desktop, quit the app and set
`PACIFICDB_WORKBENCH_VECTOR_PREVIEW=1` before launching its executable.
A running process does not inherit later environment changes.

Install and connect the matching SDK using [SDK_GUIDE.md](../SDK_GUIDE.md).
The Node examples below assume its connected `db` client and a new database.

## Dense, sparse and named storage

Dense values are finite numeric arrays. Sparse values use `sparse-v1`, a positive
dimension, sorted unique in-range indices and equally sized finite values.
Authoritative numeric compatibility is retained; a derived HNSW float32 copy does
not silently convert historical records. Named spaces live in `vectors`.

```js
await db.request({ action: 'insertVector', collection: 'articles', data: {
  id: 'article-1', category: 'news', vectors: {
    semantic: [0.25, 0.75],
    terms: { format: 'sparse-v1', precision: 'float64', dimensions: 1000000,
      indices: [3, 999999], values: [0.125, -2] }
  }
}});
const hits = await db.queryVector('articles', [0.25, 0.75], {
  vectorName: 'semantic', searchMode: 'exact', k: 10,
  metric: 'cosine', filter: { category: 'news' }
});
```

Create `articles` before inserting. Ordinary document retrieval/update/delete
operations apply to the same IDs. `insertMany` supports batch document shapes.
Schema-less collections retain compatibility; declare a schema to enforce vectors
on every write. Sparse/named spaces do not imply a separate durable store.

## Enforce collection schemas

```js
await db.createCollection('vectors', {
  vectorSchema: {
    default: { kind: 'dense', dimensions: 2, metric: 'dot', required: true },
    allow_unconfigured: false, format_version: 1
  }
});
```

Schemas are immutable at creation. Dimensions, metric, kind, required spaces and
invalid numeric values are validated. Existing schema-less collections remain
supported. Online schema changes and clustered schema creation are unavailable.
Python uses `create_collection(..., vector_schema=...)`; Java uses the schema-map
`createCollection` overload. See SDK examples for their wire option maps.

## Exact and approximate queries

Supported metrics are cosine, L2/Euclidean and dot product. Exact search retains a
bounded top-K and ranks all eligible rows within configured resource limits.
Timeout, cancellation and resource-limit failures are explicit errors.
`searchMode: 'exact'` never executes HNSW. ANN requires an eligible ready index;
exact fallback must follow the explicit request policy. Inspect the executed mode.

```js
await db.createVectorIndex('vectors', {
  name: 'ann', algorithm: 'hnsw', dimensions: 2, metric: 'dot',
  maintenance: 'incremental', max_elements: 2000000,
  max_source_bytes: 268435456, max_index_bytes: 1073741824
});
// Poll db.vectorIndexStatus('vectors', 'ann') until READY;
// handle FAILED/STALE and apply an operator deadline to polling.
const result = await db.queryVector('vectors', [0.2, 0.8], {
  indexName: 'ann', requireIndex: true, searchMode: 'ann',
  allowExactFallback: false, metric: 'dot', k: 10
});
```

Insert vectors before building if you need an index of populated data. Configure
M, ef_construction and ef_search through index options within engine bounds.
Lifecycle methods are `createVectorIndex`, `vectorIndexStatus`,
`listVectorIndexes`, `rebuildVectorIndex` and `dropVectorIndex`.
Dropping an index does not delete authoritative records.

Build once and reuse eligible indexes. Strict ANN can reject queries during
mutation lag. Incremental dense maintenance is opt-in; physical history includes
tombstones and requires manual rebuilding at its configured limit. Sparse,
flat and lexical incremental maintenance are unsupported. After restart, durable
index definitions remain but graphs require explicit rebuilding. Large builds
can take many minutes. Index READY does not waive freshness checks.

## Filtering and hybrid retrieval

Pass `filter` with the existing document filter syntax. Selective/unsupported ANN
filters need an explicitly allowed complete exact fallback or return an eligibility
error. Do not interpret approximate output as exact completeness.

`queryHybrid(collection, vector, text, options)` combines dense retrieval with an
actual BM25 lexical index using Reciprocal Rank Fusion. Configure the selected
index and text field; use the SDK hybrid options. This differs from the legacy
substring TEXT predicate. Hybrid and schema/index administration are SDK/raw
operations; every option is not exposed as a Workbench button.

Use [the SDK operation matrix](../SDK_GUIDE.md#native-vector-preview) and
[company test scenarios](../COMPANY_TESTING_GUIDE.md). Unsupported distributed
search, automatic graph compaction, advanced compression/disk ANN and complete
upgrade/power-loss/security qualification remain limitations of this prerelease.
