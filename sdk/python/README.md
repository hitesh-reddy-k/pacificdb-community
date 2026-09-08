# PacificDB Python client

Apache-2.0 client for the Community engine JSON protocol.

```python
from pacificdb import PacificDBClient
db = PacificDBClient(database="app")
db.insert("users", {"id": "1", "name": "Ada"})
print(db.find("users", {"name": "Ada"}))
```
