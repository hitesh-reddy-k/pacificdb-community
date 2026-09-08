const { spawn } = require('child_process');
const net = require('net');
const path = require('path');

const ENGINE_EXE = path.resolve(__dirname, '../build/Release/db_engine.exe');
const ROOT = path.resolve(__dirname, '..', '..', '..');

const NODES = 5;
const raftBase = 12001;
const engineBase = 19000;

const nodes = [];
for (let i = 0; i < NODES; ++i) {
  nodes.push({
    id: `node-${i+1}`,
    raftPort: raftBase + i,
    enginePort: engineBase + i
  });
}

const raftPeers = nodes.map(n => `127.0.0.1:${n.raftPort}`).join(',');

function spawnNode(node) {
  const env = Object.assign({}, process.env);
  env.RAFT_NODE_ID = node.id;
  env.RAFT_PEERS = raftPeers;
  env.RAFT_LISTEN_PORT = String(node.raftPort);
  env.ENGINE_PORT = String(node.enginePort);
  // ensure unique data root per node (main.cpp uses RAFT_NODE_ID)

  const child = spawn(ENGINE_EXE, [], { env });
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', d => console.log(`[${node.id}][OUT] ${d.toString().trim()}`));
  child.stderr.on('data', d => console.error(`[${node.id}][ERR] ${d.toString().trim()}`));
  child.on('exit', (code, sig) => console.log(`[${node.id}] exited code=${code} sig=${sig}`));
  return child;
}

function sendJson(host, port, obj, timeout=2000) {
  return new Promise((resolve, reject) => {
    const sock = new net.Socket();
    let replied = '';
    sock.setTimeout(timeout, () => { sock.destroy(); reject(new Error('timeout')); });
    sock.connect(port, host, () => {
      sock.write(JSON.stringify(obj));
    });
    sock.on('data', d => { replied += d.toString(); });
    sock.on('end', () => {
      try { resolve(JSON.parse(replied)); } catch (e) { reject(e); }
    });
    sock.on('error', e => reject(e));
  });
}

async function waitForClusterReady(timeoutMs=30000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    for (const n of nodes) {
      try {
        const res = await sendJson('127.0.0.1', n.enginePort, { action: 'ping' }, 1000);
        if (res && res.status === 'pong') {
          console.log(`[check] node ${n.id} responded ping isLeader=${res.isLeader}`);
        }
      } catch (e) {
        // ignore
      }
    }
    // detect if any node is leader
    for (const n of nodes) {
      try {
        const res = await sendJson('127.0.0.1', n.enginePort, { action: 'ping' }, 500);
        if (res && res.isLeader) {
          console.log(`[cluster] leader detected: ${n.id}`);
          return n; // return leader node
        }
      } catch (e) { }
    }
    await new Promise(r => setTimeout(r, 1000));
  }
  throw new Error('cluster not ready');
}

async function runTest() {
  console.log('Spawning nodes...');
  const procs = nodes.map(n => ({ node: n, proc: spawnNode(n) }));

  try {
    console.log('Waiting for leader election...');
    const leader = await waitForClusterReady(30000);
    console.log('Leader is', leader.id);

    // Create user/db/collection on leader
    console.log('Creating user/db/collection on leader...');
    await sendJson('127.0.0.1', leader.enginePort, { action: 'initUserSpace', userId: 'testuser' });
    await sendJson('127.0.0.1', leader.enginePort, { action: 'createDatabase', userId: 'testuser', dbName: 'db1' });
    await sendJson('127.0.0.1', leader.enginePort, { action: 'createCollection', userId: 'testuser', dbName: 'db1', collection: 'coll1' });

    // Insert some docs
    console.log('Inserting docs on leader...');
    for (let i = 0; i < 5; ++i) {
      const doc = { id: `doc-${i}`, name: `name-${i}` };
      const res = await sendJson('127.0.0.1', leader.enginePort, { action: 'insert', userId: 'testuser', dbName: 'db1', collection: 'coll1', data: doc });
      console.log('insert res', res);
    }

    // Wait a bit for replication
    await new Promise(r => setTimeout(r, 2000));

    // Verify on followers
    console.log('Verifying data on all nodes...');
    for (const n of nodes) {
      try {
        const res = await sendJson('127.0.0.1', n.enginePort, { action: 'find', userId: 'testuser', dbName: 'db1', collection: 'coll1', filter: {} }, 2000);
        console.log(`${n.id} -> found count=${res.count}`);
      } catch (e) {
        console.error(`${n.id} -> find failed: ${e.message}`);
      }
    }

    console.log('Test completed successfully');
  } finally {
    console.log('Shutting down nodes...');
    for (const p of procs) {
      try { p.proc.kill(); } catch (e) {}
    }
  }
}

runTest().catch(e => { console.error('Test failed:', e); process.exit(1); });
