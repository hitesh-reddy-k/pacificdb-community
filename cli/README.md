# PacificDB CLI

The Apache-2.0 Community CLI talks directly to a PacificDB engine.

```sh
pacificdb ping --host 127.0.0.1 --port 9000
pacificdb request '{"action":"find","collection":"users","filter":{}}' --database app
pacificdb shell --database app
```

The shell accepts one JSON command per line.

Media and vector commands:

```sh
pacificdb put-media assets hero ./hero.gif --content-type image/gif --database app
pacificdb get-media assets hero ./downloaded.gif --database app
pacificdb put-vector embeddings hero-vector '[0.2,0.8]' --metadata '{"modality":"image"}' --database app
pacificdb query-vector embeddings '[0.2,0.8]' --k 5 --database app
```

Running `put-media` again with the same ID replaces the stored bytes and
metadata.
