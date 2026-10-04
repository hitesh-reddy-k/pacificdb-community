import assert from 'node:assert/strict';
import test from 'node:test';
import { RaftTestCluster } from './lib/raft-test-cluster.mjs';

test('cleanup does not wait twice for a signal-terminated cluster child', async () => {
  const cluster = new RaftTestCluster({ build: '/tmp' });
  cluster.nodes[0] = { child: { exitCode: null, signalCode: 'SIGKILL',
    kill() { assert.fail('a terminated child must not be killed or awaited again'); } } };
  await cluster.stopNode(0, 'SIGKILL');
});
