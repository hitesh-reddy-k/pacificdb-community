#!/usr/bin/env node
// Layout regression from the public Windows/macOS ARM fresh-install failures.
// Uses the actual Workbench markup/CSS without starting a database server.
import assert from 'node:assert/strict';
import { readFile, mkdir } from 'node:fs/promises';
import { chromium } from 'playwright';

const release = JSON.parse(await readFile(new URL('../release.json', import.meta.url), 'utf8'));
const html = (await readFile(new URL('../cli/workbench/index.html', import.meta.url), 'utf8'))
  .replace(/<script[^>]*src="\/app\.js"[^>]*><\/script>/, '')
  .replace(/<link[^>]*href="\/app\.css"[^>]*>/, '')
  .replace('__RELEASE_LABEL__', `${release.version} (${release.name}; ${release.releaseVersion})`);
const css = await readFile(new URL('../cli/workbench/app.css', import.meta.url), 'utf8');
const browser = await chromium.launch({ chromiumSandbox: true });
try {
  for (const viewport of [{ width: 1024, height: 650 }, { width: 1024, height: 653 },
    { width: 1360, height: 900 }]) {
    const page = await browser.newPage({ viewport });
    await page.setContent(html);
    await page.addStyleTag({ content: css });
    await page.evaluate(() => {
      document.querySelector('#databases').innerHTML = '<div class="tree-node"><div class="tree-row selected"><button type="button" class="tree-toggle" aria-label="Collapse database" aria-expanded="true">⌄</button><button type="button" class="tree-item" role="treeitem" aria-selected="true"><span class="tree-icon">▤</span><span class="tree-name">workspace</span></button><button id="add-collection" type="button" class="tree-add" aria-label="Create collection in workspace">+</button></div><div class="tree-children" role="group"><div class="tree-empty">No collections yet</div></div></div>';
      window.collectionClicked = false;
      document.querySelector('#add-collection').addEventListener('click', () => { window.collectionClicked = true; });
    });
    try {
      // Keep real pointer hit testing: neither force clicks nor DOM dispatch can
      // prove that a user can reach the control behind an overlapping sibling.
      await page.locator('#add-collection').click({ timeout: 2500 });
      assert.equal(await page.evaluate(() => window.collectionClicked), true);
      await page.locator('#overview-create').click({ timeout: 2500 });
      await page.locator('#environment-open').click({ timeout: 2500 });
      console.log(`SHORT_WINDOW_PASS ${viewport.width}x${viewport.height}`);
    } catch (error) {
      if (process.env.WORKBENCH_SCREENSHOTS) {
        await mkdir(process.env.WORKBENCH_SCREENSHOTS, { recursive: true });
        await page.screenshot({ path: `${process.env.WORKBENCH_SCREENSHOTS}/short-window-${viewport.height}.png` });
      }
      throw error;
    } finally { await page.close(); }
  }
} finally { await browser.close(); }
