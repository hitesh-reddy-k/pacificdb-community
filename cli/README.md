# PacificDB CLI

The Apache-2.0 Community CLI talks directly to a PacificDB engine.

```sh
pacificdb ping --host 127.0.0.1 --port 9000
pacificdb request '{"action":"find","collection":"users","filter":{}}' --database app
pacificdb shell --database app
```

The shell accepts friendly commands and retains raw JSON compatibility through
`request <json>` or a bare JSON object. Run `help` to see Authentication,
Projects, Databases and queries, Backups, Security/API keys, Media, Vectors,
and System commands.

```text
pacificdb> create project demo
pacificdb> use project project_...
pacificdb> create database app
pacificdb> use app
pacificdb> create collection users
pacificdb> insert users {"id":"1","name":"Ada"}
pacificdb> findOne users {"id":"1"}
```

Media and vector commands:

```sh
pacificdb put-media assets hero ./hero.gif --content-type image/gif --database app
pacificdb get-media assets hero ./downloaded.gif --database app
pacificdb put-vector embeddings hero-vector '[0.2,0.8]' --metadata '{"modality":"image"}' --database app
pacificdb query-vector embeddings '[0.2,0.8]' --k 5 --database app
```

Running `put-media` again with the same ID replaces the stored bytes and
metadata.

The interactive shell uses bounded, checksummed chunks for media files:

```text
pacificdb> upload video ./demo.mp4 --collection videos
pacificdb> list media
pacificdb> download media media_... ./downloaded.mp4
```

PacificDB applies no total file-size limit to this chunked path. Available disk
space, per-request limits, and other machine resources still apply.
