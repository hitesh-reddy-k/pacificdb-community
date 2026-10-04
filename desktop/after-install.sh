#!/bin/sh
set -e
launcher=/usr/bin/pacificdb-workbench
executable='/opt/PacificDB Workbench/pacificdb-workbench'
# Keep the package's terminal launcher in sync with the desktop entry.
# electron-builder's default post-removal script unregisters this alternative.
if command -v update-alternatives >/dev/null 2>&1; then
  update-alternatives --install "$launcher" pacificdb-workbench "$executable" 100
else
  ln -sf "$executable" "$launcher"
fi
# Chromium's packaged sandbox helper needs these permissions on Linux systems
# that restrict unprivileged user namespaces. No database process runs as root.
if [ -f '/opt/PacificDB Workbench/chrome-sandbox' ]; then
  chown root:root '/opt/PacificDB Workbench/chrome-sandbox'
  chmod 4755 '/opt/PacificDB Workbench/chrome-sandbox'
fi
update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
