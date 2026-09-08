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

`putMedia` and `updateMedia` use PacificDB's replicated document write path.
Media is base64 encoded in the document, so this beta is intended for files
that fit within the configured request-size limit.
