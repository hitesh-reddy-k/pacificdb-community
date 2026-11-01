import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';

test('candidate desktop workflow stages without publishing or invoking stable qualification', async () => {
  const workflow = (await readFile(new URL('../../.github/workflows/workbench-desktop.yml', import.meta.url), 'utf8')).replace(/\r\n/g, '\n');
  assert.match(workflow, /tags: \['workbench-v\*', 'v\*-rc\.\*'\]/);
  assert.match(workflow, /workbench-preview-evidence\.mjs/);
  const releaseJob = workflow.slice(workflow.indexOf('\n  release:\n'));
  assert.ok(releaseJob.includes("startsWith(github.ref, 'refs/tags/workbench-v')"));
  assert.ok(!releaseJob.includes("startsWith(github.ref, 'refs/tags/v')"));
  assert.match(workflow, /--field version/);
  assert.match(workflow, /--publish never/);
});
