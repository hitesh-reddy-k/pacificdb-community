#!/usr/bin/env node
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// minimatch 11 removes the callable default export and replaces parsed RegExp
// segments with nonrecursive Matcher objects. Builder's pinned legacy clients
// need only these import/test adaptations; keep the patched matcher untouched.
// Remove this bridge when electron-builder ships compatible upstream clients.
// Advisory: https://github.com/advisories/GHSA-g3hv-75wg-m249
// Matcher API: https://github.com/isaacs/minimatch/releases/tag/v11.0.3
const patches = [
  {
    name: '@electron/asar', version: '3.4.1', file: 'lib/asar.js',
    before: '91e89e96324626b140f3e897dffaca6e3cbb1135c4192ce6ceaa0e84af4ba152',
    after: '5a0088341bcd90df3249a17da7b1852b5997fcc8bf600824dd3740a6021d7dab',
  },
  {
    name: 'dir-compare', version: '4.2.0', file: 'build/src/FilterHandler/defaultFilterHandler.js',
    before: '6b753f018576fc270dc93e2be35d98a50431bd7f1e373b98acbec5e7acf381d2',
    after: '26eca3b45e7cdb2a6ef3d085fb5230555ecd14eee802bfda041530b88f42bf4b',
  },
  {
    name: 'glob', version: '7.2.3', file: 'glob.js',
    before: 'b8f35657ca927593d0f9e1aae3a8cfe9c33c697bf3c5733c2f6727f25ae25be1',
    after: '4caeca5af34b662a1598d6975bf6541a478443e6e79765095cf24479c3dbcaa4',
  },
  {
    name: 'glob', version: '7.2.3', file: 'sync.js',
    before: '0f61227f4b55297f1ad16798c53e6a6dd55d633856f153133716413b7c5f61ad',
    after: 'fea0ef6613ef0bcb7d103a37383cecb80f12797539e4c76604d57d5e15bce7a5',
  },
  {
    name: 'glob', version: '7.2.3', file: 'common.js',
    before: 'b867e089ab5d4ab19a83e5b34da3dd7f4018fdf255fcacc681aab87d41dc77e8',
    after: 'a0c189aea2cc982112896f9e692d43d1dabfd61589eff8030349ecd5fe1fb63c',
  },
];
const digest = value => createHash('sha256').update(value).digest('hex');
const json = async file => JSON.parse(await readFile(file, 'utf8'));

export async function patchBuildToolCompat(root = fileURLToPath(new URL('../', import.meta.url))) {
  const modules = path.join(root, 'node_modules');
  // npm ci --omit=dev intentionally has no desktop build dependencies.
  const installed = await Promise.all(patches.map(async patch => {
    try { return await json(path.join(modules, patch.name, 'package.json')); }
    catch (error) { if (error.code === 'ENOENT') return null; throw error; }
  }));
  if (installed.every(value => value === null)) return { patched: 0, omitted: true };
  assert.equal((await json(path.join(modules, 'minimatch/package.json'))).version, '11.0.3',
    'build-tool bridge requires the reviewed minimatch version');
  const pending = [];
  for (const [index, patch] of patches.entries()) {
    assert.equal(installed[index]?.version, patch.version, `unreviewed build dependency: ${patch.name}`);
    const file = path.join(modules, patch.name, patch.file);
    const source = await readFile(file, 'utf8');
    const hash = digest(source);
    if (hash === patch.after) continue;
    assert.equal(hash, patch.before, `unreviewed build dependency source: ${patch.name}/${patch.file}`);
    const updated = patch.file === 'common.js'
      // glob 7 walks the original path parts and returns them to its callers.
      // Preserve ../ segments instead of using newer matcher's normalization.
      ? source.replace('self.minimatch = new Minimatch(pattern, options)',
        'options.optimizationLevel = 0\n  self.minimatch = new Minimatch(pattern, options)')
      : patch.name === 'glob'
      ? source.replace('var rawGlob = pn._glob', 'var rawGlob = pn.pattern').replaceAll('e.match(pn)', 'pn.test(e)')
      : source.replace('const minimatch_1 = __importDefault(require("minimatch"));',
        'const minimatch_1 = { default: require("minimatch").minimatch };');
    assert.equal(digest(updated), patch.after, `invalid compatibility patch: ${patch.name}/${patch.file}`);
    pending.push({ file, updated });
  }
  // Validate the entire pinned source set before changing any installed file.
  for (const { file, updated } of pending) await writeFile(file, updated);
  return { patched: pending.length, omitted: false };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  try { console.log(JSON.stringify(await patchBuildToolCompat())); }
  catch (error) { console.error(error.message); process.exitCode = 1; }
}
