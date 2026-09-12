import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { stdin, stdout } from 'node:process';
import { PacificDBClient } from '@pacificdb/client';
import { SHELL_HELP, printResponse, runShell } from './shell.js';
import { ensureLocalEngine } from './local-engine.js';

export async function main(args, streams = { input: stdin, output: stdout }) {
  const options = {};
  const positional = [];
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--host') options.host = args[++i];
    else if (args[i] === '--port') options.port = Number(args[++i]);
    else if (args[i] === '--database') options.database = args[++i];
    else if (args[i] === '--content-type') options.contentType = args[++i];
    else if (args[i] === '--metadata') options.metadata = JSON.parse(args[++i]);
    else if (args[i] === '--k') options.k = Number(args[++i]);
    else if (args[i] === '--metric') options.metric = args[++i];
    else if (args[i] === '--no-start') options.autoStart = false;
    else if (args[i] === '--help' || args[i] === '-h') options.help = true;
    else positional.push(args[i]);
  }
  if (options.help) {
    streams.output.write('Usage: pacificdb [shell|ping|request|put-media|get-media|put-vector|query-vector] [options]\n\n');
    streams.output.write(SHELL_HELP);
    return;
  }
  if (positional.length === 0) positional.push('shell');
  const client = new PacificDBClient(options);
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
    const command = JSON.parse(positional.slice(1).join(' '));
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
      collection, id, JSON.parse(vector), options.metadata || {}), 'insertVector');
    return;
  }
  if (positional[0] === 'query-vector' && positional.length === 3) {
    await ensureConnection();
    const [collection, vector] = positional.slice(1);
    printResponse(streams.output, await client.queryVector(
      collection, JSON.parse(vector), { k: options.k, metric: options.metric }), 'queryVector');
    return;
  }
  if (positional[0] !== 'shell')
    throw new Error('usage: pacificdb [shell|ping|request JSON|put-media|get-media|put-vector|query-vector] [options]');
  await runShell(client, streams, { ensureConnection });
}
