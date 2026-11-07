#!/usr/bin/env node
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdir, mkdtemp, writeFile, readFile } from 'node:fs/promises';
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
const phases=['FP_VECTOR_SCHEMA_BEFORE_MARKER_SYNC','FP_VECTOR_SCHEMA_BEFORE_PUBLICATION','FP_VECTOR_SCHEMA_AFTER_PUBLICATION'];
const results=[];let server;
try{
 for(let i=0;i<phases.length;++i){
  const failpoint=phases[i],name=`schema-crash-${i}`,evidence=path.join(root,`hit-${i}.jsonl`);
  server=await start(name,true,{PACIFICDB_TEST_MODE:'vector_index_crash',
    PACIFICDB_TEST_FAILPOINT_CONFIRM:'I_UNDERSTAND_THIS_PROCESS_WILL_TERMINATE',
    PACIFICDB_TEST_FAILPOINT:failpoint,PACIFICDB_TEST_FAILPOINT_INDEX:'1',PACIFICDB_TEST_FAILPOINT_EVIDENCE:evidence});
  let alice=await server.client('alice','tenant-a');await alice.createDatabase('app');
  const exited=once(server.processHandle,'exit');
  await assert.rejects(alice.createCollection('vectors',{vectorSchema:schema}));
  const timeout=setTimeout(()=>server.processHandle.kill('SIGKILL'),10000);
  const [code]=await exited;clearTimeout(timeout);assert.equal(code,86);
  for(const c of server.clients)c.close();server.log.end();await once(server.log,'finish');server=null;
  assert.match(await readFile(evidence,'utf8'),new RegExp(failpoint));
  server=await start(name,true);alice=await server.client('alice','tenant-a');
  await assert.rejects(alice.insert('vectors',{id:'missing'}),/vector_schema/);
  await alice.insert('vectors',{id:'recovered',vector:[1,0]});
  assert.equal((await alice.queryVector('vectors',[1,0],{metric:'dot',k:1,searchMode:'exact'})).data[0].id,'recovered');
  await stop(server);server=null;
  server=await start(name,true);alice=await server.client('alice','tenant-a');
  await assert.rejects(alice.insert('vectors',{id:'bad-again',vector:[1]}),/vector_schema/);
  assert.equal((await alice.request({action:'count',collection:'vectors'})).count,1);
  await stop(server);server=null;results.push({failpoint,status:'PASS',recoveries:2});
 }
 console.log(JSON.stringify({status:'PASS',scope:'committed schema DDL crash/repeated recovery',results,root}));
}finally{if(server)await stop(server);console.error(`Schema crash evidence retained: ${root}`);}
