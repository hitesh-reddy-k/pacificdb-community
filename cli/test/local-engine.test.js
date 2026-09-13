import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  actionForEngineState,
  classifyEngineState,
  engineStateCode,
} from '../src/local-engine.js';

const fixture = JSON.parse(await readFile(new URL(
  '../../engine/test/fixtures/local_engine_states.json', import.meta.url), 'utf8'));

test('local-engine state decisions match the native shared fixture', () => {
  for (const testCase of fixture) {
    const state = classifyEngineState(testCase.observations);
    assert.equal(state, testCase.expectedState, testCase.name);
    assert.equal(engineStateCode(state, testCase.observations),
      testCase.expectedCode, testCase.name);
    assert.equal(actionForEngineState(state, false), 'none',
      `${testCase.name} must be read-only with --no-start`);
  }
});
