# PacificDB Node.js client

Apache-2.0 client for the Community engine JSON protocol.

```js
import { PacificDBClient } from '@pacificdb/client';
const db = new PacificDBClient({ host: '127.0.0.1', port: 9000, database: 'app' });
await db.insert('users', { id: '1', name: 'Ada' });
console.log(await db.find('users', { name: 'Ada' }));
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

Export a manual backup, including all physical data files, as one checksummed
JSON document without buffering the full backup in memory:

```js
await db.exportBackup('backup_...', './backup.json');
```
