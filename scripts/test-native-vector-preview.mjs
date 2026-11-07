#!/usr/bin/env node
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, writeFile } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';
const build = path.resolve(process.argv[2] || 'build-v1.2-hnsw-core');
const root = await mkdtemp(path.join(os.tmpdir(), 'pacificdb-native-preview-'));
const password = 'native-preview-fixture-secret';
async function freePort() {
  const server=net.createServer();await new Promise((resolve,reject)=>server.once('error',reject).listen(0,'127.0.0.1',resolve));
  const port=server.address().port;await new Promise(resolve=>server.close(resolve));return port;
}
async function start(name,enabled,extra={}) {
  const home=path.join(root,name);await mkdir(home,{recursive:true});
  const accounts=path.join(home,'accounts.json');await writeFile(accounts,JSON.stringify([
    {username:'alice',password,role:'ADMIN'},{username:'bob',password,role:'ADMIN'},
    {username:'reader',password,role:'READ_ONLY'}]),{mode:0o600});
  const port=await freePort();let raftPort=await freePort();while(raftPort===port)raftPort=await freePort();
  const log=createWriteStream(path.join(home,`engine-${Date.now()}.log`));
  const processHandle=spawn(path.join(build,'db_engine'),[],{env:{...process.env,
    PACIFICDB_ENVIRONMENT:'development',PACIFICDB_HOME:home,DATA_ROOT:path.join(home,'data'),
    BACKUP_ROOT:path.join(home,'backup'),RESTORE_DIR:path.join(home,'restore'),
    ENGINE_BIND_HOST:'127.0.0.1',ENGINE_PORT:String(port),ENGINE_AUTH_REQUIRED:'1',
    RAFT_LISTEN_HOST:'127.0.0.1',RAFT_BIND_HOST:'127.0.0.1',RAFT_LISTEN_PORT:String(raftPort),
    RAFT_IS_LEADER:'1',RAFT_NODE_ID:'preview-1',RAFT_CLUSTER_ID:`native-preview-${name}`,
    MIN_QUORUM_SIZE:'1',ENGINE_CPU_CORES:'2',CONN_MIN_THREADS:'12',CONN_MAX_THREADS:'12',
    ADAPTIVE_ADMISSION:'0',MAX_SCAN_ROWS:'10000',MAX_RESULT_DOCS:'20',MAX_QUERY_TIMEOUT_SEC:'30',
    PACIFICDB_ENGINE_ACCOUNTS_FILE:accounts,PACIFICDB_VECTOR_PREVIEW_ENABLED:enabled?'1':'0',
    WAL_ENABLED:'true',WAL_FSYNC_ENABLED:'true',WAL_FLUSH_ON_COMMIT:'true',...extra},stdio:['ignore','pipe','pipe']});
  processHandle.stdout.pipe(log,{end:false});processHandle.stderr.pipe(log,{end:false});
  const clients=[];
  const probe=new PacificDBClient({port,timeoutMs:300});clients.push(probe);
  let ready=false;
  for(let i=0;i<150;++i){if(processHandle.exitCode!==null)throw Error(`startup exited ${processHandle.exitCode}`);
    try{ready=(await probe.request({action:'ping'})).status==='pong';}catch{}
    if(ready)break;await new Promise(resolve=>setTimeout(resolve,100));}
  assert.ok(ready);probe.close();
  return {port,clients,processHandle,log,async client(username,userId,database='app'){
    const c=new PacificDBClient({port,userId,database,poolSize:4,timeoutMs:30000});clients.push(c);
    await c.authenticate(username,password);return c;}};
}
async function stop(server){
  for(const c of server.clients)c.close();
  if(server.processHandle.exitCode===null){server.processHandle.kill('SIGINT');
    const timer=setTimeout(()=>server.processHandle.kill('SIGKILL'),15000);
    await once(server.processHandle,'exit');clearTimeout(timer);}
  server.log.end();await once(server.log,'finish');assert.equal(server.processHandle.exitCode,0);
}
async function wait(client,collection,name){
  const deadline=Date.now()+30000;
  while(Date.now()<deadline){const result=await client.vectorIndexStatus(collection,name);
    if(result.index.state==='READY')return result.index;
    if(result.index.state==='FAILED')throw Error(JSON.stringify(result));
    await new Promise(resolve=>setTimeout(resolve,10));}
  throw Error('index readiness timeout');
}
const options={name:'flat',algorithm:'flat',dimensions:2,metric:'dot',text_field:'title',
  max_elements:1000,max_scan_rows:1000,max_source_bytes:1024*1024,max_index_bytes:4*1024*1024,max_lexical_bytes:1024*1024};
const exact={k:3,metric:'dot',searchMode:'exact',indexName:'flat',requireIndex:true};
let server;
try{
  server=await start('off',false);let alice=await server.client('alice','tenant-a');
  await alice.createDatabase('app');await alice.createCollection('vectors');await alice.putVector('vectors','one',[1,0]);
  await assert.rejects(alice.createVectorIndex('vectors',options),/preview_disabled/);
  await assert.rejects(alice.queryVector('vectors',[1,0],exact),/preview_disabled/);
  assert.equal((await alice.queryVector('vectors',[1,0],{k:1,searchMode:'exact'})).count,1);
  await stop(server);server=null;
  server=await start('on',true);alice=await server.client('alice','tenant-a');const bob=await server.client('bob','tenant-b');
  for(const [client,owner] of [[alice,'alice'],[bob,'bob']]){
    await client.createDatabase('app');await client.createCollection('vectors');
    await client.insertMany('vectors',Array.from({length:150},(_,i)=>({id:String(i),vector:[i,1],owner,group:i%2,
      title:i===149?'rare shared':'shared',version:1})));}
  await alice.request({action:'security_database_acl_grant',principal:'reader',level:'read-only'});
  const reader=await server.client('reader','tenant-a');
  await alice.createVectorIndex('vectors',options);await wait(alice,'vectors','flat');
  await bob.createVectorIndex('vectors',{...options,name:'ann',algorithm:'hnsw',ef_search:200});await wait(bob,'vectors','ann');
  // Certificates conservatively include global commit position; rebuild after setup writes.
  await alice.rebuildVectorIndex('vectors','flat');await wait(alice,'vectors','flat');
  const hits=await reader.queryVector('vectors',[1,0],exact);
  assert.deepEqual(hits.data.map(d=>d.id),['149','148','147']);assert.equal(hits.search.index_type,'flat');
  assert.equal(hits.search.executed_mode,'exact');assert.equal(hits.search.fallback,false);
  await assert.rejects(reader.createVectorIndex('vectors',options),/permission_denied/);
  for(const action of ['vectorIndexStatus','listVectorIndexes','createVectorIndex','rebuildVectorIndex','dropVectorIndex']){
    await assert.rejects(bob.request({action,userId:'tenant-a',dbName:'app',collection:'vectors',name:'flat',options}),/permission_denied/);}
  await assert.rejects(bob.request({action:'queryVector',userId:'tenant-a',dbName:'app',collection:'vectors',vector:[1,0],index_name:'flat'}),/permission_denied/);
  const ann=await bob.queryVector('vectors',[1,0],{k:3,metric:'dot',searchMode:'ann',indexName:'ann',requireIndex:true});
  assert.equal(ann.search.executed_mode,'ann');assert.equal(ann.count,3);assert.ok(ann.data.every(d=>d.owner==='bob'));
  const hybrid=await alice.queryHybrid('vectors',[1,0],'rare',{indexName:'flat',metric:'dot',k:3,candidates:10});
  assert.equal(hybrid.search.vector_mode,'exact_flat');assert.equal(hybrid.data[0].id,'149');
  assert.ok(Math.abs(hybrid.data[0].score-2/61)<1e-12);
  const filtered=await alice.queryVector('vectors',[1,0],{...exact,requireIndex:false,filter:{group:1}});
  assert.equal(filtered.search.fallback,true);assert.equal(filtered.search.executed_mode,'exact');
  assert.deepEqual(filtered.data.map(d=>d.id),['149','147','145']);
  await assert.rejects(alice.queryVector('vectors',[1,0],{...exact,searchMode:'ann',allowExactFallback:false}),/unavailable/);
  await assert.rejects(alice.createVectorIndex('vectors',{...options,name:'oversized',max_source_bytes:1024**3,max_index_bytes:1024**3}),/global_resource_budget|global_memory_admission/);
  assert.equal((await alice.listVectorIndexes('vectors')).indexes.length,1);
  for(let i=0;i<5;++i){const results=await Promise.all([
    reader.queryVector('vectors',[1,0],exact),bob.queryVector('vectors',[1,0],{k:3,metric:'dot',searchMode:'ann',indexName:'ann',requireIndex:true}),
    alice.queryHybrid('vectors',[1,0],'rare',{indexName:'flat',metric:'dot',k:3,candidates:10})]);
    assert.ok(results[0].data.every(d=>d.owner==='alice'));assert.ok(results[1].data.every(d=>d.owner==='bob'));}
  await alice.updateOne('vectors',{id:'0'},{$set:{vector:[999,0]}});
  assert.equal((await alice.vectorIndexStatus('vectors','flat')).index.state,'STALE');
  await assert.rejects(alice.queryVector('vectors',[1,0],exact),/unavailable/);
  const fallback=await alice.queryVector('vectors',[1,0],{...exact,requireIndex:false});
  assert.equal(fallback.search.fallback,true);assert.equal(fallback.data[0].id,'0');
  await alice.rebuildVectorIndex('vectors','flat');await wait(alice,'vectors','flat');
  assert.equal((await alice.queryVector('vectors',[1,0],exact)).data[0].id,'0');
  await alice.deleteOne('vectors',{id:'149'});await alice.rebuildVectorIndex('vectors','flat');await wait(alice,'vectors','flat');
  assert.ok((await alice.queryVector('vectors',[1,0],exact)).data.every(d=>d.id!=='149'));
  // Committed collection drops cancel derived workers without blocking Raft;
  // freed worker reservations must allow replacement in a fresh incarnation.
  await bob.request({action:'dropCollection',collection:'vectors'});await bob.createCollection('replacement');
  await bob.putVector('replacement','new',[1,0],{title:'new'});
  let replacement;
  for(let attempt=0;attempt<100;++attempt){
    try{replacement=await bob.createVectorIndex('replacement',{...options,name:'replacement'});break;}
    catch(error){if(!String(error).includes('worker_limit'))throw error;await new Promise(resolve=>setTimeout(resolve,10));}
  }
  assert.ok(replacement);await wait(bob,'replacement','replacement');
  await bob.dropVectorIndex('replacement','replacement');
  await bob.createCollection('vectors');await bob.insertMany('vectors',Array.from({length:150},(_,i)=>({id:String(i),vector:[i,1]})));
  assert.equal((await bob.request({action:'count',collection:'vectors'})).count,150);
  await alice.rebuildVectorIndex('vectors','flat');await wait(alice,'vectors','flat');
  await stop(server);server=null;
  server=await start('on',true);alice=await server.client('alice','tenant-a');
  assert.equal((await alice.vectorIndexStatus('vectors','flat')).index.state,'ABSENT');
  await alice.rebuildVectorIndex('vectors','flat');await wait(alice,'vectors','flat');
  assert.equal((await alice.queryVector('vectors',[1,0],exact)).data[0].id,'0');
  await alice.dropVectorIndex('vectors','flat');assert.equal((await alice.request({action:'count',collection:'vectors'})).count,149);
  await stop(server);server=null;
  server=await start('timeout',true);alice=await server.client('alice','tenant-a');
  await alice.createDatabase('app');await alice.createCollection('vectors');await alice.putVector('vectors','one',[1,0],{title:'rare'});
  await alice.createVectorIndex('vectors',options);await wait(alice,'vectors','flat');
  await assert.rejects(alice.queryVector('vectors',[1,0],{...exact,k:1,timeoutMs:0}),/vector_query_timeout/);
  assert.equal((await alice.vectorIndexStatus('vectors','flat')).index.state,'READY');
  await assert.rejects(alice.queryHybrid('vectors',[1,0],'rare',{indexName:'flat',metric:'dot',timeoutMs:0}),/vector_query_timeout/);
  assert.equal((await alice.queryVector('vectors',[1,0],{...exact,k:1})).count,1);
  for(const timeout_ms of [-1,1.5,3600001,'10'])
    await assert.rejects(alice.request({action:'queryVector',collection:'vectors',vector:[1,0],timeout_ms}),/timeout_ms/);
  await stop(server);server=null;
  for(const MAX_QUERY_TIMEOUT_SEC of ['0','-1','junk','1.5','86401']){
    const child=spawn(path.join(build,'db_engine'),[],{env:{...process.env,PACIFICDB_HOME:path.join(root,'invalid-timeout'),
      DATA_ROOT:path.join(root,'invalid-timeout','data'),MAX_QUERY_TIMEOUT_SEC},stdio:['ignore','pipe','pipe']});
    let output='';child.stdout.on('data',chunk=>output+=chunk);child.stderr.on('data',chunk=>output+=chunk);
    const timer=setTimeout(()=>child.kill('SIGKILL'),5000);const [code]=await once(child,'exit');clearTimeout(timer);
    assert.equal(code,78);assert.match(output,/MAX_QUERY_TIMEOUT_SEC must be an integer/);
  }
  server=await start('ann-disabled',true,{VECTOR_HNSW_ENABLED:'false'});alice=await server.client('alice','tenant-a');
  await alice.createDatabase('app');await alice.createCollection('vectors');await alice.putVector('vectors','one',[1,0],{title:'rare'});
  await assert.rejects(alice.createVectorIndex('vectors',{...options,algorithm:'hnsw'}),/vector_ann_disabled/);
  await alice.createVectorIndex('vectors',options);await wait(alice,'vectors','flat');
  assert.equal((await alice.queryVector('vectors',[1,0],{...exact,k:1})).count,1);
  assert.equal((await alice.queryHybrid('vectors',[1,0],'rare',{indexName:'flat',metric:'dot',k:1,candidates:2})).count,1);
  await stop(server);server=null;
  server=await start('low-memory' ,true,{MAX_MEMORY_MB:'128'});alice=await server.client('alice','tenant-a');
  await alice.createDatabase('app');await alice.createCollection('vectors');await alice.putVector('vectors','one',[1,0]);
  await assert.rejects(alice.createVectorIndex('vectors',{name:'too-large',algorithm:'flat',dimensions:2}),/global_memory_admission|global_resource_budget/);
  assert.equal((await alice.request({action:'count',collection:'vectors'})).count,1);
  await stop(server);server=null;
  console.log(JSON.stringify({status:'PASS',preview_default_off:true,authenticated:true,tenants:2,read_only:true,
    exact_flat:true,ann:true,hybrid:true,stale_errors:true,explicit_fallback:true,restart:true,timeout:true,memory_admission:true,root}));
}finally{if(server)await stop(server);console.error(`Native preview evidence retained: ${root}`);}
