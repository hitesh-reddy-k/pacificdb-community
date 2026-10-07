#!/usr/bin/env node
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

async function manifest(path) {
  return JSON.parse(await readFile(new URL(`../${path}`, import.meta.url), 'utf8'));
}

const [root, cli, client, stage] = await Promise.all([
  manifest('package.json'),
  manifest('cli/package.json'),
  manifest('sdk/node/package.json'),
  manifest('desktop/stage/package.json'),
]);

assert.equal(root.version, '1.1.2', 'root manifest is the patch version');
assert.equal(cli.version, root.version, 'bundled CLI must match the patch');
assert.equal(client.version, root.version, 'bundled client must match the patch');
assert.equal(cli.dependencies['@pacificdb/client'], client.version,
  'CLI must depend on the exact bundled client');
assert.equal(stage.version, root.version, 'staged desktop must use the Workbench version');
assert.equal(stage.dependencies['@pacificdb/client'], client.version,
  'staged desktop must bundle the released client');

console.log('WORKBENCH_PACKAGE_CONTRACT_PASS');
