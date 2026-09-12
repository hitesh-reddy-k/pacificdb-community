#!/usr/bin/env node
import { main } from '../src/cli.js';

for (const stream of [process.stdout, process.stderr]) {
  stream.on('error', (error) => {
    if (error.code === 'EPIPE') process.exit(0);
    throw error;
  });
}

main(process.argv.slice(2)).catch((error) => {
  process.stderr.write(error.message + '\n');
  process.exitCode = 1;
});
