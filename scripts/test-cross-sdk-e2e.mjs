#!/usr/bin/env node
// Owns only its disposable root/processes. No registry or existing server writes.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createWriteStream } from 'node:fs';
import { mkdtemp, mkdir, readFile, rm, chmod } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { PacificDBClient } from '../sdk/node/src/index.js';
const repository=path.resolve(import.meta.dirname,'..'),build=path.resolve(process.argv[2]||path.join(repository,'build'));
const root=await mkdtemp(path.join(os.tmpdir(),'pacificdb-cross-sdk-'));await chmod(root,0o700);
for(const dir of ['data','backup','restore'])await mkdir(path.join(root,dir),{mode:0o700});
async function freePort(){const server=net.createServer();await new Promise((resolve,reject)=>server.once('error',reject).listen(0,'127.0.0.1',resolve));const port=server.address().port;await new Promise(resolve=>server.close(resolve));return port;}
const port=await freePort(),raft=await freePort(),password='cross-sdk-test-only-password';
const env={...process.env,PACIFICDB_ENVIRONMENT:'development',DATA_ROOT:path.join(root,'data'),BACKUP_ROOT:path.join(root,'backup'),RESTORE_DIR:path.join(root,'restore'),ENGINE_BIND_HOST:'127.0.0.1',ENGINE_PORT:String(port),ENGINE_AUTH_REQUIRED:'1',PACIFICDB_ENGINE_ADMIN_USERNAME:'admin',PACIFICDB_ENGINE_ADMIN_PASSWORD:password,RAFT_CLUSTER_ID:'cross-sdk',RAFT_NODE_ID:'node-1',RAFT_LISTEN_PORT:String(raft),RAFT_IS_LEADER:'1',MIN_QUORUM_SIZE:'1',ENGINE_CPU_CORES:'2',CONN_MIN_THREADS:'2',CONN_MAX_THREADS:'32',MAX_CONNECTIONS:'64',ADAPTIVE_ADMISSION:'0',ENGINE_KEEPALIVE_MAX_REQUESTS:'10000',TLS_ENABLED:'0',TLS_REQUIRE_CLIENT_CERT:'0',WAL_FSYNC_ENABLED:'1'};
let engine,log;
async function run(command,args,extra={}){
 const child=spawn(command,args,{cwd:extra.cwd||repository,env:{...process.env,...extra.env},stdio:['ignore','pipe','pipe']});let output='';
 child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>output+=b);
 const timer=setTimeout(()=>child.kill('SIGKILL'),180000);const [code]=await once(child,'exit');clearTimeout(timer);
 if(code!==0)throw new Error(`${command} failed (${code}): ${output.replaceAll(password,'[redacted]')}`);return output;
}
async function start(tls=false){
 log=createWriteStream(path.join(root,'engine.log'),{flags:'a',mode:0o600});
 engine=spawn(path.join(build,process.platform==='win32'?'db_engine.exe':'db_engine'),[],{cwd:repository,env:{...env,...tls?{TLS_ENABLED:'1',TLS_CERT_PATH:path.join(root,'cert.pem'),TLS_KEY_PATH:path.join(root,'key.pem'),TLS_CA_PATH:path.join(root,'cert.pem')}:{}},stdio:['ignore','pipe','pipe']});engine.stdout.pipe(log,{end:false});engine.stderr.pipe(log,{end:false});
 const probe=new PacificDBClient({host:'localhost',port,tls,...tls?{caFile:path.join(root,'cert.pem')}:{},timeoutMs:500});
 try{for(let i=0;i<150;i++){if(engine.exitCode!==null)throw new Error('engine exited before ready');try{if((await probe.request({action:'ping'})).status==='pong')return;}catch{}await new Promise(resolve=>setTimeout(resolve,100));}throw new Error('engine startup timed out');}finally{probe.close();}
}
async function stop(signal='SIGINT'){
 if(!engine)return;const done=once(engine,'exit');if(engine.exitCode===null&&engine.signalCode===null){engine.kill(signal);const timer=setTimeout(()=>engine.kill('SIGKILL'),15000);const [code,actual]=await done;clearTimeout(timer);if(signal==='SIGKILL')assert.equal(actual,'SIGKILL');}
 log.end();await once(log,'finish');engine=null;
}
async function clients(phase,tls=false){
 for(const language of ['python','java']){
  const clientEnv={PACIFICDB_URL:`${tls?'pacificdbs':'pacificdb'}://admin:${password}@localhost:${port}/sdk_${language}`,PACIFICDB_SDK_TEST_ROOT:root,PACIFICDB_SDK_PHASE:phase,...tls?{PACIFICDB_SDK_CA:path.join(root,'cert.pem')}:{}};
  let output;
  if(language==='python')output=await run(process.env.PACIFICDB_TEST_PYTHON||'python3',[path.join(repository,'sdk/python/tests/integration_client.py')],{cwd:root,env:{...clientEnv,...process.env.PACIFICDB_TEST_PYTHON?{}:{PYTHONPATH:path.join(repository,'sdk/python')}}});
  else output=await run('java',['-cp',process.env.PACIFICDB_TEST_JAVA_CLASSPATH||`${path.join(repository,'sdk/java/target/test-classes')}${path.delimiter}${path.join(repository,'sdk/java/target/classes')}${path.delimiter}${(await readFile(path.join(root,'java-classpath.txt'),'utf8')).trim()}`,'io.pacificdb.IntegrationClient'],{cwd:root,env:clientEnv});
  console.log(output.trim());
 }
}
try{
 await run('mvn',['-B','-q','-f','sdk/java/pom.xml','test-compile','dependency:build-classpath',`-Dmdep.outputFile=${path.join(root,'java-classpath.txt')}`]);
 await start();await clients('prepare');await stop('SIGKILL');await start();await clients('recovered');await stop();
 await run('openssl',['req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-keyout',path.join(root,'key.pem'),'-out',path.join(root,'cert.pem')]);await chmod(path.join(root,'key.pem'),0o600);
 await start(true);await clients('tls',true);await stop();
 console.log(JSON.stringify({status:'PASS',authenticated:['TCP','TLS'],sdk_clients:2,exact_documents_per_client:160,restart:'SIGKILL',root}));
}finally{await stop().catch(()=>{});if(process.env.PACIFICDB_KEEP_SDK_ROOT!=='1')await rm(root,{recursive:true,force:true});else console.error(`preserved SDK root: ${root}`);}
