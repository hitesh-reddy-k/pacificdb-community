import readline from 'node:readline/promises';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { stdin, stdout } from 'node:process';
import { PacificDBClient } from '@pacificdb/client';

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
    else positional.push(args[i]);
  }
  const client = new PacificDBClient(options);
  if (positional[0] === 'ping') {
    streams.output.write(JSON.stringify(await client.request({ action: 'ping' }), null, 2) + '\n');
    return;
  }
  if (positional[0] === 'request') {
    const command = JSON.parse(positional.slice(1).join(' '));
    streams.output.write(JSON.stringify(await client.request(command), null, 2) + '\n');
    return;
  }
  if (positional[0] === 'put-media' && positional.length === 4) {
    const [collection, id, filename] = positional.slice(1);
    const metadata = { ...(options.metadata || {}), filename: path.basename(filename),
      ...(options.contentType ? { contentType: options.contentType } : {}) };
    streams.output.write(JSON.stringify(
      await client.putMedia(collection, id, await readFile(filename), metadata), null, 2) + '\n');
    return;
  }
  if (positional[0] === 'get-media' && positional.length === 4) {
    const [collection, id, filename] = positional.slice(1);
    const media = await client.getMedia(collection, id);
    await writeFile(filename, media.data);
    streams.output.write(JSON.stringify({ status: 'ok', id, filename,
      sizeBytes: media.data.length }, null, 2) + '\n');
    return;
  }
  if (positional[0] === 'put-vector' && positional.length === 4) {
    const [collection, id, vector] = positional.slice(1);
    streams.output.write(JSON.stringify(await client.putVector(
      collection, id, JSON.parse(vector), options.metadata || {}), null, 2) + '\n');
    return;
  }
  if (positional[0] === 'query-vector' && positional.length === 3) {
    const [collection, vector] = positional.slice(1);
    streams.output.write(JSON.stringify(await client.queryVector(
      collection, JSON.parse(vector), { k: options.k, metric: options.metric }), null, 2) + '\n');
    return;
  }
  if (positional[0] !== 'shell')
    throw new Error('usage: pacificdb ping|request JSON|shell|put-media|get-media|put-vector|query-vector [options]');
  const prompt = readline.createInterface(streams);
  while (true) {
    const line = (await prompt.question('pacificdb> ')).trim();
    if (!line || line === 'exit' || line === 'quit') break;
    try {
      streams.output.write(JSON.stringify(await client.request(JSON.parse(line)), null, 2) + '\n');
    } catch (error) {
      streams.output.write('error: ' + error.message + '\n');
    }
  }
  prompt.close();
}
