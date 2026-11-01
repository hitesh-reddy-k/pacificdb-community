# Desktop installer findings retained for the company preview

These are scoped observations, not a promise of error-free desktop operation.
The engine tag remains `v1.2.0-rc.1` at
`c553c6b6c62702f494f6f95acb7d53c6a410068e`.

## Ubuntu24 installer sandbox startup

The first hosted Linux publication attempts failed before opening the Workbench:

```text
LaunchProcess: failed to execvp:
/opt/PacificDB
Failed to receive boot message from zygote
```

The packaged `chrome-sandbox` helper was already root-owned with mode4755.
The custom post-install script had replaced electron-builder's upstream script,
omitting its application-scoped AppArmor profile installation. Ubuntu's user
namespace restrictions caused the Chromium SUID fallback to encounter the known
space-in-helper-path failure. See the upstream
[Electron issue](https://github.com/electron/electron/issues/44414) and
[Chromium explanation](https://chromium.googlesource.com/chromium/src/+/main/docs/security/apparmor-userns-restrictions.md).

The published Ubuntu24 package was built from the frozen RC source with an
explicit packaging override selecting the pinned electron-builder upstream
`templates/linux/after-install.tpl`. Its platform receipt records the override,
script SHA-256 and packaging workflow revision. It installs the application's
userns profile, retains the Chromium sandbox and does not change global AppArmor
settings. The development branch now uses that default post-install behavior
directly. No database binary or benchmark executable was replaced by this fix.

- Failed diagnostic runs:
  [41315800782](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41315800782),
  [41316288786](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41316288786).
- Successful build, actual apt installation and sandboxed GUI/CRUD/media/restart
  test, followed by publication:
  [41316942083](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41316942083).
- Public Ubuntu24 installer SHA-256:
  `91e57b7cea5ef4911eae1cbd342d26f11bf0c2f34b596c1774150411480d0dee`.

## Independent public-install tests

The first fresh public download/install run
[41317560421](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41317560421)
verified all four public installer hashes. Linux and macOS Intel completed the
GUI, document/media, restart and CLI backup/restore flow. Windows and macOS ARM
timed out clicking the new collection control because another sidebar element
intercepted pointer events. These failures are retained and require investigation;
earlier upgrade/install passes do not erase them.

Two additional verification-harness problems were found: the receipt step used a
staging-only helper that expected a local build tree, and the Linux SDK test
requested vector schemas without enabling the documented engine preview flag.
The verifier now records installed-package evidence directly, enables the
explicit opt-in for vector-specific tests, and captures GUI screenshots/DOM/hit
geometry on failure. Assertions have not been removed or force-clicked.

The diagnostic rerun is
[41317973346](https://github.com/hitesh-reddy-k/pacificdb-community/actions/runs/41317973346).
Its unexecuted or unsuccessful platform checks must not be described as passed.

## Signing and deployment scope

These are company-testing prerelease installers. Stable Windows signing,
macOS Developer ID/notarization, browser-download quarantine/Gatekeeper policy,
all supported OS point releases and a complete mixed-version upgrade matrix
are not qualified by these CI tests. Organizational installation approval may
still be necessary. Linux native Debian13 assets and the Ubuntu24 Workbench
package target different runtime ABIs; use the matching package.
