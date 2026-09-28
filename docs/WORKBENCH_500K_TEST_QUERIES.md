# Workbench test dataset: 500,000 customers

Inserted and verified: **500,000 customer documents**. All 15 filter totals below were checked against the live engine. The `country_idx` index is ready. The extra collections `crud_sandbox`, `vector_samples`, and `media_samples` contain small samples for the other Workbench tools.

Connection: `127.0.0.1:34383` (desktop ports can change on restart).
Project: **Workbench 500k test** (`project_6ab823adb8b8b3c45dbf4809`).
Database: **workbench_500k_1790452653418**.
Main collection: **customers**.

Refresh the sidebar, select this project/database/collection, then open **Query Workbench**.
Paste only the JSON filter into the editor and press **Ctrl/Cmd+Enter**.
The results show one page, not the full matching count; the expected totals below describe the whole dataset.

## All customers

```json
{}
```

Expected total: **500,000** matching documents.

## Find one ID

```json
{
  "id": "customer-250000"
}
```

Expected total: **1** matching documents.

## India

```json
{
  "country": "India"
}
```

Expected total: **100,000** matching documents.

## Active customers

```json
{
  "status": "active"
}
```

Expected total: **166,667** matching documents.

## Enterprise tier

```json
{
  "tier": "enterprise"
}
```

Expected total: **166,665** matching documents.

## Sequence range

```json
{
  "sequence": {
    "$gte": 100001,
    "$lte": 100100
  }
}
```

Expected total: **100** matching documents.

## Two countries

```json
{
  "country": {
    "$in": [
      "India",
      "USA"
    ]
  }
}
```

Expected total: **200,000** matching documents.

## Active in India

```json
{
  "country": "India",
  "status": "active"
}
```

Expected total: **33,334** matching documents.

## High spending

```json
{
  "spending": {
    "$gte": 9000
  }
}
```

Expected total: **50,000** matching documents.

## Verified customers

```json
{
  "verified": true
}
```

Expected total: **250,000** matching documents.

## Nested field

```json
{
  "address.city": "Hyderabad"
}
```

Expected total: **100,000** matching documents.

## Array membership

```json
{
  "tags": {
    "$in": [
      "enterprise"
    ]
  }
}
```

Expected total: **166,665** matching documents.

## OR conditions

```json
{
  "$or": [
    {
      "country": "India"
    },
    {
      "spending": {
        "$gte": 9000
      }
    }
  ]
}
```

Expected total: **140,000** matching documents.

## Missing field

```json
{
  "phone": {
    "$exists": false
  }
}
```

Expected total: **500,000** matching documents.

## No matches

```json
{
  "id": "customer-does-not-exist"
}
```

Expected total: **0** matching documents.

## Pagination, views, copy, and query history

Run `{}` on customers. Select 25, 50, and 100 rows; use Next and Previous.
Switch between JSON cards, compact, and table views. Open **Explore fields** and
expand `address` and `tags`. Try **Copy value** and **Copy JSON**. Run two filters
and select one from the session query history. Test Ctrl/Cmd+K workspace search,
sidebar search, and light/dark/system display settings.

## Insert, update, and delete safely

Use **crud_sandbox**, leaving the 500,000 customers intact.
Click **New document** and insert:

```json
{"id":"manual-test-001","name":"Workbench test","status":"draft","value":10}
```

Read filter:

```json
{"id":"manual-test-001"}
```

Click **Edit** on the result. Keep this update/delete filter:

```json
{"id":"manual-test-001"}
```

Document JSON for **Update one**:

```json
{"name":"Workbench updated","status":"complete","value":20}
```

Run the read filter again to confirm the change, then choose **Delete one** and
accept confirmation. The same filter should return no results. Cancel a deletion
first to confirm that cancellation preserves the document. Try invalid JSON to
check validation; an empty update/delete filter should be rejected.

## Index management

On **customers**, open **Indexes**. The engine-managed primary index and
`country_idx` are ready. The sequence range filter also works without a secondary sequence index.
Use the India query to exercise the country index; the sequence-range query exercises range filtering.

For create/rebuild/delete checks, use **crud_sandbox**: create `status_idx` on
field `status`, choose ascending, run **Validate**, expand the engine report,
then **Rebuild** and **Delete**. Documents should survive deleting the index.
Only non-unique single-field B-tree indexes are supported; no automatic-index
budget controls or unsupported index types are simulated.

## Vector search

Select **vector_samples** and open **Vectors**.
Vector JSON:

```json
[1, 0, 0, 0]
```

Results / Top K: **3**. Metadata filter: `{}`.
Run Cosine, L2, and Dot product. `vec-x` should be the first result for all three metrics.
For filtered search use:

```json
{"category":"axis"}
```

Only axis vectors should be returned. To test storage, enter ID `manual-vector`,
vector `[0.9, 0.1, 0, 0]`, metadata `{"category":"manual"}`, and choose **Store vector**.
Search with filter `{"category":"manual"}` to retrieve it.

## Media

Select **media_samples** and open **Media**. Preview and download the seeded
`pacificdb-workbench-test.txt`. Upload another small text file, filter by its
filename, preview it, download it, then delete it with confirmation. Cancel a
deletion once to check that the file remains. No 500,000-record dataset is
removed by these actions.

## Monitoring and connection

Open **Monitoring**, click Refresh, and inspect engine health, latency, memory,
connection counts, Raft state, and the live engine counters. These are real
current values; no historical chart data is fabricated. The sidebar connection
button supplies CLI, Node.js, and Java examples for the current engine/project.

## Resource creation and deletion

Create a separate small project, database, and collection through the sidebar.
Insert one document, then delete the collection, database, and finally project.
Cancel each confirmation once before accepting. Keep **Workbench 500k test**
when testing resource deletion so the large dataset remains available.

## Verified query evidence

All 15 filters passed exact engine counts and five-row result-page checks (one row for the ID lookup and zero for no matches). Full count scans took approximately 4.6–5.5 seconds during the background index build. This is observed test timing, not a performance guarantee.

After desktop restart, all 15 counts passed again, along with sandbox insert/read/update/delete, filtered cosine/L2/dot vector search, and media download content verification. The large country index initially returned a commit timeout but completed in the background and is now ready.
