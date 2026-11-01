import assert from 'node:assert/strict';
import { access, readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const siteRoot = path.join(repositoryRoot, 'site');
// Website release metadata is independent of stable/default npm dist-tags.
const published = JSON.parse(await readFile(path.join(siteRoot, 'release.json'), 'utf8'));
const version = published.candidate;
const tag = published.tag;
assert.equal(published.channel, 'prerelease');
assert.equal(tag, `v${version}`);
assert.match(version, /^1\.2\.0-rc\.\d+$/);
const releaseFilename = `release-${published.version}.html`;

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
    if (fragment) {
      const targetHtml = relativeFile && relativeFile !== filename
        ? await readFile(targetPath, 'utf8')
        : html;
      assert.ok(elementIds(targetHtml).has(fragment), `missing fragment target: ${reference}`);
    } else await access(targetPath);
  }
}

const [index, docs, releaseNotes] = await Promise.all([
  readSiteFile('index.html'),
  readSiteFile('docs.html'),
  readSiteFile(releaseFilename)
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
assert.ok(!index.includes('hitesh-reddy-k.github.io/pacificdb-community/docs.html'));
assert.match(index, /id=["']sdks["']/);
assert.ok(index.includes(`release-${published.version}.html`));
assert.ok(docs.includes(`release-${published.version}.html`));
const workbench = index.match(/<section\b[^>]*\bid=["']workbench["'][^>]*>[\s\S]*?<\/section>/)?.[0];
assert.ok(workbench, 'landing page must expose the Workbench section');
const releaseBase = `https://github.com/hitesh-reddy-k/pacificdb-community/releases/download/${tag}/`;
for (const suffix of ['linux-amd64.deb', 'win-x64.exe', 'mac-arm64.dmg', 'mac-x64.dmg']) {
  assert.ok(workbench.includes(`href="${releaseBase}PacificDB-Workbench-${version}-${suffix}"`),
    `Workbench must link the published ${suffix} installer`);
}
assert.ok(workbench.includes(`href="${releaseBase}SHA256SUMS"`),
  'Workbench must link its release checksum manifest');
assert.doesNotMatch(workbench, /<button\b[^>]*\bdisabled/, 'published platforms must be actionable');
for (const heading of ['Added', 'Removed', 'Improved']) {
  assert.ok(releaseNotes.includes('<h3>' + heading + '</h3>'));
}
for (const section of ['top', 'why', 'how', 'features', 'downloads', 'start', 'sdks']) {
  assert.ok(elementIds(index).has(section), 'missing landing section: ' + section);
}
const publishedAssets = new Set(published.assets);
for (const html of [index, docs, releaseNotes]) {
  assert.doesNotMatch(html, /current stable|not certified production-ready[^<]*guaranteed/i);
  assert.doesNotMatch(html, /1\.1\.[12]/, 'current guides must use the RC2 release');
  assert.match(html, /prerelease|pre-release/i, 'current pages must retain the prerelease boundary');
  for (const match of html.matchAll(/href=["']https:\/\/github\.com\/hitesh-reddy-k\/pacificdb-community\/releases\/download\/([^"'<>\s&]+)/g)) {
    const [downloadTag, name] = match[1].split('/');
    assert.equal(downloadTag, tag, 'download tags must match the published prerelease');
    assert.ok(publishedAssets.has(name), `unpublished release download: ${name}`);
  }
}
assert.ok(index.includes(`<span class="badge">v${version}</span>`), 'visible candidate version must match release metadata');
assert.ok(index.includes(releaseBase + `pacificdb-community-${version}-linux-x64.deb`));
for (const platform of ['Windows', 'macOS']) {
  const card = index.match(new RegExp(`<article class="download-card">(?:(?!</article>)[\\s\\S])*?<h3>${platform}</h3>[\\s\\S]*?</article>`))?.[0];
  assert.ok(card, 'missing native platform card');
  assert.doesNotMatch(card, /<button\b[^>]*\bdisabled/, 'published installers must be actionable');
  assert.match(card, /href=["'][^"']*releases\/download/, 'published installers must have download links');
}
assert.doesNotMatch(index, /<select[^>]*id="mac-arch"[^>]*disabled/);
assert.match(index, /macArch\?\.addEventListener\('change'/);
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
  'install', 'quickstart', 'projects', 'databases',
  'documents', 'shell-reference', 'nodejs', 'python', 'java', 'backups',
  'api-keys', 'media', 'vectors', 'configuration', 'security',
  'troubleshooting', 'v1-status'
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
  assertLocalReferences('docs.html', docs),
  assertLocalReferences(releaseFilename, releaseNotes)
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
