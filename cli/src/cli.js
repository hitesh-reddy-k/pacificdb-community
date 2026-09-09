import readline from 'node:readline/promises';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { stdin, stdout } from 'node:process';
import { PacificDBClient } from '@pacificdb/client';

const shellHelp = `Shell commands:
  help                         Show this help
  quit | exit                  Close the shell
  {"action":"ping"}          Check the server
  {"action":"createDatabase","dbName":"app"}
  {"action":"createCollection","dbName":"app","collection":"users"}
  {"action":"insert","dbName":"app","collection":"users","data":{"id":"1","name":"Ada"}}
  {"action":"find","dbName":"app","collection":"users","filter":{}}
`;

function printResponse(stream, response, action = '') {
  if (response && !Array.isArray(response) && typeof response === 'object' &&
      !action.startsWith('admin_')) {
    for (const key of Object.keys(response)) {
      if (key.startsWith('_') || ['requestId', 'trace_id', 'traceparent', 'term', 'isLeader',
          'leader_term', 'commit_index', 'last_applied', 'consistency_mode',
          'consistency_semantics', 'client_session_id', 'last_seen_version',
          'minimum_visible_version'].includes(key)) delete response[key];
    }
  }
  stream.write(JSON.stringify(response, null, 2) + '\n');
}

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
    printResponse(streams.output, await client.request({ action: 'ping' }), 'ping');
    return;
  }
  if (positional[0] === 'request') {
    const command = JSON.parse(positional.slice(1).join(' '));
    printResponse(streams.output, await client.request(command), command.action);
    return;
  }
  if (positional[0] === 'put-media' && positional.length === 4) {
    const [collection, id, filename] = positional.slice(1);
    const metadata = { ...(options.metadata || {}), filename: path.basename(filename),
      ...(options.contentType ? { contentType: options.contentType } : {}) };
    printResponse(streams.output,
      await client.putMedia(collection, id, await readFile(filename), metadata), 'insert');
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
    printResponse(streams.output, await client.putVector(
      collection, id, JSON.parse(vector), options.metadata || {}), 'insertVector');
    return;
  }
  if (positional[0] === 'query-vector' && positional.length === 3) {
    const [collection, vector] = positional.slice(1);
    printResponse(streams.output, await client.queryVector(
      collection, JSON.parse(vector), { k: options.k, metric: options.metric }), 'queryVector');
    return;
  }
  if (positional[0] !== 'shell')
    throw new Error('usage: pacificdb ping|request JSON|shell|put-media|get-media|put-vector|query-vector [options]');
  const prompt = readline.createInterface(streams);
  streams.output.write('PacificDB shell. Type help for commands; quit to exit.\n');
  while (true) {
    const line = (await prompt.question('pacificdb> ')).trim();
    if (!line || line === 'exit' || line === 'quit') break;
    if (line === 'help') {
      streams.output.write(shellHelp);
      continue;
    }
    try {
      const command = JSON.parse(line);
      printResponse(streams.output, await client.request(command), command.action);
    } catch (error) {
      streams.output.write('error: ' + error.message + '\n');
    }
  }
  prompt.close();
}
