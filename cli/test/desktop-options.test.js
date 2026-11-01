import assert from 'node:assert/strict';
import test from 'node:test';
import { desktopVectorPreviewEnvironment } from '../../desktop/engine.mjs';

test('desktop vector preview requires its own explicit opt-in', () => {
  assert.deepEqual(desktopVectorPreviewEnvironment({ PACIFICDB_VECTOR_PREVIEW_ENABLED: '1' }), {});
  assert.deepEqual(desktopVectorPreviewEnvironment({ PACIFICDB_WORKBENCH_VECTOR_PREVIEW: '0' }), {});
  assert.deepEqual(desktopVectorPreviewEnvironment({ PACIFICDB_WORKBENCH_VECTOR_PREVIEW: '1' }), { PACIFICDB_VECTOR_PREVIEW_ENABLED: '1' });
});
