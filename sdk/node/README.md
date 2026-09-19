<p align="center">
  <img src="https://raw.githubusercontent.com/hitesh-reddy-k/pacificdb-community/pacificdb-v1.0/site/assets/pacificdb-logo-symbol.png" width="112" alt="PacificDB logo">
</p>

# PacificDB Node.js client

Apache-2.0 client for the Community engine JSON protocol.

```js
import { PacificDBClient } from '@pacificdb/client';
const db = new PacificDBClient({ host: '127.0.0.1', port: 9000, database: 'app' });
await db.insert('users', { id: '1', name: 'Ada' });
console.log(await db.find('users', { name: 'Ada' }));
db.close();
```

The client reuses up to 16 persistent TCP/TLS connections by default. Set
`poolSize` from 1 through 32 to tune concurrency, call `await db.connect()` to
prewarm the full pool before latency-sensitive work, and call `db.close()`
when the client is no longer needed. Standalone and locally managed Community
engines serve up to 10,000 sequential requests per connection by default; tune
that lifecycle with `ENGINE_KEEPALIVE_MAX_REQUESTS` and
`ENGINE_KEEPALIVE_IDLE_MS` when required.

Insert a batch in one engine request and one WAL batch:

```js
const result = await db.insertMany('users', [
  { id: '2', name: 'Grace' },
  { id: '3', name: 'Linus' }
]);
console.log(result.inserted);
```

Store or replace any image, GIF, audio, or video bytes under a stable ID:

```js
import { readFile, writeFile } from 'node:fs/promises';

await db.putMedia('assets', 'hero', await readFile('hero.gif'), {
  contentType: 'image/gif', filename: 'hero.gif'
});
const media = await db.getMedia('assets', 'hero');
await writeFile('downloaded.gif', media.data);
```

Store and search vector data:

```js
await db.putVector('embeddings', 'hero-vector', [0.2, 0.8], {
  modality: 'image', assetId: 'hero'
});
const matches = await db.queryVector('embeddings', [0.2, 0.8], {
  k: 5, filter: { assetId: 'hero' }
});
```

For files larger than one engine request, stream bounded chunks through the
same replicated document path:

```js
const uploaded = await db.uploadMediaFile('videos', './demo.mp4');
await db.downloadMediaFile(uploaded.id, './downloaded.mp4');
```

`uploadMediaFile` computes the whole-file SHA-256 before publishing a manifest,
sends one Base64-safe chunk at a time, and supports explicit resume with
`{ resume: mediaId }`. PacificDB applies no total file-size limit; disk space,
request limits, and other machine resources still apply. `putMedia` and
`getMedia` remain available for Buffer-sized callers whose entire encoded
document fits in one request.

If the connection is interrupted after the engine assigns an ID, the method
throws the exported `MediaUploadError`. Its `uploadId`, `nextChunk`,
`receivedChunks`, `receivedBytes`, and `resumable` fields can be persisted and
passed back as `{ resume: error.uploadId }`. Resume uses the engine's durable
chunk index and sends only missing or uncertain chunks.

Export a manual backup, including all physical data files, as one checksummed
JSON document without buffering the full backup in memory:

```js
await db.exportBackup('backup_...', './backup.json');
```
