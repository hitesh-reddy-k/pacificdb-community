import readline from 'node:readline/promises';
import { stdin, stdout } from 'node:process';
import { PacificDBClient } from '../../sdk/node/src/index.js';

export async function main(args, streams = { input: stdin, output: stdout }) {
  const options = {};
  const positional = [];
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--host') options.host = args[++i];
    else if (args[i] === '--port') options.port = Number(args[++i]);
    else if (args[i] === '--database') options.database = args[++i];
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
  if (positional[0] !== 'shell')
    throw new Error('usage: pacificdb ping|request JSON|shell [--host H --port P --database DB]');
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
