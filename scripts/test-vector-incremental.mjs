#!/usr/bin/env node
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
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
async function start(name,enabled,extra={},accountContents=null) {
  const home=path.join(root,name);await mkdir(home,{recursive:true});
  const accounts=path.join(home,'accounts.json');await writeFile(accounts,accountContents??JSON.stringify([
    {username:'alice',password,role:'ADMIN'},{username:'bob',password,role:'ADMIN'},
    {username:'reader',password,role:'READ_ONLY'}]),{mode:0o600});
  if(process.platform==='win32'){
    // POSIX mode bits do not set Windows ACLs. Grant the current test owner
    // access explicitly without changing account roles or disabling auth.
    execFileSync('powershell.exe',['-NoProfile','-NonInteractive','-Command',
      '$identity=[System.Security.Principal.WindowsIdentity]::GetCurrent().User; '+
      '$acl=New-Object System.Security.AccessControl.FileSecurity; '+
      '$acl.SetOwner($identity); $acl.SetAccessRuleProtection($true,$false); '+
      '$acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($identity,"FullControl","Allow"))); '+
      'Set-Acl -LiteralPath $env:PACIFICDB_TEST_ACCOUNT_PATH -AclObject $acl'],
      {env:{...process.env,PACIFICDB_TEST_ACCOUNT_PATH:accounts},stdio:'pipe'});
  }
  const port=await freePort();let raftPort=await freePort();while(raftPort===port)raftPort=await freePort();
  const log=createWriteStream(path.join(home,`engine-${Date.now()}.log`));
  const env={...process.env,
    PACIFICDB_ENVIRONMENT:'development',PACIFICDB_HOME:home,DATA_ROOT:path.join(home,'data'),
    BACKUP_ROOT:path.join(home,'backup'),RESTORE_DIR:path.join(home,'restore'),
    ENGINE_BIND_HOST:'127.0.0.1',ENGINE_PORT:String(port),ENGINE_AUTH_REQUIRED:'1',
    RAFT_LISTEN_HOST:'127.0.0.1',RAFT_BIND_HOST:'127.0.0.1',RAFT_LISTEN_PORT:String(raftPort),
    RAFT_IS_LEADER:'1',RAFT_NODE_ID:'preview-1',RAFT_CLUSTER_ID:`native-preview-${name}`,
    MIN_QUORUM_SIZE:'1',ENGINE_CPU_CORES:'2',CONN_MIN_THREADS:'12',CONN_MAX_THREADS:'12',
    ADAPTIVE_ADMISSION:'0',MAX_SCAN_ROWS:'10000',MAX_RESULT_DOCS:'20',MAX_QUERY_TIMEOUT_SEC:'30',
    PACIFICDB_ENGINE_ACCOUNTS_FILE:accounts,PACIFICDB_VECTOR_PREVIEW_ENABLED:enabled?'1':'0',
    WAL_ENABLED:'true',WAL_FSYNC_ENABLED:'true',WAL_FLUSH_ON_COMMIT:'true',...extra};
  const processHandle=spawn(path.join(build,process.platform==='win32'?'db_engine.exe':'db_engine'),[],{env,stdio:['ignore','pipe','pipe']});
  const closed=once(processHandle,'close');
  let diagnostic='';
  for(const stream of [processHandle.stdout,processHandle.stderr])stream.on('data',chunk=>{diagnostic=(diagnostic+chunk.toString()).slice(-8192);});
  processHandle.stdout.pipe(log,{end:false});processHandle.stderr.pipe(log,{end:false});
  const clients=[];
  const probe=new PacificDBClient({port,timeoutMs:300});clients.push(probe);
  let ready=false;
  try{
  for(let i=0;i<150;++i){if(processHandle.exitCode!==null)throw Error(`startup exited ${processHandle.exitCode}: ${diagnostic.replaceAll(password,'[fixture-secret]')}`);
    try{ready=(await probe.request({action:'ping'})).status==='pong';}catch{}
    if(ready)break;await new Promise(resolve=>setTimeout(resolve,100));}
  assert.ok(ready);probe.close();
  return {port,clients,processHandle,log,env,async client(username,userId,database='app'){
    const c=new PacificDBClient({port,userId,database,poolSize:4,timeoutMs:30000});clients.push(c);
    await c.authenticate(username,password);return c;}};
  }catch(error){
    for(const client of clients)client.close();
    if(processHandle.exitCode===null&&processHandle.signalCode===null)processHandle.kill('SIGKILL');
    await closed;log.end();await once(log,'finish');throw error;
  }
}
async function stop(server){
  for(const c of server.clients)c.close();
  let failure;
  if(server.processHandle.exitCode===null&&server.processHandle.signalCode===null){
    const exited=once(server.processHandle,'exit');
    const timer=setTimeout(()=>server.processHandle.kill('SIGKILL'),process.platform==='win32'?40000:15000);
    try{
    if(process.platform==='win32'){
      // Use the engine's private owner-identity shutdown event. Node's Windows
      // kill emulation cannot exercise the graceful signal path used on POSIX.
      const stopper=spawn(path.join(build,'pacificdb.exe'),['--port',String(server.port),'stop'],{env:server.env,stdio:'ignore'});
      const cliTimer=setTimeout(()=>stopper.kill('SIGKILL'),35000);
      const [code]=await once(stopper,'exit');clearTimeout(cliTimer);assert.equal(code,0,'owned Windows engine stop failed');
    }else server.processHandle.kill('SIGINT');
    await exited;
    }catch(error){failure=error;
      if(server.processHandle.exitCode===null&&server.processHandle.signalCode===null)server.processHandle.kill('SIGKILL');
      await exited;
    }finally{clearTimeout(timer);}
  }
  server.log.end();await once(server.log,'finish');if(failure)throw failure;assert.equal(server.processHandle.exitCode,0);
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
 // Invalid startup configuration must refuse cleanly, preserving the original
 // error instead of terminating through the live memory-monitor thread.
 await assert.rejects(start('refused-accounts',true,
   {PACIFICDB_ENGINE_ACCOUNTS_FILE:path.join(root,'missing-accounts.json')}),
   error=>/startup exited 1:/.test(String(error))&&!/engine_fatal/.test(String(error)));
 await assert.rejects(start('malformed-accounts',true,{},
   '{"password":"fixture-sensitive-never-log", malformed'),
   error=>/startup exited 1:/.test(String(error))&&/accounts file contains invalid JSON/.test(String(error))&&
     !/fixture-sensitive-never-log|engine_fatal/.test(String(error)));
 server=await start('incremental',true,{RAFT_STANDALONE_BYPASS:'0'});let alice=await server.client('alice','tenant-a');
 await alice.createDatabase('app');await alice.createCollection('vectors',{vectorSchema:schema});
 await alice.createCollection('documents');
 await alice.insertMany('vectors',Array.from({length:200},(_,i)=>({id:String(i),vector:[i,1],payload:'document'})));
 await alice.createVectorIndex('vectors',options);await fresh(alice);
 // Unrelated committed documents cannot activate an omitted vector in this
 // complete incremental source. They must not require graph maintenance or a
 // longer freshness wait. Keep exact fallback disabled and count every read.
 const beforeUnrelated=await alice.queryVector('vectors',[1,0],query);
 let unrelatedSuccesses=0;
 for(let i=0;i<30;++i){
  await alice.insert('documents',{id:`unrelated-${i}`,value:i});
  const response=await alice.queryVector('vectors',[1,0],{...query,indexWaitMs:0});
  assert.equal(response.search.executed_mode,'ann');assert.equal(response.search.fallback,false);
  assert.equal(response.search.generation,beforeUnrelated.search.generation,
   'unrelated document commit unnecessarily republished the vector graph');
  assert.equal(response.search.source_revision,beforeUnrelated.search.source_revision);
  assert.deepEqual(response.data.map(row=>row.id),beforeUnrelated.data.map(row=>row.id));
  ++unrelatedSuccesses;
 }
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
 // A completed correctness check which accepts every lag error does not prove
 // query availability. Ordinary strict ANN requests must bridge bounded
 // incremental lag without silently changing to an exact plan.
 assert.equal(successes,30,'strict ANN queries failed during incremental writes');
 assert.equal(explicitLag,0,'freshness lag leaked as immediate query failures');
 // Match the published failed workload shape: four independent clients each
 // submit ten writes interleaved with ninety strict ANN reads. Count every
 // request; no client retries, exact fallback, or accepted freshness errors.
 let mixedSuccesses=0;
 const mixedStart=performance.now();
 await Promise.all(Array.from({length:4},async(_,worker)=>{
  const client=await server.client('alice','tenant-a');
  for(let operation=0;operation<100;++operation){
   if(operation%10===0){
    await client.updateOne('vectors',{id:String(100+worker*10+operation/10)},
      {$set:{vector:[40000+worker*100+operation,1]}});
   }else{
    const result=await client.queryVector('vectors',[1,0],query);
    assert.equal(result.search.executed_mode,'ann');assert.equal(result.search.fallback,false);
    assert.equal(result.data.length,3);assert.equal(new Set(result.data.map(d=>d.id)).size,3);
    for(const row of result.data)assert.equal(row.score,row.vector[0]);
    ++mixedSuccesses;
   }
  }
 }));
 assert.equal(mixedSuccesses,360);
 const mixedMilliseconds=performance.now()-mixedStart;
 for(const index_wait_ms of [-1,1.5,5001,'1',null]){
  await assert.rejects(alice.request({action:'queryVector',collection:'vectors',vector:[1,0],
    metric:'dot',search_mode:'ann',index_name:'ann',index_wait_ms}),/index_wait_ms/);
 }
 await assert.rejects(alice.queryVector('vectors',[1,0],{...query,timeoutMs:0}),/timeout|cancel/);
 assert.equal((await alice.queryVector('vectors',[1,0],{...query,indexWaitMs:0})).search.executed_mode,'ann');
 await fresh(alice);const before=await alice.vectorIndexStatus('vectors','ann');
 for(let i=0;i<10;++i)await alice.updateOne('vectors',{id:'0'},{$set:{payload:`metadata-${i}`}});
 await fresh(alice);assert.equal((await alice.vectorIndexStatus('vectors','ann')).index.graph.physicalNodes,before.index.graph.physicalNodes);
 await stop(server);server=null;
 server=await start('incremental',true,{RAFT_STANDALONE_BYPASS:'0'});alice=await server.client('alice','tenant-a');
 assert.equal((await alice.vectorIndexStatus('vectors','ann')).index.state,'ABSENT');
 await alice.rebuildVectorIndex('vectors','ann');await fresh(alice);
 await alice.updateOne('vectors',{id:'0'},{$set:{vector:[99999,0]}});assert.equal((await fresh(alice)).data[0].id,'0');
 await stop(server);server=null;
 console.log(JSON.stringify({status:'PASS',scope:'standalone replicated-apply incremental ANN preview',crud:true,batch:true,unrelated_commits_without_wait:{attempts:30,successes:unrelatedSuccesses},queries_during_updates:{successes,explicitLag},mixed:{attempts:360,successes:mixedSuccesses,milliseconds:mixedMilliseconds},metadata_no_node_growth:true,restart:true,root}));
}finally{if(server)await stop(server);console.error(`Incremental API evidence retained: ${root}`);}
