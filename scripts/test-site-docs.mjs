import assert from 'node:assert/strict';
import { access, readFile } from 'node:fs/promises';
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
      const targetHtml = relativeFile && relativeFile !== filename
        ? await readFile(targetPath, 'utf8')
        : html;
      assert.ok(elementIds(targetHtml).has(fragment), `missing fragment target: ${reference}`);
    }
  }
}

const [index, docs] = await Promise.all([
  readSiteFile('index.html'),
  readSiteFile('docs.html')
]);

for (const asset of [
  'assets/pacificdb-logo-lockup.png',
  'assets/pacificdb-logo-original.png',
  'assets/pacificdb-logo-symbol.png'
]) {
  await access(path.join(siteRoot, asset));
}

assert.match(index, /href=["']docs\.html["'][^>]*>Documentation</);
assert.match(index, /href=["']docs\.html#quickstart["']/);
assert.doesNotMatch(index, /hitesh-reddy-k\.github\.io\/pacificdb-community\/docs\.html/);
assert.match(index, /id=["']sdks["']/);

for (const landingSection of ['top', 'why', 'how', 'features', 'downloads', 'start', 'sdks']) {
  assert.ok(elementIds(index).has(landingSection), `missing landing section: ${landingSection}`);
}

for (const platform of [
  'linux-amd64.deb',
  'windows-x64.exe',
  'macos-arm64.pkg',
  'macos-x86_64.pkg'
]) {
  assert.ok(index.includes(platform), `missing download platform: ${platform}`);
}

const publishedRelease = index.match(
  /const releaseBase='https:\/\/github\.com\/hitesh-reddy-k\/pacificdb-community\/releases\/download\/v([^']+)'/);
assert.ok(publishedRelease, 'landing page must declare a published release URL');
const publishedVersion = publishedRelease[1];
assert.ok(index.includes(`pacificdb-community-${publishedVersion}-`),
  'landing page artifact names must match the published release URL');
assert.ok(docs.includes(`v${publishedVersion}`),
  'documentation version must match the published release URL');
assert.match(index, /id=["']mac-arch["']/);
assert.match(index, /id=["']mac-download["']/);
assert.match(index, /navigator\.clipboard/);
assert.match(index, /document\.createRange/);
assert.match(index, /prefers-reduced-motion:\s*reduce/);
assert.match(index, /<style>[\s\S]*@media\(max-width:720px\)/);
assert.match(index, /classList\.add\('motion-ready'\)/);
assert.match(index, /\.motion-ready \.motion-reveal\{[^}]*opacity:0/);
assert.match(index, /data-reveal/);
assert.match(index, /--scroll-shift/);
assert.match(index, /requestAnimationFrame\(flushScrollMotion\)/);
assert.match(index, /id=["']landingScrollProgress["']/);
assert.doesNotMatch(index, /\.motion-ready \.motion-reveal\[data-reveal="(?:left|right)"\]/);
assert.doesNotMatch(index, /--scroll-scale/);
assert.doesNotMatch(index, /\['\.(?:hero-copy|proof-inner|why-grid|features|downloads|closing \.wrap)'/);

const requiredSections = [
  'install', 'quickstart', 'authentication', 'projects', 'databases',
  'documents', 'shell-reference', 'nodejs', 'python', 'java', 'backups',
  'api-keys', 'media', 'vectors', 'configuration', 'security',
  'troubleshooting', 'beta-status'
];
const docsIds = elementIds(docs);
for (const section of requiredSections) {
  assert.ok(docsIds.has(section), `missing documentation section: ${section}`);
  assert.match(docs, new RegExp(`href=["']#${section}["']`));
}

assert.match(docs, /data-doc-search/);
assert.match(docs, /navigator\.clipboard/);
assert.match(docs, /IntersectionObserver/);
assert.match(docs, /id=["']scrollProgress["']/);
assert.match(docs, /<style>[\s\S]*@media\(max-width:760px\)/);
assert.doesNotMatch(docs, /motion-reveal|flushScrollMotion|--scroll-shift/);

await Promise.all([
  assertLocalReferences('index.html', index),
  assertLocalReferences('docs.html', docs)
]);

for (const obsolete of ['style.css', 'docs.css', 'app.js', 'docs.js', 'pacificdb-logo.png']) {
  await assert.rejects(
    access(path.join(siteRoot, obsolete)),
    error => error?.code === 'ENOENT',
    `${obsolete} should be removed`
  );
  assert.doesNotMatch(index + docs, new RegExp(obsolete.replace('.', '\\.')));
}

console.log('PacificDB website documentation checks passed');
