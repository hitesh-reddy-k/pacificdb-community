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
const scratch=path.resolve('build-v1.2-schema-test-roots');await mkdir(scratch,{recursive:true});
const root = await mkdtemp(path.join(scratch, 'schema-'));
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
const schema={default:{dimensions:2,metric:'dot',required:true}};
const options={name:'ann',dimensions:2,metric:'dot',maintenance:'incremental',ef_search:300,max_elements:2000,
 max_source_bytes:1048576,max_index_bytes:16777216};
const query={metric:'dot',k:3,searchMode:'ann',indexName:'ann',requireIndex:true,allowExactFallback:false};
let server;
const fresh=async client=>{
 await wait(client,'vectors','ann');
 const exact=await client.queryVector('vectors',[1,0],{metric:'dot',k:3,searchMode:'exact'});
 const ann=await client.queryVector('vectors',[1,0],query);
 assert.equal(ann.search.executed_mode,'ann');assert.equal(ann.search.fallback,false);
 assert.deepEqual(ann.data.map(x=>x.id),exact.data.map(x=>x.id));return ann;
};
try{
 server=await start('incremental',true,{RAFT_STANDALONE_BYPASS:'0'});let alice=await server.client('alice','tenant-a');
 await alice.createDatabase('app');await alice.createCollection('vectors',{vectorSchema:schema});
 await alice.insertMany('vectors',Array.from({length:200},(_,i)=>({id:String(i),vector:[i,1],payload:'document'})));
 await alice.createVectorIndex('vectors',options);await fresh(alice);
 const tuned=await alice.queryVector('vectors',[1,0],{...query,efSearch:40});assert.equal(tuned.search.ef_search,40);
 await assert.rejects(alice.queryVector('vectors',[1,0],{...query,efSearch:301}),/ef_search/);
 await alice.updateOne('vectors',{id:'0'},{$set:{vector:[9999,0]}});assert.equal((await fresh(alice)).data[0].id,'0');
 await alice.putVector('vectors','new',[10000,0]);assert.equal((await fresh(alice)).data[0].id,'new');
 await alice.deleteOne('vectors',{id:'new'});assert.equal((await fresh(alice)).data[0].id,'0');
 await alice.insertMany('vectors',Array.from({length:300},(_,i)=>({id:String(i+200),vector:[20000+i,1]})));await fresh(alice);
 const reader=await server.client('alice','tenant-a');let successes=0,explicitLag=0;
 for(let i=0;i<30;++i){
  const write=alice.updateOne('vectors',{id:'0'},{$set:{vector:[30000+i,0]}});
  try{const result=await reader.queryVector('vectors',[1,0],query);assert.equal(result.search.executed_mode,'ann');assert.equal(result.search.fallback,false);++successes;}
  catch(error){assert.match(String(error),/unavailable|freshness_changed|source_changing|visibility_changed/);++explicitLag;}
  await write;
 }
 await fresh(alice);const before=await alice.vectorIndexStatus('vectors','ann');
 for(let i=0;i<10;++i)await alice.updateOne('vectors',{id:'0'},{$set:{payload:`metadata-${i}`}});
 await fresh(alice);assert.equal((await alice.vectorIndexStatus('vectors','ann')).index.graph.physicalNodes,before.index.graph.physicalNodes);
 await stop(server);server=null;
 server=await start('incremental',true,{RAFT_STANDALONE_BYPASS:'0'});alice=await server.client('alice','tenant-a');
 assert.equal((await alice.vectorIndexStatus('vectors','ann')).index.state,'ABSENT');
 await alice.rebuildVectorIndex('vectors','ann');await fresh(alice);
 await alice.updateOne('vectors',{id:'0'},{$set:{vector:[99999,0]}});assert.equal((await fresh(alice)).data[0].id,'0');
 await stop(server);server=null;
 console.log(JSON.stringify({status:'PASS',scope:'standalone replicated-apply incremental ANN preview',crud:true,batch:true,queries_during_updates:{successes,explicitLag},metadata_no_node_growth:true,restart:true,root}));
}finally{if(server)await stop(server);console.error(`Incremental API evidence retained: ${root}`);}
