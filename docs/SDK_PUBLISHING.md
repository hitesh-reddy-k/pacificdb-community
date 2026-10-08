# SDK package publication

The current source and published GitHub download assets are version **1.1.2**:
`pacificdb==1.1.2` and `io.pacificdb:pacificdb-client:1.1.2`.
The [Community prerelease](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/1.1.2)
contains the inspected Python wheel and Java binary/source/Javadoc JARs.
Immutable tag `1.1.2` includes the reviewed Jackson 2.18.11 dependency.
Neither coordinate has been published as 1.1.2 to PyPI or Maven Central.
PyPI needs trusted-publisher registration; Central needs owner credentials,
signing and explicit portal action. Never overwrite or claim ownership without
release-owner verification and a coordinated version.

Build and inspect locally:

```sh
python -m pip install build==1.2.2.post1 twine==6.1.0 setuptools==80.9.0 pytest==8.3.5
python -m build sdk/python
python -m twine check sdk/python/dist/*
scripts/test-python-installed-client.sh sdk/python/dist/*.whl
mvn -B -f sdk/java/pom.xml install
scripts/test-java-installed-client.sh
```

Python ships no runtime dependency. Java ships Jackson only, binary/sources/
Javadoc JARs and Apache license metadata. Maven uses `release=11`. Both clients
support direct database URLs; the examples in each README use no project.
Set `PACIFICDB_TEST_ENGINE_BUILD` to an absolute build directory to include
installed-package authenticated TCP/TLS and exact-value SIGKILL verification.

The `central` Maven profile signs during `verify`, has `autoPublish=false` and
`central.skipPublishing=true` by default. Ordinary build/test/package/install
never uploads. In the locally tested plugin 0.11.0, skip mode required a dummy
server settings entry and then skipped artifact staging, producing no ZIP,
despite the [documented bundle-only behavior](https://central.sonatype.org/publish/publish-portal-maven/).
The preserved diagnostic logs record this difference. `scripts/pack-java-sdk.py`
therefore verifies signatures and constructs a checksummed Central ZIP from
already-built bytes without an upload or rebuild. The local bundle is signed
with a disposable **test-only** identity. It cannot substitute for the release
owner's signing identity. Its private keyring was removed; the test public key
is retained as local evidence.

For a local signed bundle, provide a private temporary GPG keyring and its exact
40-hex primary fingerprint, copy the POM alongside the JARs, then run:

```sh
cp sdk/java/pom.xml sdk/java/target/pacificdb-client-1.1.2.pom
python scripts/pack-java-sdk.py --target sdk/java/target \
  --output sdk/java/target/central-publishing/central-bundle.zip \
  --fingerprint "$GPG_FINGERPRINT" --sign
python scripts/verify-sdk-packages.py --python-dist sdk/python/dist \
  --java-target sdk/java/target --output build/sdk-package-inspection.json
```

`--manifest` on the packer checks hashes before release signing. The artifact
verifier checks archive inventories, metadata, license, Python RECORD hashes,
Java 11 bytecode, public sources/Javadoc, detached signatures and bundle
checksums. The packer cryptographically verifies signatures against the exact
fingerprint. No unsigned artifact is accepted as a valid Central bundle.

Publication workflows run only for an existing semantic release tag or explicit
manual dispatch naming that tag. They first run the SDK runtime matrix, installed
smoke and real-engine checks, then hash inspected artifacts. Publish jobs
consume those exact bytes and refuse a tag/version/revision/hash mismatch.

Before enabling them, the release owner must:

- Verify PyPI project ownership and register the exact repository/workflow/
  environment as a [trusted publisher](https://docs.pypi.org/trusted-publishers/adding-a-publisher/).
- Configure the `pypi` and `maven-central` GitHub environments with required
  reviewers, deployment tag restrictions and prevention of self-review where
  available. Merely naming an environment does not establish those protections.
- Verify the `io.pacificdb` Central namespace, publish the release signing public
  key, and configure `GPG_PRIVATE_KEY`, `GPG_PASSPHRASE`, `GPG_FINGERPRINT` and
  `CENTRAL_TOKEN` only for the protected Central job. The token is the portal's
  Base64-encoded user-token username/password pair, never printed.
- Confirm version availability, external engine release gates and final approval
  before creating a release tag or dispatching publication.

The Python job uses job-scoped OIDC and the pinned official PyPI action;
[PyPI's publisher documentation](https://docs.pypi.org/trusted-publishers/using-a-publisher/)
explains the registered identity. No long-lived PyPI secret is used. The Java job
signs the tested bytes with the configured release-owner key and uses the
[Central validation API](https://central.sonatype.org/publish/publish-portal-api/)
with explicit `USER_MANAGED`. It stops at `VALIDATED`; publication requires a
separate owner action in the portal. The uploader contains no publish endpoint.

## Historical v1.1.1 hosted results (not 1.1.2 qualification)

The v1.1.1 Python workflow was executed across its Linux/Windows runtime matrix;
artifact qualification passed and publication stopped at the missing PyPI
publisher mapping. The Java workflow passed the same matrix and exact-artifact
qualification, then stopped before bundle signing/upload because the owner GPG
identity and Central token were absent. Neither failure is represented as a
registry publication.
Independent security review and physical power-loss evidence were not performed.
See [release evidence](RELEASE_EVIDENCE.md).
