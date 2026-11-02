# RC.2 current company-testing delivery

Version `1.2.0`, candidate/tag `v1.2.0-rc.2`, source
`e3d70761f03341acc13130d17b75fed2bc758a29`. RC.1 stays published unchanged.
RC.2 is a public **prerelease**, not latest/stable or production-ready.

| Activity | Status | Evidence/scope |
| --- | --- | --- |
| Short-window GUI regression | Passed | Original pointer-overlap failure reproduced, then650/653/900 px real clicks passed |
| Canonical version consistency | Passed | Engine/CLI/Workbench/Node/Java1.2.0-rc.2; Python1.2.0rc2 |
| Native checkpoint | Passed | [41320240384](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41320240384):78 checks / 50 CTest cases, real crash failpoints |
| SDK runtime/artifact qualification | Passed | Same run:16 SDK jobs, exact installed packages and live TCP/TLS/media/crash tests |
| Complete Community and actual disk-full | Passed | [41321365611](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41321365611):all 21 ENOSPC categories, RF3, CRUD/media/storage/auth/concurrency/SDK |
| Windows x64/macOS Intel/Apple Silicon installers | Passed | [41320240390](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41320240390):three successful exact-source jobs, fresh and upgrade scenarios; initial Linux infrastructure failure retained |
| Linux Ubuntu24 x64 installer and browser | Passed | [41321368699](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41321368699):successful Linux job, normal sandbox and short-window/browser/fresh/upgrade tests |
| Full/runtime dependency advisory checks | Passed | Zero findings, no exceptions; audits tied to the source revision in sealed evidence |
| RC.2 tag and sealed GitHub prerelease | Passed | [41322676130](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41322676130):25 artifacts plus manifest/checksums; remote tag dereferenced to exact source |
| Public RC.2 manifest/API integrity | Passed | Fresh public manifest/checksums matched; initial 25 artifact public digests/sizes matched; later additions have separate integrity checks |
| Public RC.2 desktop and SDK installation | Passed | [41324080656, attempt2](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41324080656/attempts/2):all four desktop packages, registry Node client/CLI, GitHub wheel/JAR/POM, live TCP/TLS/media/SIGKILL recovery; initial npm propagation failure retained |
| npm publication/registry installation | Passed | [41322694402](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41322694402):client/CLI1.2.0-rc.2 to next, independent public install; published CLI bytes match tested package |
| Public RC.2 GHCR image | Passed | [41322877469](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41322877469):pushed digest pull/runtime smoke, independent anonymous manifest/config identity |
| PyPI publication and installed client | Passed | [41336261556](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41336261556); independently downloaded public 1.2.0rc2 wheel/sdist; all contained files match GitHub packages, archive timestamps/digests differ; installed wheel passed 134 tests and live TCP/TLS/SIGKILL recovery |
| Maven Central publication | Blocked | Owner namespace/signing setup remains incomplete; GitHub JAR/POM available |
| RC.2 post-publication benchmark execution | Passed | 10K/100K/1M and supplements completed with zero controller exit codes; [report](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/BENCHMARK_REPORT_1.2.0-rc.2.txt) / [evidence](https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/v1.2.0-rc.2/pacificdb-benchmarks-1.2.0-rc.2-20261101.tar.gz); numerical/availability failures remain disclosed |
| 1M mixed strict ANN read availability | Failed | PacificDB0/360 successful reads versus PostgreSQL360/360 during concurrent mutation; correctness errors are explicit, availability target not met |
| Targeted sanitizer checks | Passed | [41327257515](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41327257515):separate TSAN and ASan/UBSan jobs over frozen RC.2 durability/request/vector tests; scoped, not all code |
| Full public artifact inventory | Passed | [41326682257](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41326682257):29 public assets including prepared benchmark inputs, manifest/checksums |
| Full stable release qualification | Not tested | Physical power loss, independent security, complete platform/mixed-version upgrade/distributed/advanced vector gates remain INCOMPLETE |

## Limits and publication dependencies

Unreleased follow-up (2026-11-02): bounded ANN freshness waits passed the local
78-check checkpoint, broader Community/disk-full/SDK regressions and three
targeted ThreadSanitizer checks. A separate published-RC.2-to-candidate rolling
RF3 upgrade and full restart passed; this is a scoped exact-artifact result.
The small mixed-write case achieved 360/360;
the published 1M failure above remains unchanged until that workload is rerun.
The Linux launcher icon packaging correction passed real GTK lookup and
packaged Workbench checks locally. These changes are not yet in released
packages. [Fresh 398316c desktop CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41454844337)
passed all four targets, and [full Community CI](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41455594933)
passed the frozen source. Those receipts do not qualify a stable release.

Python is published on PyPI. Maven Central remains Blocked pending
[owner setup](PUBLISHING_OWNER_GUIDE.md).
GitHub SDK packages remain available. Failed attempts and original logs are retained
in their CI runs/local evidence; a successful retry does not erase the original failure.
Physical power-loss, independent security, complete mixed-version upgrades and
complete distributed vector search remain unqualified. This prerelease is not stable.
