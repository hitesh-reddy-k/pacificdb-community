import { spawnSync } from 'node:child_process';

export async function desktopOperation(desktop, action, phase, timeoutMs = 45_000) {
  console.log(`[workbench-desktop] ${phase}`);
  let deadline;
  try {
    return await Promise.race([Promise.resolve().then(action), new Promise((_, reject) => {
      deadline = setTimeout(() => {
        const child = desktop.process();
        if (child.exitCode === null && child.signalCode === null) {
          if (process.platform === 'win32') spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'],
            { windowsHide: true, timeout: 5000 });
          else {
            try { process.kill(-child.pid, 'SIGKILL'); }
            catch { child.kill('SIGKILL'); }
          }
        }
        reject(new Error(`${phase} did not complete within ${timeoutMs} ms`));
      }, timeoutMs);
    })]);
  } finally { clearTimeout(deadline); }
}

export async function closeDesktopApplication(desktop, timeoutMs = 45_000, primaryError) {
  const child = desktop.process();
  if (child.exitCode !== null || child.signalCode !== null) return;
  try {
    await desktopOperation(desktop, () => desktop.close(), 'graceful shutdown', timeoutMs);
  } catch (error) {
    if (!primaryError) throw error;
    console.error('[workbench-desktop] cleanup also failed:', error);
  }
}
