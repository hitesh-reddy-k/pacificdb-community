# PacificDB CLI

The Apache-2.0 Community CLI talks directly to a PacificDB engine.

```sh
pacificdb ping --host 127.0.0.1 --port 9000
pacificdb request '{"action":"find","collection":"users","filter":{}}' --database app
pacificdb shell --database app
```

The shell accepts one JSON command per line.
