// Run solely through the installed-module qualification harness.
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
const execute=promisify(execFile);
assert.ok(process.env.PACIFICDB_TEST_NODE_MODULE,'installed Node module required');
const { PacificDB, releaseInfo }=await import(process.env.PACIFICDB_TEST_NODE_MODULE);
assert.equal(releaseInfo.releaseVersion,'1.2.0-rc.1');
const phase=process.env.PACIFICDB_SDK_PHASE,root=process.env.PACIFICDB_SDK_TEST_ROOT;
const db=await PacificDB.connect(process.env.PACIFICDB_URL,{timeoutMs:120000,...process.env.PACIFICDB_SDK_CA?{caFile:process.env.PACIFICDB_SDK_CA}:{}});
const bytes=Buffer.from(Array.from({length:700000},(_,i)=>i%251));
try{
 if(phase==='prepare'){
  await db.createDatabase();for(const name of ['docs','assets','vectors'])await db.createCollection(name);
  await db.insertMany('docs',Array.from({length:128},(_,i)=>({id:String(i),value:i,unicode:'నమస్తే',empty:null})));
  await Promise.all(Array.from({length:32},(_,i)=>db.insert('docs',{id:`concurrent-${i}`,value:i})));
  await db.updateOne('docs',{id:'0'},{$set:{value:500}});
  await db.putVector('vectors','east',[1,0]);
  const input=path.join(root,'node-source.bin');await writeFile(input,bytes);
  const media=await db.uploadMediaFile('assets',input,{chunkBytes:65536});
  await writeFile(path.join(root,'node-state.json'),JSON.stringify({mediaId:media.id}));
  const parsed=new URL(process.env.PACIFICDB_URL);parsed.username='';parsed.password='';
  const anonymous=PacificDB.fromUrl(parsed.href,{...process.env.PACIFICDB_SDK_CA?{caFile:process.env.PACIFICDB_SDK_CA}:{}});
  try{await assert.rejects(anonymous.request({action:'listDatabases'}));}finally{anonymous.close();}
 }
 assert.equal((await db.request({action:'count',collection:'docs'})).count,160);
 assert.equal((await db.find('docs',{id:'0'})).data[0].value,500);
 assert.equal((await db.queryVector('vectors',[1,0],{k:1})).data[0].id,'east');
 if(process.env.PACIFICDB_TEST_CLI_ENTRY){
  const cliEnv={...process.env,...process.env.PACIFICDB_SDK_CA?{PACIFICDB_URL:process.env.PACIFICDB_URL+'?caFile='+encodeURIComponent(process.env.PACIFICDB_SDK_CA)}:{}};
  const ping=await execute(process.execPath,[process.env.PACIFICDB_TEST_CLI_ENTRY,'--no-start','ping'],{cwd:root,env:cliEnv});
  assert.match(ping.stdout,/pong/);
  const count=await execute(process.execPath,[process.env.PACIFICDB_TEST_CLI_ENTRY,'--no-start','request',JSON.stringify({action:'count',collection:'docs'})],{cwd:root,env:cliEnv});
  assert.equal(JSON.parse(count.stdout).count,160);
 }
 const state=JSON.parse(await readFile(path.join(root,'node-state.json')));
 const output=path.join(root,`node-${phase}.bin`);await db.downloadMediaFile(state.mediaId,output);
 assert.deepEqual(await readFile(output),bytes);
 if(process.env.PACIFICDB_TEST_TYPED_VECTORS==='1'){
  const sparse={format:'sparse-v1',precision:'float64',dimensions:1000000,indices:[3,999999],values:[0.125,-2]};
  if(phase==='prepare')await db.insert('vectors',{id:'typed',vectors:{terms:sparse,dense:[0.123456789012345,1]}});
  assert.equal((await db.queryVector('vectors',sparse,{metric:'dot',searchMode:'exact',vectorName:'terms',k:1})).data[0].id,'typed');
 }
 if(process.env.PACIFICDB_TEST_NATIVE_VECTOR_PREVIEW==='1'){
  if(phase==='prepare'){
   await db.createCollection('preview',process.env.PACIFICDB_TEST_VECTOR_SCHEMA==='1'?{vectorSchema:{default:{dimensions:2,metric:'dot',required:true}}}:{});
   await db.putVector('preview','one',[1,0],{title:'rare ocean'});await db.putVector('preview','two',[0,1],{title:'ocean'});
  }
  if(process.env.PACIFICDB_TEST_VECTOR_SCHEMA==='1')await assert.rejects(db.insert('preview',{id:'bad',vector:[1]}));
  await db.createVectorIndex('preview',{name:'flat',algorithm:'flat',dimensions:2,metric:'dot',text_field:'title',max_elements:100,max_source_bytes:1048576,max_index_bytes:4194304,max_lexical_bytes:1048576});
  const end=Date.now()+30000;
  while((await db.vectorIndexStatus('preview','flat')).index.state!=='READY'){assert.ok(Date.now()<end);await new Promise(r=>setTimeout(r,10));}
  assert.equal((await db.queryVector('preview',[1,0],{metric:'dot',k:1,indexName:'flat',requireIndex:true,searchMode:'exact'})).data[0].id,'one');
  assert.equal((await db.queryHybrid('preview',[1,0],'rare',{indexName:'flat',metric:'dot',k:1,candidates:2})).data[0].id,'one');
  if(phase==='tls')await db.dropVectorIndex('preview','flat');
 }
 console.log(JSON.stringify({status:'PASS',language:'node',phase,exact_documents:160,media_bytes:bytes.length}));
}finally{db.close();}
