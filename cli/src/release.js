import { readFile } from 'node:fs/promises';

// Installed npm packages carry their candidate identity in package.json.
export function describeRelease(releaseVersion) {
  const match = /^(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.-]+)?$/.exec(releaseVersion);
  if (!match) throw new Error('invalid release version');
  const version = `${match[1]}.${match[2]}.${match[3]}`;
  const channel = match[4] ? 'prerelease' : 'stable';
  const name = channel === 'prerelease' ? `Pre-release v${match[1]}.${match[2]}` : `PacificDB ${version}`;
  return { version, releaseVersion, channel, name,
    label: channel === 'prerelease' ? `${version} (${name}; ${releaseVersion})` : version };
}
export const releaseIdentity = describeRelease(JSON.parse(await readFile(
  new URL('../package.json', import.meta.url), 'utf8')).version);
