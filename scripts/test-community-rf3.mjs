#!/usr/bin/env node

import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
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
  const incarnation = async (collection) => {
    const tokens = await Promise.all([0, 1, 2].map(async (index) => {
      const metadata = JSON.parse(await readFile(path.join(cluster.root, `node-${index + 1}`,
        clients[index].userId, 'rf3db', 'db.meta'), 'utf8'));
      return metadata.collections.find((entry) => entry.name === collection)?.vector_incarnation;
    }));
    for (const token of tokens) assert.match(token ?? '', /^raft-create:[1-9][0-9]*$/);
    assert.equal(tokens[1], tokens[0]); assert.equal(tokens[2], tokens[0]);
    return tokens[0];
  };
  await cluster.retry(async () => {
    assert.equal((await clients[0].request({ action: 'ping' })).isLeader, true);
  }, 'leader');
  const project = (await cluster.retry(() => clients[0].request({
    action: 'community_project_create', name: 'rf3-project'
  }), 'project create')).project;
  await cluster.retry(() => clients[0].useProject(project.id), 'select project');
  await clients[0].createDatabase('rf3db');
  clients[0].database = 'rf3db';
  await clients[0].createCollection('docs');
  await clients[0].insert('docs', { id: 'before-stop', value: 1 });
  await clients[0].createCollection('vectors');
  await clients[0].createCollection('named_vectors');
  if(process.env.PACIFICDB_TEST_VECTOR_PREVIEW_GATES==='1'){
    await assert.rejects(clients[0].createCollection('unsupported_schema',{
      vectorSchema:{default:{dimensions:2,metric:'cosine',required:true}}
    }),/vector_schema_cluster_capability_unavailable/);
    await assert.rejects(clients[0].createVectorIndex('vectors',{
      name:'unsupported_cluster_index',dimensions:2,metric:'cosine'
    }),/vector_index_catalog_distributed_identity_unavailable/);
    const metadata=await clients[0].request({action:'listCollections'});
    const names=Array.isArray(metadata)?metadata:metadata.collections;
    assert.ok(Array.isArray(names));assert.ok(!names.some(row=>(typeof row==='string'?row:row.name)==='unsupported_schema'));
  }

  const sparse = (value) => ({ format: 'sparse-v1', precision: 'float64', dimensions: 1000000,
    indices: [999999], values: [value] });
  const namedQuery = (client) => client.queryVector('named_vectors', sparse(1),
    { k: 2, metric: 'dot', vectorName: 'terms', searchMode: 'exact' });
  await clients[0].request({ action: 'insertVector', collection: 'named_vectors',
    data: { id: 'sparse-a', vectors: { terms: sparse(2), dense: [1, 0] } } });
  await clients[0].request({ action: 'insertVector', collection: 'named_vectors',
    data: { id: 'sparse-b', vectors: { terms: sparse(1), dense: [0, 1] } } });
  await clients[0].putVector('vectors', 'east', [1, 0]);
  await clients[0].putVector('vectors', 'north', [0, 1]);
  const exactVector = (client) => client.queryVector('vectors', [1, 0],
    { k: 1, metric: 'dot', searchMode: 'exact' });
  assert.equal((await exactVector(clients[0])).data[0].id, 'east');
  assert.equal((await clients[0].request({ action: 'queryVector', collection: 'vectors',
    vector: [1, 0], k: 1, metric: 'dot', search_mode: 'exact',
    consistency: 'strong' })).data[0].id, 'east');

  const mediaPath = path.join(cluster.root, 'rf3-media.bin');
  await writeFile(mediaPath, Buffer.alloc(300_000, 23));
  const media = await clients[0].uploadMediaFile('media', mediaPath, { chunkBytes: 131_072 });
  for (let index = 1; index < 3; index += 1) {
    clients[index].database = 'rf3db';
    await cluster.retry(async () => {
      assert.equal((await clients[index].find('docs', { id: 'before-stop' })).data[0].value, 1);
      assert.equal((await exactVector(clients[index])).data[0].id, 'east');
      const named = (await namedQuery(clients[index])).data;
      assert.deepEqual(named.map((row) => row.id), ['sparse-a', 'sparse-b']);
      assert.equal(named[0].score, 2);
      assert.deepEqual(named[0].vectors.terms, sparse(2));
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

  const originalVectorIncarnation = await cluster.retry(() => incarnation('vectors'), 'vector incarnation');
  await cluster.stopNode(2);
  await clients[0].createCollection('created_offline');
  await clients[0].request({ action: 'updateOne', collection: 'named_vectors',
    filter: { id: 'sparse-a' }, update: { $set: { vectors: { terms: sparse(-3), dense: [1, 0] } } } });
  await clients[0].request({ action: 'deleteOne', collection: 'named_vectors', filter: { id: 'sparse-b' } });
  await clients[0].insert('docs', { id: 'while-offline', value: 2 });
  await clients[0].request({ action: 'updateOne', collection: 'vectors',
    filter: { id: 'east' }, update: { $set: { vector: [-1, 0] } } });
  await clients[0].request({ action: 'deleteOne', collection: 'vectors', filter: { id: 'north' } });
  await clients[0].putVector('vectors', 'west', [0.5, 0]);
  const project2 = (await clients[0].request({
    action: 'community_project_create', name: 'rf3-catchup'
  })).project;
  await cluster.startNode(2);
  clients[2] = cluster.client(2, { timeoutMs: 3000 });
  clients[2].database = 'rf3db';
  await cluster.retry(async () => {
    assert.equal((await clients[2].find('docs', { id: 'while-offline' })).data[0].value, 2);
    assert.equal((await exactVector(clients[2])).data[0].id, 'west');
    const named = (await namedQuery(clients[2])).data;
    assert.equal(named.length, 1); assert.equal(named[0].id, 'sparse-a'); assert.equal(named[0].score, -3);
    assert.equal((await clients[2].queryVector('vectors', [0, 1],
      { k: 3, searchMode: 'exact' })).data.some((row) => row.id === 'north'), false);
    assert.equal((await clients[2].request({ action: 'community_project_get',
      id: project2.id })).project.name, 'rf3-catchup');
  }, 'follower catchup');
  assert.equal(await cluster.retry(() => incarnation('vectors'), 'restored vector incarnation'),
    originalVectorIncarnation);
  const offlineIncarnation = await cluster.retry(() => incarnation('created_offline'), 'offline creation incarnation');
  assert.notEqual(offlineIncarnation, originalVectorIncarnation);
  await clients[0].request({ action: 'dropCollection', collection: 'created_offline' });
  await clients[0].createCollection('created_offline');
  const recreatedIncarnation = await cluster.retry(() => incarnation('created_offline'), 'recreated incarnation');
  assert.notEqual(recreatedIncarnation, offlineIncarnation);
  const terms = await Promise.all(clients.map((client) => client.request({ action: 'ping' })));
  console.log(JSON.stringify({
    status: 'PASS', leader_term: terms[0].leader_term,
    commit_indexes: terms.map((result) => result.commit_index),
    last_applied: terms.map((result) => result.last_applied),
    replicated_media_chunks: media.chunk_count, follower_restart: 1,
    exact_vector_replication_update_delete_catchup: true,
    replicated_vector_incarnation_restart_catchup_recreate: true,
    sparse_named_vector_insert_update_delete_catchup: true
  }, null, 2));
} finally {
  await cluster.close();
}
