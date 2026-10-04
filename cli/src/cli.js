import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { stdin, stdout } from 'node:process';
import { PacificDBClient, parseConnectionUrl } from '@pacificdb/client';
import { SHELL_HELP, printResponse, runShell } from './shell.js';
import { ensureLocalEngine } from './local-engine.js';
import { startWorkbench } from './workbench.js';

function parseJson(text) {
  try { return JSON.parse(text); }
  catch { throw new Error('invalid JSON'); }
}

export async function main(args, streams = { input: stdin, output: stdout }) {
  let options = {};
  const positional = [];
  const valueFlags = new Set(['--host', '--port', '--database', '--content-type',
    '--metadata', '--k', '--metric', '--ui-port', '--url']);
  for (let i = 0; i < args.length; ++i) {
    if (valueFlags.has(args[i]) && (!args[i + 1] || args[i + 1].startsWith('--'))) {
      throw new Error(`${args[i]} requires a value`);
    }
    if (args[i] === '--host') options.host = args[++i];
    else if (args[i] === '--port') options.port = Number(args[++i]);
    else if (args[i] === '--database') options.database = args[++i];
    else if (args[i] === '--content-type') options.contentType = args[++i];
    else if (args[i] === '--metadata') options.metadata = parseJson(args[++i]);
    else if (args[i] === '--k') options.k = Number(args[++i]);
    else if (args[i] === '--metric') options.metric = args[++i];
    else if (args[i] === '--no-start') options.autoStart = false;
    else if (args[i] === '--ui-port') options.uiPort = Number(args[++i]);
    else if (args[i] === '--url') options.url = args[++i];
    else if (args[i] === '--help' || args[i] === '-h') options.help = true;
    else if (args[i] === '--version' || args[i] === '-V') options.version = true;
    else positional.push(args[i]);
  }
  if (options.version) {
    const packageJson = parseJson(await readFile(
      new URL('../package.json', import.meta.url), 'utf8'));
    streams.output.write(`PacificDB ${packageJson.version}\n`);
    return;
  }
  if (options.help) {
    streams.output.write('Usage: pacificdb [shell|workbench|ping|request|put-media|get-media|put-vector|query-vector] [options]\n\n');
    streams.output.write('Options: --help, -h  --version, -V  --url URL  --host HOST  --port PORT  --database NAME  --ui-port PORT  --no-start\nUse PACIFICDB_URL for credentials instead of putting them in command history.\n\n');
    streams.output.write(SHELL_HELP);
    return;
  }
  const url = options.url ?? process.env.PACIFICDB_URL;
  delete options.url;
  if (url !== undefined) options = parseConnectionUrl(url, options);
  if (options.port !== undefined && (!Number.isSafeInteger(options.port) || options.port < 1 || options.port > 65535)) {
    throw new Error('--port must be an integer between 1 and 65535');
  }
  if (positional.length === 0) positional.push('shell');
  if (positional[0] === 'workbench') {
    if (options.uiPort !== undefined && (!Number.isSafeInteger(options.uiPort) ||
        options.uiPort < 0 || options.uiPort > 65535)) {
      throw new Error('--ui-port must be an integer between 0 and 65535');
    }
    return startWorkbench(options, streams.output,
      (probe) => ensureLocalEngine(probe, { autoStart: options.autoStart !== false,
        output: streams.output }));
  }
  const client = new PacificDBClient(options);
  try {
    let ready = false;
    const ensureConnection = async () => {
      if (ready) return;
      await ensureLocalEngine(client, { autoStart: options.autoStart !== false,
        output: streams.output });
      ready = true;
    };
    if (positional[0] === 'ping') {
      await ensureConnection();
      printResponse(streams.output, await client.request({ action: 'ping' }), 'ping');
      return;
    }
    if (positional[0] === 'request') {
      await ensureConnection();
      const command = parseJson(positional.slice(1).join(' '));
      printResponse(streams.output, await client.request(command), command.action);
      return;
    }
    if (positional[0] === 'put-media' && positional.length === 4) {
      await ensureConnection();
      const [collection, id, filename] = positional.slice(1);
      const metadata = { ...(options.metadata || {}), filename: path.basename(filename),
        ...(options.contentType ? { contentType: options.contentType } : {}) };
      printResponse(streams.output,
        await client.putMedia(collection, id, await readFile(filename), metadata), 'insert');
      return;
    }
    if (positional[0] === 'get-media' && positional.length === 4) {
      await ensureConnection();
      const [collection, id, filename] = positional.slice(1);
      const media = await client.getMedia(collection, id);
      await writeFile(filename, media.data);
      streams.output.write(JSON.stringify({ status: 'ok', id, filename,
        sizeBytes: media.data.length }, null, 2) + '\n');
      return;
    }
    if (positional[0] === 'put-vector' && positional.length === 4) {
      await ensureConnection();
      const [collection, id, vector] = positional.slice(1);
      printResponse(streams.output, await client.putVector(
        collection, id, parseJson(vector), options.metadata || {}), 'insertVector');
      return;
    }
    if (positional[0] === 'query-vector' && positional.length === 3) {
      await ensureConnection();
      const [collection, vector] = positional.slice(1);
      printResponse(streams.output, await client.queryVector(
        collection, parseJson(vector), { k: options.k, metric: options.metric }), 'queryVector');
      return;
    }
    if (positional[0] !== 'shell')
      throw new Error('usage: pacificdb [shell|workbench|ping|request JSON|put-media|get-media|put-vector|query-vector] [options]');
    await runShell(client, streams, { ensureConnection });
  } finally { client.close(); }
}
