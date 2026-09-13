import net from 'node:net';
import { constants as fsConstants } from 'node:fs';
import {
  access, chmod, mkdir, open, readFile, readlink, realpath, rename, rm, stat,
  writeFile,
} from 'node:fs/promises';
import { createHash, randomBytes } from 'node:crypto';
import os from 'node:os';
import path from 'node:path';
import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);

export function classifyEngineState(observations = {}) {
  const lockIdentityMatches = observations.lockIdentityMatches ?? true;
  if (observations.lockOwned && !lockIdentityMatches) return 'data_root_in_use';
  if (observations.protocolHealthy) {
    if (observations.identityAvailable && !observations.identityMatches) {
      return 'unhealthy';
    }
    return 'healthy_existing';
  }
  if (observations.spawnedProcessExited) return 'exited';
  if (observations.pidPresent && !observations.pidLive) return 'stale_pid';
  if (observations.pidPresent && observations.pidLive) {
    if (observations.executableIdentityKnown && !observations.executableMatches) {
      return 'pid_reused';
    }
    if (observations.startupDeadlineExpired) return 'unhealthy';
    if (observations.executableIdentityKnown && observations.executableMatches) {
      return 'starting';
    }
    return 'unhealthy';
  }
  if (observations.portOpen) return 'port_conflict';
  if (observations.startupDeadlineExpired) return 'unhealthy';
  return 'spawned';
}

export function engineStateCode(state, observations = {}) {
  const codes = {
    healthy_existing: 'engine_healthy_existing',
    starting: 'engine_starting',
    spawned: 'engine_spawn_required',
    exited: 'engine_exited',
    stale_pid: 'engine_stale_pid',
    pid_reused: 'engine_pid_reused',
    port_conflict: 'engine_port_conflict',
    data_root_in_use: 'engine_data_root_in_use',
  };
  if (state === 'unhealthy') {
    return observations.protocolHealthy && observations.identityAvailable &&
      !observations.identityMatches
      ? 'engine_identity_mismatch' : 'engine_unhealthy';
  }
  return codes[state] || 'engine_unhealthy';
}

export function actionForEngineState(state, autoStart) {
  if (!autoStart) return 'none';
  if (state === 'starting') return 'wait';
  if (state === 'spawned') return 'spawn';
  if (state === 'stale_pid') return 'remove_stale_metadata';
  return 'none';
}

function isLocalHost(host) {
  return ['127.0.0.1', 'localhost', '::1'].includes(host);
}

function dataHome() {
  if (process.env.PACIFICDB_HOME) return path.resolve(process.env.PACIFICDB_HOME);
  if (process.platform === 'win32') {
    return path.join(process.env.LOCALAPPDATA || os.homedir(), 'PacificDB');
  }
  if (process.platform === 'darwin') {
    return path.join(os.homedir(), 'Library', 'Application Support', 'PacificDB');
  }
  return path.join(process.env.XDG_DATA_HOME || path.join(os.homedir(), '.local', 'share'),
    'pacificdb');
}

function canConnect(host, port, timeoutMs = 300) {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host, port });
    const done = (connected) => { socket.destroy(); resolve(connected); };
    socket.setTimeout(timeoutMs, () => done(false));
    socket.once('connect', () => done(true));
    socket.once('error', () => done(false));
  });
}

function protocolReady(host, port, nonce = '', timeoutMs = 1000) {
  return new Promise((resolve) => {
    let response = '';
    const socket = net.createConnection({ host, port });
    let settled = false;
    const done = (result) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      resolve(result);
    };
    socket.setTimeout(timeoutMs, () => done(null));
    socket.once('error', () => done(null));
    socket.once('connect', () => socket.write(JSON.stringify({
      action: 'ping', userId: 'system',
      ...(nonce ? { local_discovery_nonce: nonce } : {})
    }) + '\n'));
    socket.on('data', (chunk) => {
      response += chunk;
      if (response.length > 64 * 1024) return done(null);
      const newline = response.indexOf('\n');
      if (newline < 0) return;
      try {
        const value = JSON.parse(response.slice(0, newline));
        done(value?.status === 'pong' ? value : null);
      } catch { done(null); }
    });
    socket.once('end', () => done(null));
  });
}

function localEnvironment(home, port, identity) {
  const raftPort = Number(port) === 9000 ? 9100 : Math.min(Number(port) + 1, 65535);
  return { ...process.env,
    PACIFICDB_HOME: home,
    PACIFICDB_ENVIRONMENT: process.env.PACIFICDB_ENVIRONMENT || 'development',
    DATA_ROOT: process.env.DATA_ROOT || path.join(home, 'data'),
    BACKUP_ROOT: process.env.BACKUP_ROOT || path.join(home, 'backup'),
    RESTORE_DIR: process.env.RESTORE_DIR || path.join(home, 'restore'),
    ENGINE_BIND_HOST: '127.0.0.1',
    ENGINE_AUTH_REQUIRED: process.env.ENGINE_AUTH_REQUIRED || '0',
    ENGINE_PORT: String(port),
    RAFT_LISTEN_PORT: process.env.RAFT_LISTEN_PORT || String(raftPort),
    RAFT_CLUSTER_ID: process.env.RAFT_CLUSTER_ID ?? 'pacificdb-local',
    RAFT_NODE_ID: process.env.RAFT_NODE_ID ?? 'node-1',
    RAFT_IS_LEADER: process.env.RAFT_IS_LEADER || '1',
    MIN_QUORUM_SIZE: process.env.MIN_QUORUM_SIZE || '1',
    ENGINE_CPU_CORES: process.env.ENGINE_CPU_CORES || '2',
    ENGINE_KEEPALIVE_MAX_REQUESTS: process.env.ENGINE_KEEPALIVE_MAX_REQUESTS || '1',
    PACIFICDB_INSTANCE_ID: identity.instanceId,
    PACIFICDB_DISCOVERY_NONCE: identity.discoveryNonce,
    PACIFICDB_DATA_ROOT_FINGERPRINT: identity.dataRootFingerprint,
  };
}

function processIsLive(pid) {
  if (!Number.isSafeInteger(pid) || pid <= 0) return false;
  try { process.kill(pid, 0); return true; }
  catch (error) { return error.code === 'EPERM'; }
}

function validMetadata(value) {
  return value?.schema === 'pacificdb.local-engine.v1' && value.version === 1 &&
    Number.isSafeInteger(value.pid) && value.pid > 0 &&
    typeof value.executable === 'string' && path.isAbsolute(value.executable) &&
    typeof value.instance_id === 'string' && value.instance_id.length > 0 &&
    typeof value.data_root_fingerprint === 'string' &&
    value.data_root_fingerprint.length > 0 &&
    typeof value.discovery_nonce === 'string' && value.discovery_nonce.length > 0 &&
    Number.isInteger(value.port) && value.port >= 1 && value.port <= 65535 &&
    Number.isSafeInteger(value.started_at_ms) && value.started_at_ms >= 0;
}

async function readMetadata(filename) {
  try {
    const value = JSON.parse(await readFile(filename, 'utf8'));
    return validMetadata(value) ? value : null;
  } catch { return null; }
}

async function fileExists(filename) {
  try { await stat(filename); return true; }
  catch { return false; }
}

async function readNumericPid(filename) {
  try {
    const text = await readFile(filename, 'utf8');
    if (!/^[1-9]\d*\s*$/.test(text)) return 0;
    const pid = Number(text.trim());
    return Number.isSafeInteger(pid) ? pid : 0;
  } catch { return 0; }
}

async function atomicOwnerWrite(filename, contents) {
  const temporary = `${filename}.tmp.${process.pid}.${randomBytes(6).toString('hex')}`;
  await writeFile(temporary, contents, { mode: 0o600, flag: 'wx' });
  if (process.platform !== 'win32') await chmod(temporary, 0o600);
  try { await rename(temporary, filename); }
  catch (error) {
    await rm(temporary, { force: true });
    throw error;
  }
}

async function writeMetadata(filename, metadata) {
  await atomicOwnerWrite(filename, JSON.stringify({
    schema: 'pacificdb.local-engine.v1', version: 1, ...metadata,
  }, null, 2) + '\n');
}

async function executableForPid(pid) {
  try {
    if (process.platform === 'linux') {
      return await realpath(await readlink(`/proc/${pid}/exe`));
    }
    if (process.platform === 'win32') {
      const { stdout } = await execFileAsync('powershell.exe', ['-NoProfile',
        '-NonInteractive', '-Command',
        `(Get-Process -Id ${pid} -ErrorAction Stop).Path`], { timeout: 2000 });
      return stdout.trim() || null;
    }
    if (process.platform === 'darwin') {
      const { stdout } = await execFileAsync('/bin/ps',
        ['-p', String(pid), '-o', 'comm='], { timeout: 2000 });
      return stdout.trim() || null;
    }
    return null;
  }
  catch { return null; }
}

async function sameExecutable(first, second) {
  try {
    const [left, right] = await Promise.all([realpath(first), realpath(second)]);
    return process.platform === 'win32'
      ? left.toLowerCase() === right.toLowerCase() : left === right;
  } catch { return false; }
}

async function resolveExecutable(engine) {
  if (path.isAbsolute(engine) || engine.includes(path.sep)) {
    return path.resolve(engine);
  }
  const extensions = process.platform === 'win32'
    ? (process.env.PATHEXT || '.EXE;.CMD;.BAT').split(';') : [''];
  for (const directory of (process.env.PATH || '').split(path.delimiter)) {
    for (const extension of extensions) {
      const candidate = path.join(directory || '.', engine + extension);
      try {
        await access(candidate, fsConstants.X_OK);
        return await realpath(candidate);
      } catch { /* continue */ }
    }
  }
  return path.resolve(engine);
}

async function fingerprintDataRoot(dataRoot) {
  let canonical = await realpath(dataRoot);
  if (process.platform === 'win32') canonical = canonical.toLowerCase();
  return `sha256:${createHash('sha256').update(canonical).digest('hex')}`;
}

async function inspectEngine({ host, port, home, engine }) {
  const metadataFile = path.join(home, 'engine.metadata.json');
  const metadataPresent = await fileExists(metadataFile);
  const metadata = await readMetadata(metadataFile);
  const numericPid = await readNumericPid(path.join(home, 'engine.pid'));
  const pid = metadata?.pid || numericPid;
  const pidLive = processIsLive(pid);
  const observedExecutable = pidLive ? await executableForPid(pid) : null;
  let executableIdentityKnown = Boolean(observedExecutable);
  let executableMatches = observedExecutable
    ? await sameExecutable(observedExecutable, engine) : false;
  const probe = await protocolReady(host, port, metadata?.discovery_nonce || '');
  const protocolHealthy = Boolean(probe);
  const responseIdentityAvailable = Boolean(probe &&
    'instance_id' in probe && 'engine_pid' in probe &&
    'data_root_fingerprint' in probe);
  if (process.platform !== 'linux' && metadata && responseIdentityAvailable &&
      await sameExecutable(metadata.executable, engine)) {
    executableIdentityKnown = true;
    executableMatches = true;
  }
  const identityMatches = Boolean(metadata && responseIdentityAvailable &&
    probe.edition === 'community' && probe.instance_id === metadata.instance_id &&
    probe.engine_pid === metadata.pid &&
    probe.data_root_fingerprint === metadata.data_root_fingerprint &&
    metadata.port === port && executableIdentityKnown && executableMatches);
  const dataRoot = path.resolve(process.env.DATA_ROOT || path.join(home, 'data'));
  let lockOwned = false;
  let lockIdentityMatches = true;
  try {
    const owner = JSON.parse(await readFile(
      path.join(dataRoot, '.pacificdb-root.lock'), 'utf8'));
    if (owner?.schema === 'pacificdb.storage-root-owner.v1' &&
        Number.isSafeInteger(owner.pid) && processIsLive(owner.pid)) {
      const ownerExecutable = await executableForPid(owner.pid);
      lockOwned = Boolean(ownerExecutable &&
        await sameExecutable(ownerExecutable, engine));
      if (lockOwned) {
        lockIdentityMatches = Boolean(metadata && owner.pid === metadata.pid &&
          owner.instance_id && owner.instance_id === metadata.instance_id);
      }
    }
  } catch { /* absent, incomplete, or stale owner metadata */ }
  return {
    metadata, pid,
    observations: {
      protocolHealthy,
      identityAvailable: protocolHealthy && (Boolean(metadata) || metadataPresent),
      identityMatches,
      pidPresent: Boolean(pid), pidLive,
      executableIdentityKnown, executableMatches,
      portOpen: await canConnect(host, port), lockOwned, lockIdentityMatches,
    }
  };
}

function startupTimeoutMs() {
  const configured = process.env.PACIFICDB_STARTUP_TIMEOUT_MS;
  if (!configured) return 120_000;
  const timeout = Number(configured);
  if (!Number.isInteger(timeout) || timeout < 100 || timeout > 1_800_000) {
    throw new Error('PACIFICDB_STARTUP_TIMEOUT_MS must be from 100 to 1800000');
  }
  return timeout;
}

function stateError(state, observations, port, detail = '') {
  const code = engineStateCode(state, observations);
  const description = state === 'port_conflict'
    ? `Port ${port} is in use by a service that is not PacificDB or does not match ` +
      'this local engine; choose another --port'
    : state === 'pid_reused'
      ? 'engine.pid belongs to a different executable'
      : state === 'data_root_in_use'
        ? 'DATA_ROOT is owned by another PacificDB instance'
      : state === 'exited'
        ? 'local engine exited during startup'
        : 'local engine could not be verified';
  return new Error(`${code}: ${description}${detail ? `: ${detail}` : ''}`);
}

async function logTail(filename, maximum = 8192) {
  try {
    const bytes = await readFile(filename);
    return bytes.subarray(Math.max(0, bytes.length - maximum)).toString('utf8');
  } catch { return ''; }
}

export async function ensureLocalEngine(client, { autoStart = true, output } = {}) {
  if (!isLocalHost(client.host)) return false;
  // --no-start is deliberately side-effect free: it does not create the home,
  // clean stale metadata, join a startup owner, or signal any process.
  if (!autoStart) return false;
  const home = dataHome();
  const configuredEngine = process.env.PACIFICDB_ENGINE ||
    (process.platform === 'win32' ? 'db_engine.exe' : 'db_engine');
  const engine = await resolveExecutable(configuredEngine);
  const startLock = path.join(home, '.engine-starting');
  let inspection = await inspectEngine({
    host: client.host, port: client.port, home, engine,
  });
  let state = classifyEngineState(inspection.observations);
  if (state === 'pid_reused' && await fileExists(startLock)) state = 'starting';
  if (state === 'healthy_existing') return false;
  if (state === 'stale_pid') {
    if (inspection.observations.portOpen) {
      throw stateError('port_conflict', inspection.observations, client.port);
    }
    await Promise.all([
      rm(path.join(home, 'engine.pid'), { force: true }),
      rm(path.join(home, 'engine.metadata.json'), { force: true }),
    ]);
    inspection = await inspectEngine({
      host: client.host, port: client.port, home, engine,
    });
    state = classifyEngineState(inspection.observations);
  }
  if (!['spawned', 'starting'].includes(state)) {
    throw stateError(state, inspection.observations, client.port);
  }

  await mkdir(home, { recursive: true, mode: 0o700 });
  let ownsStartLock = false;
  try {
    await mkdir(startLock);
    ownsStartLock = true;
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
    const deadline = performance.now() + startupTimeoutMs();
    while (performance.now() < deadline) {
      inspection = await inspectEngine({
        host: client.host, port: client.port, home, engine,
      });
      if (classifyEngineState(inspection.observations) === 'healthy_existing') {
        return false;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    inspection.observations.startupDeadlineExpired = true;
    throw stateError(classifyEngineState(inspection.observations),
      inspection.observations, client.port,
      'another launcher did not finish before the startup deadline');
  }
  try {
    inspection = await inspectEngine({
      host: client.host, port: client.port, home, engine,
    });
    state = classifyEngineState(inspection.observations);
    if (state === 'healthy_existing') return false;
    if (state === 'starting') {
      const deadline = performance.now() + startupTimeoutMs();
      while (performance.now() < deadline) {
        await new Promise((resolve) => setTimeout(resolve, 100));
        inspection = await inspectEngine({
          host: client.host, port: client.port, home, engine,
        });
        state = classifyEngineState(inspection.observations);
        if (state === 'healthy_existing') return false;
        if (state === 'stale_pid') {
          await Promise.all([
            rm(path.join(home, 'engine.pid'), { force: true }),
            rm(path.join(home, 'engine.metadata.json'), { force: true }),
          ]);
          break;
        }
        if (state !== 'starting') {
          throw stateError(state, inspection.observations, client.port);
        }
      }
      if (state === 'starting') {
        inspection.observations.startupDeadlineExpired = true;
        throw stateError(classifyEngineState(inspection.observations),
          inspection.observations, client.port,
          'the existing process did not become ready before the startup deadline');
      }
    }
    if (inspection.observations.portOpen) {
      throw stateError('port_conflict', inspection.observations, client.port);
    }
    try { await access(engine, fsConstants.X_OK); }
    catch (error) {
      throw new Error(`engine_not_installed: Local engine is not installed (${error.message}). ` +
        'Install the PacificDB Community package or use --host.');
    }

    const provisional = localEnvironment(home, client.port, {
      instanceId: '', discoveryNonce: '', dataRootFingerprint: '',
    });
    await Promise.all([
      mkdir(provisional.DATA_ROOT, { recursive: true, mode: 0o700 }),
      mkdir(provisional.BACKUP_ROOT, { recursive: true, mode: 0o700 }),
      mkdir(provisional.RESTORE_DIR, { recursive: true, mode: 0o700 })
    ]);
    const identity = {
      instanceId: `engine_${randomBytes(16).toString('hex')}`,
      discoveryNonce: randomBytes(32).toString('hex'),
      dataRootFingerprint: await fingerprintDataRoot(provisional.DATA_ROOT),
    };
    const environment = localEnvironment(home, client.port, identity);
    const logPath = path.join(home, 'engine.log');
    const log = await open(path.join(home, 'engine.log'), 'a', 0o600);
    const child = spawn(engine, [], { detached: true, windowsHide: true,
      cwd: path.dirname(engine), env: environment,
      stdio: ['ignore', log.fd, log.fd] });
    try {
      await new Promise((resolve, reject) => {
        child.once('spawn', resolve);
        child.once('error', reject);
      });
    } catch (error) {
      throw new Error(`Local engine is not installed (${error.message}). ` +
        'Install the PacificDB Community package or use --host.');
    } finally {
      await log.close();
    }
    const metadata = {
      pid: child.pid,
      executable: await realpath(engine),
      instance_id: identity.instanceId,
      data_root_fingerprint: identity.dataRootFingerprint,
      discovery_nonce: identity.discoveryNonce,
      port: client.port,
      started_at_ms: Date.now(),
    };
    await writeMetadata(path.join(home, 'engine.metadata.json'), metadata);
    await atomicOwnerWrite(path.join(home, 'engine.pid'), `${child.pid}\n`);

    let exit = null;
    child.once('exit', (code, signal) => { exit = { code, signal }; });
    const deadline = performance.now() + startupTimeoutMs();
    while (performance.now() < deadline) {
      if (exit || child.exitCode !== null || child.signalCode !== null) {
        const observed = exit || { code: child.exitCode, signal: child.signalCode };
        throw stateError('exited', { spawnedProcessExited: true }, client.port,
          `${observed.signal ? `signal ${observed.signal}` : `exit code ${observed.code}`}; ` +
          `see ${logPath}\n${await logTail(logPath)}`);
      }
      inspection = await inspectEngine({
        host: client.host, port: client.port, home, engine,
      });
      if (classifyEngineState(inspection.observations) === 'healthy_existing') {
        child.unref();
        output?.write(`✓ Local engine started at ${client.host}:${client.port}\n`);
        return true;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    inspection.observations.startupDeadlineExpired = true;
    child.unref();
    throw stateError(classifyEngineState(inspection.observations),
      inspection.observations, client.port,
      `startup deadline expired; the process was not killed; see ${logPath}`);
  } finally {
    if (ownsStartLock) await rm(startLock, { recursive: true, force: true });
  }
}
