# PacificDB Node.js client

Apache-2.0 client for the Community engine JSON protocol.

```js
import { PacificDBClient } from '@pacificdb/client';
const db = new PacificDBClient({ host: '127.0.0.1', port: 9000, database: 'app' });
await db.insert('users', { id: '1', name: 'Ada' });
console.log(await db.find('users', { name: 'Ada' }));
```
