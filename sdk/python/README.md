<p align="center">
  <img src="https://raw.githubusercontent.com/hitesh-reddy-k/pacificdb-community/pacificdb-v1.0/site/assets/pacificdb-logo-symbol.png" width="112" alt="PacificDB logo">
</p>

# PacificDB Python client

Apache-2.0 client for the Community engine JSON protocol.

## Quick start

Start a local PacificDB instance and create a project and database using the
PacificDB CLI:

```text
create project demo
use project <project-id>
create database app
use app
```

Install the Python client from the repository:

```bash
python -m pip install ./sdk/python
```

Then connect to the `app` database and perform basic CRUD operations:

```python
from pacificdb import PacificDBClient

db = PacificDBClient(database="app")

db.create_collection("users")

db.insert("users", {"id": "1", "name": "Ada"})
print(db.find("users", {"id": "1"}))

db.update_one("users", {"id": "1"}, {"name": "Ada Lovelace"})
print(db.find("users", {"id": "1"}))

db.delete_one("users", {"id": "1"})
print(db.find("users", {"id": "1"}))
```

The Python client connects to the local engine at `127.0.0.1:9000` by default.

For more information about building and running PacificDB, see the main repository README.
