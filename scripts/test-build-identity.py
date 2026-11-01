#!/usr/bin/env python3
"""Verify source-archive builds preserve explicitly supplied release identities."""
import shutil
import subprocess
import tempfile
import os
from pathlib import Path

root = Path(__file__).resolve().parents[1]
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
with tempfile.TemporaryDirectory(prefix='pacificdb-build-identity-') as directory:
    temporary = Path(directory)
    source = temporary / 'engine'
    shutil.copy(root / 'LICENSE', temporary / 'LICENSE')
    shutil.copy(root / 'release.json', temporary / 'release.json')
    shutil.copytree(root / 'engine', source, ignore=shutil.ignore_patterns('build', '__pycache__'))
    for name, supplied, expected in [('explicit', revision, revision), ('unavailable', None, 'unknown')]:
        build = temporary / name
        command = ['cmake', '-S', str(source), '-B', str(build)]
        if os.environ.get('CMAKE_TOOLCHAIN_FILE'):
            command.append('-DCMAKE_TOOLCHAIN_FILE='+os.environ['CMAKE_TOOLCHAIN_FILE'])
        if supplied:
            command.append(f'-DPACIFICDB_GIT_COMMIT={supplied}')
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        header = (build / 'generated/build_identity.hpp').read_text()
        assert f'#define PACIFICDB_GIT_COMMIT "{expected}"' in header, (name, header)
        assert '#define PACIFICDB_ENGINE_VERSION "1.2.0"' in header, header
        assert '#define PACIFICDB_RELEASE_VERSION "1.2.0-rc.1"' in header, header
        assert '#define PACIFICDB_RELEASE_CHANNEL "prerelease"' in header, header
print('BUILD_IDENTITY_PASS: explicit archive revision retained; absent metadata stays unknown')
