#!/usr/bin/env node
// Preview staging evidence is deliberately separate from stable qualification.
import assert from 'node:assert/strict';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { validateDesktopIdentity } from '../desktop/release-identity.mjs';

const [platform, artifact, output] = process.argv.slice(2);
assert.ok(['linux-x64', 'windows-x64', 'macos-arm64', 'macos-x64'].includes(platform), 'platform required');
assert.ok(artifact && output, 'usage: workbench-preview-evidence.mjs PLATFORM ARTIFACT OUTPUT');
const release = JSON.parse(await readFile('release.json', 'utf8'));
assert.equal(release.channel, 'prerelease', 'preview evidence requires a prerelease');
const stage = JSON.parse(await readFile('desktop/stage/package.json', 'utf8'));
const cli = JSON.parse(await readFile('cli/package.json', 'utf8'));
const manifest = JSON.parse(await readFile('desktop/resources/engine/manifest.json', 'utf8'));
validateDesktopIdentity(release, manifest.engineIdentity, stage.version, cli.version);
assert.deepEqual(manifest.release, release, 'staged canonical identity mismatch');
const revision = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
assert.equal(manifest.sourceRevision, revision, 'staged revision mismatch');
assert.equal(manifest.engineIdentity.gitCommit, revision, 'engine revision mismatch');
for (const [filename, expected] of [[process.platform === 'win32' ? 'db_engine.exe' : 'db_engine', manifest.engineSha256],
  [process.platform === 'win32' ? 'pacificdb.exe' : 'pacificdb', manifest.cliSha256]]) {
  const actual = createHash('sha256').update(await readFile(path.join('desktop/resources/engine', filename))).digest('hex');
  assert.equal(actual, expected, 'staged native executable digest mismatch');
}
const dirty = execFileSync('git', ['status', '--porcelain', '--untracked-files=all'], { encoding: 'utf8' }).trim().length > 0;
const bytes = await readFile(artifact);
const evidence = { schemaVersion: 1, profile: 'workbench-prerelease-staging', release,
  revision, sourceDirty: dirty, platform, status: 'STAGED',
  artifact: { path: artifact, sizeBytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') },
  engineIdentity: manifest.engineIdentity,
  signing: 'NOT_ASSERTED', installedPlatformQualification: 'NOT_ASSERTED',
  stableQualification: { status: 'NOT_QUALIFIED', physicalPower: 'NOT_TESTED',
    externalSecurityReview: 'NOT_TESTED', sustainedLoad: 'NOT_TESTED' } };
await mkdir(path.dirname(output), { recursive: true });
await writeFile(output, JSON.stringify(evidence, null, 2) + '\n');
console.log(`WORKBENCH_PREVIEW_STAGED ${platform} ${evidence.artifact.sha256} sourceDirty=${dirty}`);
