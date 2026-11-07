import { constants } from 'node:fs';
import { access, lstat, mkdir, open, readlink, symlink } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { homedir } from 'node:os';
import path from 'node:path';

const execute = promisify(execFile);
const quote = value => `'${value.replaceAll("'", "'\\''")}'`;

export async function installTerminalCommand({ executable, home = homedir(),
  platform = process.platform, shell = process.env.SHELL || (process.platform === 'darwin' ? '/bin/zsh' : '/bin/bash'), run = execute }) {
  if (!path.isAbsolute(executable) || !path.isAbsolute(home)) throw new Error('Absolute installation paths are required');
  await access(executable, platform === 'win32' ? constants.F_OK : constants.X_OK);
  if (platform === 'win32') {
    const powershell = path.join(process.env.SystemRoot || 'C:\\Windows',
      'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
    await run(powershell, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
      '-File', path.join(path.dirname(executable), 'pacificdb-path.ps1'), 'add', path.dirname(executable)],
    { shell: false, windowsHide: true, timeout: 15000 });
    return { command: executable };
  }
  const shellName = path.basename(shell);
  let profiles = shellName === 'zsh' ? ['.zprofile', '.zshrc'] : shellName === 'bash'
    ? [platform === 'darwin' ? '.bash_profile' : '.profile', '.bashrc'] : shellName === 'sh'
      ? ['.profile'] : null;
  if (shellName === 'bash') {
    for (const login of ['.bash_profile', '.bash_login', '.profile']) {
      try { await lstat(path.join(home, login)); profiles = [login, '.bashrc']; break; }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
  }
  if (!profiles) throw new Error('Automatic terminal setup supports bash, zsh and sh. Add the bundled engine directory to your shell PATH manually.');
  const bin = path.join(home, '.local', 'bin');
  const command = path.join(bin, 'pacificdb');
  const block = `\n# PacificDB terminal command\nexport PATH=${quote(bin)}:"$PATH"\n`;
  const updates = [];
  for (const name of profiles) {
    const file = path.join(home, name);
    try {
      // Follow neither profile symlinks nor non-regular files. Preserve all user settings.
      const handle = await open(file, constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK);
      try {
        if (!(await handle.stat()).isFile()) throw new Error('not a regular file');
        const text = await handle.readFile('utf8');
        if (!text.includes(block)) updates.push(file);
      } finally { await handle.close(); }
    } catch (error) {
      if (error.code === 'ENOENT') updates.push(file);
      else throw new Error(`Terminal profile must be a regular file: ${file}`, { cause: error });
    }
  }
  await mkdir(bin, { recursive: true });
  try {
    await symlink(executable, command);
  } catch (error) {
    if (error.code !== 'EEXIST') throw error;
    if (!(await lstat(command)).isSymbolicLink() || await readlink(command) !== executable) {
      throw new Error(`A different command already exists at ${command}; it was not changed`);
    }
  }
  for (const file of updates) {
    const handle = await open(file, constants.O_APPEND | constants.O_CREAT | constants.O_WRONLY | constants.O_NOFOLLOW, 0o600);
    try {
      if (!(await handle.stat()).isFile()) throw new Error(`Terminal profile must be a regular file: ${file}`);
      await handle.writeFile(block);
    } finally { await handle.close(); }
  }
  return { command, profiles: profiles.map(name => path.join(home, name)) };
}
