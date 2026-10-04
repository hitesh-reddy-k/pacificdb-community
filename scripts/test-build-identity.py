#!/usr/bin/env python3
"""Verify source-archive builds preserve explicitly supplied release identities."""
import shutil
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
with tempfile.TemporaryDirectory(prefix='pacificdb-build-identity-') as directory:
    temporary = Path(directory)
    source = temporary / 'engine'
    shutil.copy(root / 'LICENSE', temporary / 'LICENSE')
    shutil.copytree(root / 'engine', source, ignore=shutil.ignore_patterns('build', '__pycache__'))
    for name, supplied, expected in [('explicit', revision, revision), ('unavailable', None, 'unknown')]:
        build = temporary / name
        command = ['cmake', '-S', str(source), '-B', str(build)]
        if supplied:
            command.append(f'-DPACIFICDB_GIT_COMMIT={supplied}')
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        header = (build / 'generated/build_identity.hpp').read_text()
        assert f'#define PACIFICDB_GIT_COMMIT "{expected}"' in header, (name, header)
print('BUILD_IDENTITY_PASS: explicit archive revision retained; absent metadata stays unknown')
