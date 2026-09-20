# Contributing to PacificDB

Thank you for helping improve PacificDB Community. Contributions to code,
tests, documentation, packaging, and issue triage are welcome.

By participating, you agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
For usage questions and project discussion, join the
[PacificDB Discord](https://discord.gg/67w8ET9Sf2). Do not disclose security
vulnerabilities in Discord or public issues; follow [SECURITY.md](SECURITY.md).

## Before opening a change

1. Search the [issue tracker](https://github.com/hitesh-reddy-k/pacificdb-community/issues)
   for an existing report or proposal.
2. Open an issue for substantial behavior, protocol, storage-format, or public
   API changes so the scope can be agreed before implementation.
3. Keep each pull request focused on one change.

## Development setup

Follow the [Quick Start](README.md#quick-start) for installation and the
[build and test instructions](README.md#build-and-test) for a source build.
The complete user documentation is available at
[pacificdb.in/docs.html](https://pacificdb.in/docs.html).

At minimum, run the checks relevant to your change. The primary local gate is:

```sh
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
scripts/test-community.sh build
```

SDK-only changes should also run their focused tests:

```sh
npm run test:npm
PYTHONPATH=sdk/python python -m pytest sdk/python/tests
mvn -f sdk/java/pom.xml test
```

## Pull requests

- Describe the problem and the chosen solution.
- Link related issues with `Fixes #<number>` when appropriate.
- Add or update tests for changed behavior.
- Update documentation when commands, configuration, APIs, or operational
  behavior changes.
- Do not commit generated build directories, credentials, customer data, or
  machine-specific configuration.
- Confirm applicable tests pass and disclose any test that could not be run.

Maintainers may request changes to preserve compatibility, durability,
security, or the documented Community product boundary.

## Licensing

Contributions to the engine, query intelligence, and deployment code are
accepted under AGPL-3.0. Contributions to the CLI and SDK directories are
accepted under Apache-2.0, matching their existing licenses. By submitting a
contribution, you confirm that you have the right to provide it under the
applicable repository license.
