import assert from 'node:assert/strict';
import { access, readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const siteRoot = path.join(repositoryRoot, 'site');

async function readSiteFile(filename) {
  return readFile(path.join(siteRoot, filename), 'utf8');
}

function elementIds(html) {
  return new Set([...html.matchAll(/\sid=["']([^"']+)["']/g)].map(match => match[1]));
}

async function assertLocalReferences(filename, html) {
  for (const match of html.matchAll(/\s(?:href|src)=["']([^"']+)["']/g)) {
    const reference = match[1];
    if (/^(?:https?:|mailto:|tel:)/.test(reference)) continue;

    const [relativeFile, fragment] = reference.split('#', 2);
    const targetName = relativeFile || filename;
    const targetPath = path.resolve(siteRoot, targetName);
    assert.ok(targetPath.startsWith(siteRoot + path.sep), `unsafe local reference: ${reference}`);
    await access(targetPath);

    if (fragment) {
      const targetHtml = targetName === filename ? html : await readFile(targetPath, 'utf8');
      assert.ok(elementIds(targetHtml).has(fragment), `missing fragment target: ${reference}`);
    }
  }
}

const [index, docs, css, javascript] = await Promise.all([
  readSiteFile('index.html'),
  readSiteFile('docs.html'),
  readSiteFile('docs.css'),
  readSiteFile('docs.js')
]);

const requiredSections = [
  'install', 'quickstart', 'authentication', 'projects', 'databases',
  'documents', 'shell-reference', 'nodejs', 'python', 'java', 'backups',
  'api-keys', 'media', 'vectors', 'configuration', 'security',
  'troubleshooting', 'beta-status'
];
const ids = elementIds(docs);
for (const section of requiredSections) {
  assert.ok(ids.has(section), `missing documentation section: ${section}`);
  assert.match(docs, new RegExp(`href=["']#${section}["']`));
}

assert.match(docs, /href=["']docs\.css["']/);
assert.match(docs, /src=["']docs\.js["']/);
assert.match(docs, /pacificdb-logo\.png/);
assert.match(css, /@media\s*\(max-width:\s*760px\)/);

await Promise.all([
  assertLocalReferences('index.html', index),
  assertLocalReferences('docs.html', docs)
]);

const require = createRequire(import.meta.url);
const docsModule = require(path.join(siteRoot, 'docs.js'));
assert.equal(typeof docsModule.filterDocumentationItems, 'function');
assert.deepEqual(
  docsModule.filterDocumentationItems(['Install PacificDB', 'Vector search', 'Backups'], 'VECTOR'),
  [false, true, false]
);
assert.deepEqual(
  docsModule.filterDocumentationItems(['Install PacificDB', 'Vector search'], '   '),
  [true, true]
);
assert.match(javascript, /navigator\.clipboard/);

console.log('PacificDB website documentation checks passed');
