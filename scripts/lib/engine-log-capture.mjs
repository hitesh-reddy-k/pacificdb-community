import { once } from 'node:events';

export function captureEngineLogs(child, stderrLog, stdoutLog) {
  // Combining pipes can place stdout between a stderr JSON event and its newline.
  child.stdout.pipe(stdoutLog, { end: false });
  child.stderr.pipe(stderrLog, { end: false });
  const closed = new Promise((resolve) => child.once('close', resolve));
  return async () => {
    await closed;
    const finished = Promise.all([stderrLog, stdoutLog].map((log) => once(log, 'finish')));
    stderrLog.end();
    stdoutLog.end();
    await finished;
  };
}
