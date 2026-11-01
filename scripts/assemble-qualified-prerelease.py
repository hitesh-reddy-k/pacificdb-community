#!/usr/bin/env python3
"""Seal CI-tested frozen artifacts. No network, Git ref changes or publication."""
import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path.cwd() / 'scripts'))
from prerelease_qualification import REQUIRED
from release_identity import load_release_identity
import importlib.util
spec = importlib.util.spec_from_file_location('seal', Path.cwd() / 'scripts/qualify-prerelease.py')
seal = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seal)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_receipt(receipt, revision, release, artifact):
    if receipt.get('revision') != revision or receipt.get('sourceDirty') is not False:
        raise ValueError('platform source identity/cleanliness mismatch')
    if receipt.get('release', {}).get('releaseVersion') != release:
        raise ValueError('platform candidate mismatch')
    if receipt.get('engineIdentity', {}).get('gitCommit') != revision:
        raise ValueError('platform native identity mismatch')
    if digest(artifact) != receipt.get('artifact', {}).get('sha256'):
        raise ValueError('platform artifact hash mismatch')


def assemble(root):
    root = root.resolve(strict=True)
    out, evidence = root / 'distribution', root / 'evidence'
    identity = load_release_identity()
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    release = identity['releaseVersion']
    manifest = seal.staging.verify(out, revision)
    for key in ['gitCommit', 'releaseVersion']:
        expected = revision if key == 'gitCommit' else release
        if manifest.get('buildInfo', {}).get(key) != expected:
            raise ValueError('staged engine identity mismatch: ' + key)
    checkpoint = json.loads((evidence / 'checkpoint/summary.json').read_text())
    if checkpoint.get('status') != 'PASS' or not checkpoint.get('media_crash_executed'):
        raise ValueError('complete checkpoint with failpoint recovery required')
    native = out / f'pacificdb-community-{release}-linux-x64.tar.gz'
    with tarfile.open(native) as archive:
        entries = [entry for entry in archive.getmembers() if entry.name.endswith('/bin/db_engine')]
        if len(entries) != 1:
            raise ValueError('one native engine required')
        if hashlib.file_digest(archive.extractfile(entries[0]), 'sha256').hexdigest() != checkpoint['engine_sha256']:
            raise ValueError('native distribution differs from checkpoint binary')
    community = json.loads((evidence / 'community/summary.json').read_text())
    for key, expected in [('status', 'PASS'), ('revision', revision), ('releaseVersion', release), ('sourceClean', True)]:
        if community.get(key) != expected:
            raise ValueError('complete Community receipt mismatch: ' + key)
    if digest(evidence / 'community/community-complete.log') != community['logSHA256']:
        raise ValueError('Community log changed')
    gate = json.loads((evidence / 'advisory-gate.json').read_text())
    if gate.get('status') != 'PASS' or gate.get('acceptedEntries') != 0 or gate.get('runtimeAdvisories') != 0:
        raise ValueError('zero-advisory gate required; no exceptions')
    for name in ['full-audit.json', 'runtime-audit.json']:
        if json.loads((evidence / name).read_text())['metadata']['vulnerabilities']['total'] != 0:
            raise ValueError('dependency findings remain')
    platforms = {}

    def add(path, platform):
        dest = out / path.name
        if dest.exists():
            raise ValueError('refusing duplicate staged artifact: ' + path.name)
        shutil.copy2(path, dest)
        manifest['artifacts'].append({'filename': path.name, 'sha256': digest(dest), 'sizeBytes': dest.stat().st_size})
        platforms[path.name] = platform

    platform_evidence = []
    for platform in ['linux-x64', 'windows-x64', 'macos-x64', 'macos-arm64']:
        directory = root / ('platform-' + platform)
        receipt_path = directory / ('workbench-' + platform + '-preview.json')
        receipt = json.loads(receipt_path.read_text())
        artifact = directory / Path(receipt['artifact']['path']).name
        check_receipt(receipt, revision, release, artifact)
        if receipt.get('platform') != platform:
            raise ValueError('incorrect platform label')
        add(artifact, platform)
        add(receipt_path, 'platform-receipt')
        copy = evidence / receipt_path.name
        shutil.copy2(receipt_path, copy)
        platform_evidence.append(copy)

    # Preserve raw CI evidence in an independently checksummed release artifact.
    validation = root / f'pacificdb-validation-{release}.tar.gz'
    with tarfile.open(validation, 'w:gz') as archive:
        for path in sorted(evidence.rglob('*')):
            if path.is_file():
                archive.add(path, arcname=path.relative_to(evidence))
    add(validation, 'portable validation evidence')
    source = root / f'pacificdb-source-{release}.tar.gz'
    raw = subprocess.check_output(['git', 'archive', '--format=tar', '--prefix=pacificdb-source-' + release + '/', revision])
    with tarfile.open(source, 'w:gz') as archive, tarfile.open(fileobj=io.BytesIO(raw)) as original:
        for entry in original.getmembers():
            archive.addfile(entry, original.extractfile(entry) if entry.isfile() else None)
    add(source, 'portable source; compiled binaries separately qualified')
    inventory = root / f'software-inventory-{release}.json'
    inventory.write_text(json.dumps({'release': identity, 'revision': revision, 'buildInfo': manifest['buildInfo'],
        'desktopPlatforms': list(['linux-x64', 'windows-x64', 'macos-x64', 'macos-arm64']),
        'nativeLinuxABI': 'Ubuntu24.04 x64; not Debian13 native ABI',
        'licenseScope': 'Engine AGPL-3.0-only; CLI/SDK Apache-2.0; redistribution notices in source/packages',
        'sbomScope': 'npm SBOMs only; this inventory is not a complete native dependency vulnerability audit'}, indent=2) + '\n')
    add(inventory, 'portable inventory')
    for label, options in [('build', []), ('runtime', ['--omit=dev'])]:
        sbom = root / f'npm-{label}-sbom-{release}.json'
        with sbom.open('w') as stream:
            subprocess.run(['npm', 'sbom', '--sbom-format=cyclonedx', *options], stdout=stream, check=True)
        json.loads(sbom.read_text())
        add(sbom, 'portable npm inventory')
    status = root / 'PUBLICATION_STATUS.md'
    status.write_text(f'''# Pre-release v1.2 — {release}

Source `{revision}`. Qualified for scoped company testing, not production.
Native Ubuntu24 x64 engine/CLI, Windows x64 and macOS Intel/Apple Silicon desktop
packages passed frozen-source CI. Workbench includes its own native engine/CLI.
Checksums, source, documentation and raw validation evidence are attached.
RC.1's confirmed short-window sidebar defect is fixed in RC.2 and fresh-data
GUI checks are separate from upgrade checks. Linux uses upstream AppArmor setup.

Native vector preview remains default-off. ANN graphs require reconstruction
after restart; strict ANN searches can be unavailable during mutations.
Distributed completeness, advanced disk/quantization/compaction, complete
mixed-version upgrade, physical power-loss, independent security and full stable
production qualification remain INCOMPLETE. Signing/notarization and browser
quarantine policy are not asserted by installed CI tests.

PyPI/Maven require owner setup; GitHub wheel/JAR/POM installation is available.
npm/GHCR publication requires separate workflow receipts and public install checks.
Registry availability must not be inferred from this GitHub distribution.
RC.1 benchmark results retain their original identity. RC.2 measured benchmarks
are separate and incomplete until their raw runs finish. No universal superiority,
identical timings on different hardware or zero-defect claim is made.

Follow INSTALLATION.md, SDK_GUIDE.md, COMPANY_TESTING_GUIDE.md and
PUBLISHING_OWNER_GUIDE.md in the documentation archive, with disposable data.
''')
    add(status, 'portable publication status')
    manifest['nativeLinuxABI'] = 'Ubuntu24.04 x64'
    manifest['distributionScope'] = 'external company testing; productionReady=false'
    manifest['desktopDistribution'] = 'Linux x64, Windows x64, macOS x64/arm64; normal sandbox GUI, fresh and upgrade paths tested'
    (out / 'RELEASE-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    # Seal checks with exact bytes of execution logs; no status-only qualification.
    sources = {
        'identity': [evidence / 'checkpoint/summary.json', evidence / 'native-run.json'],
        'engine': [evidence / 'checkpoint/summary.json'],
        'document-media': [evidence / 'community/summary.json'],
        'authentication': [evidence / 'checkpoint/summary.json'],
        'vector': [evidence / 'checkpoint/summary.json'],
        'cli': [evidence / 'native-run.json', evidence / 'community/summary.json'],
        'workbench': [evidence / 'platform-run.json', *platform_evidence],
        'installed-sdks': [evidence / 'native-run.json'],
        'artifact-integrity': [evidence / 'native-run.json', evidence / 'checkpoint/summary.json'],
        'security-blockers': [evidence / 'advisory-gate.json', evidence / 'full-audit.json', evidence / 'runtime-audit.json'],
        'disk-full-recovery': [evidence / 'community/summary.json', evidence / 'community/community-complete.log'],
    }
    checks = [{'name': name, 'status': 'Passed', 'evidence': [
        {'path': str(path.relative_to(evidence)), 'sha256': digest(path)} for path in sources[name]]} for name in REQUIRED]
    receipt = {'releaseVersion': release, 'revision': revision, 'checks': checks,
        'artifacts': [{'name': entry['filename'], 'sha256': entry['sha256'],
            'platform': platforms.get(entry['filename'], 'linux-x64' if 'linux-x64' in entry['filename'] else 'portable package/documentation'),
            'tested': True} for entry in manifest['artifacts']]}
    receipt_path = evidence / 'QUALIFICATION_RECEIPTS.json'
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    seal.qualify(out, receipt_path, revision)
    notes = (Path.cwd() / 'RELEASE_NOTES_v1.2.md').read_text()
    (root / 'RELEASE_NOTES.md').write_text(f'''# Pre-release v1.2 — {release}

This GitHub distribution passed scoped frozen-source package qualification.
Source `{revision}`. It is a company-testing prerelease, never latest/stable.
Linux native packages target Ubuntu24 x64; desktop packages include their engine
and native CLI. Windows x64, macOS Intel/Apple Silicon and Linux desktop packages
passed installation, ordinary sandbox, fresh/upgrade document/media/restart and
CLI backup/restore scenarios. Stable signing/notarization is not asserted.

PyPI/Maven remain owner-blocked. npm/GHCR publication and public installations
require their separate receipts. Read PUBLICATION_STATUS.md and verify SHA256SUMS.
RC.1 benchmark numbers are not RC.2 measurements. Latest benchmark qualification
remains incomplete until the actual new artifact runs finish.

The source notes below describe implementation and historical/in-progress gates;
this sealed manifest and raw evidence record the executed package qualification.

---

''' + notes)
    print('ASSEMBLED_PREVIEW_QUALIFIED', release, revision, len(manifest['artifacts']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    assemble(parser.parse_args().root)
