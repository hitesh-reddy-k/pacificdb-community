# PacificDB v1.1.1 Release

## Release Summary

**Status: released with documented Python and Java registry gaps.**
This release provides database-first CLI and Workbench flows, simpler Java/Python
SDK entry points, safe legacy transport handling, and a corrected Node connection
pool. On 4 October 2026 the release owner explicitly accepted the missing
independent review, documented batch-10 regression, and unsigned Windows/macOS
artifacts for v1.1.1. That decision does not turn unavailable evidence into a
PASS and does not relax signing requirements for later versions.

The release checkout is `/home/hitesh/pacificdb-release-1.1.1`, branch
`v-1.1.1`. The recovered Downloads mount was not used for writes.
See [engineering analysis](CORE_ENGINE_OPTIMIZATION_1.1.1.md),
[execution record](RELEASE_1.1.1_EXECUTION.md) and
[raw release evidence](../../benchmarks/results/v1.1.1/).

## Published Components

The table records the publication targets and the independently verified public
state on 4 October 2026.

| Component | Previous public version | New version | Registry/distribution | Status |
|---|---|---|---|---|
| Engine and native CLI | 1.0.0 | 1.1.1 | [GitHub release](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/v1.1.1), GHCR | Published; checksums, manifest, Linux version and GHCR digest verified |
| Node SDK `@pacificdb/client` | 1.0.0 | 1.1.1 | npm | Published as `latest`; clean install and real CRUD verified |
| Node CLI `@pacificdb/cli` | 1.0.0 | 1.1.1 | npm | Published as `latest`; clean install and `--version` verified |
| Python `pacificdb` | No public version verified | 1.1.1 | PyPI / tagged source | PyPI not published: run `37191728049` passed qualification but failed OIDC with `invalid-publisher`; tagged-source install verified |
| Java `io.pacificdb:pacificdb-client` | No public version verified | 1.1.1 | Maven Central / tagged source | Central not published: run `37191730662` passed qualification but release credentials were absent |
| PacificDB Workbench | Linux 1.0.1 prerelease | 1.1.1 | [GitHub desktop release](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/workbench-v1.1.1) | Published; all five checksums and bundled engine/CLI versions verified |
| Helm deployment chart | Published chart version not independently verified | 1.1.1 | Repository chart | Lint, schema and fail-closed production checks pass |
| Website | Public stable 1.0.0 links | 1.1.1 pages | [pacificdb.in](https://pacificdb.in/) | Published; homepage, docs and release page return HTTP 200 |

The configured npm workflow published both exact qualified tarballs. Their public
integrity values and SHA-256 hashes match the local inspected bytes. Python/Maven
publisher ownership or credentials and platform signing secrets were not
available. No credential value was exposed or written into an artifact.

## Installation

These public installation paths were checked from clean temporary environments:

```sh
npm install @pacificdb/client@1.1.1 @pacificdb/cli@1.1.1

python3 -m venv .venv
. .venv/bin/activate
python -m pip install --no-deps \
  'git+https://github.com/hitesh-reddy-k/pacificdb-community.git@v1.1.1#subdirectory=sdk/python'
```

The published npm client completed database creation, collection creation, insert,
find and close against the downloaded 1.1.1 release engine. The public Python tag
install reports version 1.1.1. The exact website-example harness also installs the
qualified Python wheel and compiles Java with `--release 11` against the release
JAR, but those local artifacts are not represented as public registry packages.

Native package extraction/install checks are performed by
`scripts/test-native-package.sh` and `scripts/test-debian-container-install.sh`.
Do not overwrite a production installation or data root during qualification.
The portable Workbench launch uses its own new test data directory as documented
in [Workbench guide](../WORKBENCH.md).

Minimum client requirements: Node 18, Python 3.10, Java 11. Workbench uses its
bundled Electron runtime and engine; native installer ABI requirements are those
recorded in the actual installer metadata, not inferred from a version badge.

## Package Usage

The website's complete examples are executable checks, including CRUD, vectors,
file transfer, byte equality and close. See [Node](../../sdk/node/README.md),
[Python](../../sdk/python/README.md), [Java](../../sdk/java/README.md) and
[CLI](../../cli/README.md) for the actual API surface and error handling.

Node uses `PacificDB.fromUrl(url)` for lazy connection and
`await PacificDB.connect(url)` for eager connection. Python uses
`PacificDB.connect(url)` with a context manager. Java uses
`PacificDB.connect(url)` with try-with-resources. A URL selects the database;
create calls are explicit and should be omitted for existing namespaces.
Node `find` returns the original response; do not invent a `findOne` convenience
method from another language. Python/Java expose their checked `find_one`/`findOne`
helpers. All three support document, vector and media operations and bounded
connection pools. No interrupted write is silently retried.

Use a private environment variable for credential-bearing URLs. `pacificdbs://`
uses verified TLS; plain TCP authentication does not encrypt credentials.
Authentication/RBAC and invalid URL behavior were checked separately from the
unauthenticated loopback performance experiments. URL parser fixtures are shared
across SDKs and native CLI.

## Workbench v1.1.1

The [public Workbench release](https://github.com/hitesh-reddy-k/pacificdb-community/releases/tag/workbench-v1.1.1)
contains
`PacificDB-Workbench-1.1.1-linux-amd64.deb` and
`PacificDB-Workbench-1.1.1-linux-x64.tar.gz`, plus Windows x64 and macOS
ARM64/x64 installers. Run `37191983730` built and exercised each platform before
publication. A clean download of every public asset passed `SHA256SUMS`; the
extracted Debian package reports version 1.1.1 and both bundled native binaries
report 1.1.1.
The included native engine starts with an isolated owned data root and shuts down
with the application. Connections to remote engines use the actual host, port,
TCP/TLS and authentication settings; the Workbench is not a distributed cluster
orchestration product.

Normal flow: server → Workbench connection/authentication → database → collection
→ documents, vectors, media and metadata. Mandatory project selection and project
commands/screens are removed from the ordinary UX. Legacy project API/data remain.
A collection can be created, queried, paged, edited and deleted directly. Invalid
JSON and failed server calls surface errors without replacing successful state.

### Previous Workbench vs v1.1.1

| Area | Public Linux 1.0.1 preview | 1.1.1 | Evidence/impact |
|---|---|---|---|
| Navigation | Project-dependent ordinary flow | Database-first flow | Source and browser/native E2E; fewer required selections |
| Connections | Existing host/protocol setup | Shared URL-compatible selection | Native/npm TCP/TLS contract checks |
| Editor/query | Prior preview baseline | Direct-database CRUD and retained tools | Browser CRUD, editor, query history and pagination checks |
| Media/vector | Existing preview capabilities | Retained with database-first scope | Exact file transfer and vector tests |
| Startup/shutdown | Published preview artifact | Fresh bundled-engine lifecycle checks | Sandbox/startup/restart passed; comparative startup speed not measured |
| Performance | No matched preview UI timing baseline | Functional ready-time samples only | Do not claim a Workbench speedup from two launches |
| Packaging | Linux prerelease artifacts | Linux installer/archive plus Windows/macOS installers | Actual sandbox-enabled packaged apps tested in hosted run `37176501189`; release owner authorized unsigned v1.1.1 artifacts |

### Troubleshooting

On unpacked Linux archives, Electron's SUID helper must be owned by root with mode
4755. The `.deb` post-install script configures it. For a checksum-verified local
archive, follow the scoped helper commands in the Workbench guide. Do not disable
the sandbox. A root already owned by another live engine remains protected; under
the startup lock the launcher waits for the owner rather than racing startup.
Use a new disposable data directory for testing. Failed credentials, unavailable
servers and malformed connection URLs must be corrected, not bypassed.

## Website Changes

Updated `site/index.html`, `site/docs.html`, and added `site/release-1.1.1.html`.
README links point to package and Workbench guides. The homepage, documentation,
and v1.1.1 release page now describe the stable update and link to 1.1.1 engine,
SDK, and Workbench artifacts. The public release page contains update information,
not benchmark comparison copy. Static asset/anchor checks and exact complete SDK
examples pass. Pages run `37191954543` deployed the release branch; the homepage,
documentation and release notes returned HTTP 200 and contained v1.1.1 with no
removed comparison copy.

## Compatibility

Storage format 2 and Raft wire protocol 2 are unchanged. This release branch does
not contain the unreleased one-sync format-3 experiment or rejected fast paths.
Existing SDK constructors/raw protocol methods and legacy project metadata are
retained. Python typed errors and Java exceptions retain documented compatibility;
close/pool/TLS constraints are explicit. Public API compatibility is tested with
named dispatch coverage and authenticated client contracts; it is not a claim
that every historical application behaves identically.

## Upgrade Instructions

Take and verify a recoverable backup before upgrading valuable data.
Build/install into a disposable environment first. Record engine `--build-info`,
artifact digests and the actual previous version. The automated old-to-new RF3
check uses the exact published 1.0.0 Linux artifact, upgrades followers before the
leader, interrupts/restarts nodes and verifies convergence and acknowledged IDs.
Its scope is defined in the engineering document. Full application-history,
physical-power and new-to-old downgrade certification remain separate.
Do not reuse a production data root for benchmark or fault injection.

## Breaking Changes

Ordinary CLI/Workbench project commands and mandatory project selection are
removed, as explicitly requested. Scripts depending on those UI/CLI commands
must use database commands or retained legacy SDK/raw APIs. Legacy stored project
metadata is not deleted. New safety limits and errors are documented per SDK;
this is not marketed as an entirely behavior-identical UX upgrade.

## Known Issues

Independent physical-power/security evidence remains unavailable. The release
owner explicitly accepted those missing external checks and the documented
batch-10 regression for v1.1.1. Windows/macOS engine and Workbench artifacts are
unsigned under exact-version workflow exceptions. PyPI trusted-publisher
registration is absent, and Maven Central signing/token credentials are absent.
Consequently those two registry coordinates are not published; the website uses
verified tagged-source installation instructions. The default branch was not
protected; secret scanning/push protection were reported disabled. Full npm
build-tool audit has an unpatched `http-cache-semantics` advisory propagated
through the Electron build dependency chain; runtime-only npm audit has zero
reported advisories. The release branch updates the Java SDK from Jackson 2.18.9
to the first patched version, 2.18.10, and the rebuilt package/installed-client
suites pass. GitHub's three Jackson alerts remain open against the default branch
until that fix is merged; `security-advisories.json` records the alerts and exact
verification. GitHub also warns that the pinned `actions/checkout@v4`,
`actions/upload-artifact@v4` and container build actions target deprecated
Node.js 20; the hosted runner forced Node.js 24 and the jobs passed. Do not
describe those as eight unrelated runtime CVEs.
See the retained raw audit JSON for precise dependency data. A shared laptop and
finite datasets limit any performance generalization. Missing measurements are
listed in the engineering report. The repeatable batch-10 throughput regression
(756.8 → 237.5 calls/s by median) is now causally profiled. A separate 12-trial,
engine-only diagnostic used the same 1.1.1 client for both engines. At
concurrency 8, v1.1.1 reported 25.992 ms mean request lock wait per call by
median versus 0.480 ms in 1.0.0 and stayed near its concurrency-1 throughput.
Source comparison identifies the material lock-scope difference: v1.1.1
deliberately holds the per-collection lock across
durable WAL completion to keep WAL and in-memory apply order aligned. Removing
the ordering guard without an independently verified ordered-apply replacement
would trade correctness for a benchmark number, so the regression remains a
documented release trade-off rather than an unexplained result.
The release-owner disposition is recorded in the execution contract. The
ordered-apply optimization remains future work rather than being hidden or
replaced by an unsafe lock-scope change.

## Validation

The completed matched benchmark contains 108 standalone trials and six RF3 trials,
all with timed-call success plus post-run integrity/SIGKILL recovery. Read median
throughput was 3,850.9 → 19,990.6 calls/s and low-cardinality index rebuild was
4.9 → 21.6 calls/s. The release also records regressions: batch-10 was
756.8 → 237.5 calls/s, batch-1,000 was 35.7 → 32.4 calls/s, delete P99 rose from
20.859 to 23.888 ms, and mixed P99 rose from 10.822 to 13.135 ms. RF3's median
paired change was +4.82%, but a severe baseline outlier makes that too variable
for a headline RF3 performance claim.

The batch-lock diagnostic adds 12 fresh-data trials (two engines, concurrency 1
and 8, three alternating runs), using 100 calls of ten 1 KiB documents per
trial with fsync enabled. Every trial verified the exact count before and after
SIGKILL. Raw responses, engine logs and `summary.json` are retained under
`batch-lock-diagnostic-final/`.

The local Linux build, 58-case comprehensive engine suite, WAL/checkpoint crash
tests, authenticated E2E, RF3 partition/quorum/recovery, mixed-version upgrade,
Node/Python/Java SDKs, browser Workbench and packaged desktop lifecycle have
executed successfully. The final matrix distinguishes those local passes from
hosted platform CI, unavailable external certification and public artifact
verification. A passing build is not publication. Previous failed attempts and
their corrections remain in the evidence trail, including SDK pool races, runner
fixtures, build identity and hosted Windows/macOS/browser failures.

After the dependency review, the Java SDK was rebuilt with Jackson 2.18.10:
29 Maven tests passed, all three Jackson modules resolve to 2.18.10, and the
packaged Java consumer plus the cross-package release examples passed again.

The corrected Workbench workflow passed Linux, Windows, macOS ARM64 and macOS
Intel at source `03289fb95999462222973e04d8869827fc1a98c7` in hosted run
`37176501189`. Those hosted artifacts validate packaging and behavior; the
v1.1.1 release remains explicitly unsigned.

The final exact-tag Workbench run `37191983730` passed Linux, Windows, macOS
ARM64, macOS x64 and release publication. All five downloaded installers/archive
match the public checksum file. The Linux Debian package SHA-256 is
`1ecb3e96b9758819082f419ff09988890b872d25577bf66f1f40acedc515e54b`.

Hosted release-installer run `37176502323` also passed Linux, Windows, macOS
ARM64, macOS x86_64 and the non-root/read-only container smoke test at that exact
source. Downloaded P0 evidence verifies install/version, protocol failures,
discovery, lifecycle, recovery, 100 MiB media, sustained writes and uninstall
behavior where applicable. Branch validation intentionally skipped publishing
and treated signing/notarization as not applicable. The later release tag uses
the recorded exact-version unsigned exception.

Public native release run `37181644375` succeeded. `SHA256SUMS` verified every
downloaded asset, the extracted Linux CLI/engine both reported 1.1.1, and GHCR
tag `1.1.1` resolved to digest
`sha256:769d8d493775be49da8fee1687cc4aa58e104f473f523f159f6f104f0651c909`.
Npm run `37181644291` published both packages; their public tarballs exactly match
the inspected local SHA-256 values, and a clean public install passed real CRUD.

The raw inventory, method/configuration, per-run metrics, resource counters and
summaries are collected in `benchmarks/results/v1.1.1/`. See the engineering
document for complete tables, variability and limitations.

## Release Artifacts

The artifact manifest records each actual path, size, SHA-256 and embedded engine
identity. No checksum is asserted for an unbuilt/unuploaded object. Release
artifacts include npm tarballs, wheel/sdist, Java main/sources/javadoc JARs, Linux
native installer, Workbench installer/archive and a local non-root OCI image.
Hosted qualification artifact IDs, archive digests and expiry dates are recorded in
`benchmarks/results/v1.1.1/hosted-runs.json`; downloaded P0 JSON is retained under
`hosted-p0/`.
Public engine/native artifacts are at the `v1.1.1` GitHub release. Public npm
identifiers are `@pacificdb/client@1.1.1` and `@pacificdb/cli@1.1.1`; their
SHA-256 values are respectively
`80a1dccd8ead55bc1a192cdbabf26a254bd22dac861f7a827f02f8b7a90d5ff9` and
`d471aaf61de5731e948a84a293fadafa3e490b8dbf60efc922d734be7e094e05`.
The native `RELEASE-MANIFEST.json` SHA-256 is
`7aaadf9f7feb2053fd6e29f8656c24a9a8e2c2f7227caddbb7b6f6a4afe1cef9`.
Workbench assets and `SHA256SUMS` are at the `workbench-v1.1.1` release; the
checksum file SHA-256 is
`c2dc8c74b7ab06007d2e6df17b5b3a23b5c0c9f3360f802d02b780be417cd21d`.
Python and Maven Central remain explicitly absent rather than inferred as
published from local files.
