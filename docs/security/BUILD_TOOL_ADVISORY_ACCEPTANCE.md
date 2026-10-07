# Time-limited build-tool exception

Reviewed 2026-10-07; expires at 2026-11-06 00:00 UTC. Owner: PacificDB release
maintainer. This repository policy implements the requested documented
acceptance; it is not independent security approval or a runtime waiver.

The current full npm audit has eight moderate affected package entries caused
by one underlying [sprintf-js advisory](https://github.com/advisories/GHSA-hp3w-g68c-fv3c).
The advisory lists no patched version. The lockfile dependency path is
electron-builder -> app-builder-lib -> @electron/get -> global-agent -> roarr ->
sprintf-js (other builder entries inherit the same issue). All affected nodes
are `dev: true`. Runtime npm audit must remain at zero.

Risk: attacker-controlled logging format strings can abort a build operation.
This is not an assertion that every build input is safe. Do not expose a release
build service to untrusted inputs, use contributor code with release secrets,
or pass user-provided format strings to the build logger. Run isolated builds
from reviewed source with pinned dependencies and `npm ci`. The packaged
application must exclude builder/proxy/logger packages; its runtime test checks
the ASAR inventory, source revision and bundled binary hashes.

`build-tool-advisory-exceptions.json` accepts only this advisory, its listed
versions, moderate severity and development-only lockfile entries. Run
`npm run test:build-tools` for fresh full/runtime audits, or provide saved JSON
to `scripts/check-build-tool-advisories.mjs`. CI and all desktop platform jobs
run this gate. Runtime findings, new advisories/packages/severities, changed
versions, malformed audit evidence and expiry fail closed. The regression
checks are `node --test scripts/test-build-tool-advisories.mjs`.

Do not run an automatic forced downgrade just to turn the audit green. Replace
the vulnerable dependency path once a reviewed compatible fix is available,
then rerun native packaging and upgrade tests. Re-review this exception before
expiry; it must not silently renew. Signed releases and operational risk
acceptance still require the release owner's normal approval process.
