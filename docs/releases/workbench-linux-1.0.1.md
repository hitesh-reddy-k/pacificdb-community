PacificDB Workbench 1.0.1 is a public **Linux x86-64 preview**. The installer includes the desktop interface, database engine, native CLI, and runtime. It uses local storage and does not require an account or separate server.

Download `PacificDB-Workbench-1.0.1-linux-amd64.deb` and `SHA256SUMS`, verify the checksum, then install:

```sh
sha256sum -c SHA256SUMS
sudo apt install ./PacificDB-Workbench-1.0.1-linux-amd64.deb
```

Open **PacificDB Workbench** from the Applications menu or run `pacificdb-workbench`. Data is stored under `~/.config/PacificDB Workbench/database` and persists across launches. The `.tar.gz` archive is also available for portable use; the `.deb` configures the application launcher and Chromium sandbox helper.

The Linux release workflow builds on Ubuntu 24.04, tests the packaged application, installs the `.deb`, and tests the installed application against its bundled engine. Local checks also covered 500,000-record indexing and upgrade from the published 1.0.0 engine.

This preview has **not** passed the repository's full production release qualification. Independent security review and physical power-interruption evidence for this revision remain outstanding. Windows and macOS installers are not included. Do not expose the local Workbench bridge or its unauthenticated desktop engine as a public service.
