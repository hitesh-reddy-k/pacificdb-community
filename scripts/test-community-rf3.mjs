#!/usr/bin/env node

import assert from 'node:assert/strict';
import { writeFile } from 'node:fs/promises';
import path from 'node:path';
import { RaftTestCluster } from './lib/raft-test-cluster.mjs';

const repositoryRoot = path.resolve(import.meta.dirname, '..');
const build = path.resolve(process.argv[2] || path.join(repositoryRoot, 'build'));
const cluster = await RaftTestCluster.create({
  build, clusterId: `community-rf3-cert-${process.pid}`, useProxies: false,
  allEligible: false, rootPrefix: 'pacificdb-rf3-cert-'
});

try {
  await cluster.start();
  const clients = [0, 1, 2].map((index) => cluster.client(index, { timeoutMs: 3000 }));
  await cluster.retry(async () => {
    assert.equal((await clients[0].request({ action: 'ping' })).isLeader, true);
  }, 'leader');
  const project = (await cluster.retry(() => clients[0].request({
    action: 'community_project_create', name: 'rf3-project'
  }), 'project create')).project;
  await clients[0].createDatabase('rf3db');
  clients[0].database = 'rf3db';
  await clients[0].createCollection('docs');
  await clients[0].insert('docs', { id: 'before-stop', value: 1 });

  const mediaPath = path.join(cluster.root, 'rf3-media.bin');
  await writeFile(mediaPath, Buffer.alloc(300_000, 23));
  const media = await clients[0].uploadMediaFile('media', mediaPath, { chunkBytes: 131_072 });
  for (let index = 1; index < 3; index += 1) {
    clients[index].database = 'rf3db';
    await cluster.retry(async () => {
      assert.equal((await clients[index].find('docs', { id: 'before-stop' })).data[0].value, 1);
      assert.equal((await clients[index].request({ action: 'community_project_get',
        id: project.id })).project.name, 'rf3-project');
      const manifest = (await clients[index].request({
        action: 'community_media_get', media_id: media.id })).media;
      assert.equal(manifest.status, 'ready');
      assert.equal(manifest.chunk_count, 3);
      assert.equal((await clients[index].request({ action: 'community_media_get_chunk',
        media_id: media.id, index: 0 })).chunk.index, 0);
    }, `initial convergence node ${index + 1}`);
  }

  await cluster.stopNode(2);
  await clients[0].insert('docs', { id: 'while-offline', value: 2 });
  const project2 = (await clients[0].request({
    action: 'community_project_create', name: 'rf3-catchup'
  })).project;
  await cluster.startNode(2);
  clients[2] = cluster.client(2, { timeoutMs: 3000 });
  clients[2].database = 'rf3db';
  await cluster.retry(async () => {
    assert.equal((await clients[2].find('docs', { id: 'while-offline' })).data[0].value, 2);
    assert.equal((await clients[2].request({ action: 'community_project_get',
      id: project2.id })).project.name, 'rf3-catchup');
  }, 'follower catchup');
  const terms = await Promise.all(clients.map((client) => client.request({ action: 'ping' })));
  console.log(JSON.stringify({
    status: 'PASS', leader_term: terms[0].leader_term,
    commit_indexes: terms.map((result) => result.commit_index),
    last_applied: terms.map((result) => result.last_applied),
    replicated_media_chunks: media.chunk_count, follower_restart: 1
  }, null, 2));
} finally {
  await cluster.close();
}
