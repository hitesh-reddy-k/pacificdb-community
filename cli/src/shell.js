import readline from 'node:readline/promises';
import { appendFile, mkdir, readFile, rename, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

export const SHELL_HELP = `Authentication
  login <username>                     Sign in
  whoami                              Show current identity
  logout                              Clear the current credential

Projects
  create project <name>               Create project
  list projects                       List projects
  use project <id>                    Switch project
  show project                        Show project
  delete project <id>                 Delete project

Databases and queries
  create database <name>              Create database
  list databases                      List databases
  use <name>                          Switch database
  show database                       Show database
  drop database <name>                Drop database
  create collection <name>            Create collection
  list collections                    List collections
  insert <collection> <json>          Insert document
  find <collection> [filter-json]     Find documents
  findOne <collection> [filter-json]  Find one document
  update <collection> <filter> <json> Update one document
  delete <collection> <filter-json>   Delete one document
  aggregate <collection> <pipeline>   Run bounded aggregation
  count <collection> [filter-json]    Count documents
  explain <collection> [filter-json]  Explain a find

Backups
  create backup [--name <name>]       Create manual backup
  list backups                        List backups
  show backup <id>                    Show backup
  restore backup <id>                 Restore synchronously
  list restores                       List restore attempts
  delete backup <id>                  Delete backup
  backup verify <id>                  Verify backup
  backup export <id> [file]           Export manifest

Security / API keys
  create api-key [--name <n>] [--role read|readwrite|admin]
  list api-keys                       List keys
  show api-key <id>                   Show key metadata
  revoke api-key <id>                 Revoke key

Media
  upload image|video|media <path> [--collection <name>] [--resume <id>]
  download media <id> <path>          Download and verify media
  list media [--all]                  List ready or all media
  find media <query>                  Find media metadata
  show media <id>                     Show media metadata
  delete media <id>                   Delete media and chunks
  media cleanup <id>                  Delete one incomplete upload

Vectors
  put vector <collection> <id> <json-vector>
  query vector <collection> <json-vector> [--k <n>] [--metric <name>]

System
  help [topic]                        Show help
  context show                        Show context
  context clear                       Clear context and credentials
  status                              Show connection status
  history                             Show command history
  clear                               Clear screen
  request <json>                      Send a raw request
  exit | quit                         Exit shell
`;

export function sanitizeResponse(value, action = '') {
  const response = structuredClone(value);
  if (!response || Array.isArray(response) || typeof response !== 'object' ||
      action.startsWith('admin_')) return response;
  for (const key of Object.keys(response)) {
    if (key.startsWith('_') || ['requestId', 'trace_id', 'traceparent', 'term',
      'isLeader', 'leader_term', 'commit_index', 'last_applied',
      'consistency_mode', 'consistency_semantics', 'client_session_id',
      'last_seen_version', 'minimum_visible_version', 'returned_doc_version',
      'sst_visibility_source'].includes(key)) delete response[key];
  }
  const strip = (document) => {
    if (!document || Array.isArray(document) || typeof document !== 'object') return;
    for (const key of ['_mvcc_commit_ms', '_mvcc_version', '_raft_commit_index',
      '_raft_term', '_visibility_floor', '_visibility_state',
      '_logicalWritePayloadHash', 'created_at_ms', 'created_txn', 'deleted_at_ms',
      'deleted_txn', 'version', 'committed', 'tenant_id']) delete document[key];
  };
  if (Array.isArray(response.data)) response.data.forEach(strip);
  else strip(response.data);
  return response;
}

export function printResponse(stream, response, action = '') {
  stream.write(JSON.stringify(sanitizeResponse(response, action), null, 2) + '\n');
}

function words(text) {
  const result = [];
  let token = '';
  let quote = '';
  for (const character of text.trim()) {
    if (quote) {
      if (character === quote) quote = '';
      else token += character;
    } else if (character === '"' || character === "'") quote = character;
    else if (/\s/.test(character)) {
      if (token) { result.push(token); token = ''; }
    } else token += character;
  }
  if (quote) throw new Error('unterminated quote');
  if (token) result.push(token);
  return result;
}

function jsonValues(text, count) {
  const values = [];
  let rest = text.trim();
  for (let valueIndex = 0; valueIndex < count; valueIndex += 1) {
    if (!rest || !['{', '['].includes(rest[0])) throw new Error('JSON value required');
    const opening = rest[0];
    const closing = opening === '{' ? '}' : ']';
    let depth = 0;
    let quoted = false;
    let escaped = false;
    let end = -1;
    for (let i = 0; i < rest.length; i += 1) {
      const character = rest[i];
      if (quoted) {
        if (escaped) escaped = false;
        else if (character === '\\') escaped = true;
        else if (character === '"') quoted = false;
      } else if (character === '"') quoted = true;
      else if (character === opening) depth += 1;
      else if (character === closing && --depth === 0) { end = i + 1; break; }
    }
    if (end < 0) throw new Error('incomplete JSON value');
    values.push(JSON.parse(rest.slice(0, end)));
    rest = rest.slice(end).trim();
  }
  return { values, rest };
}

function requireDatabase(context) {
  if (!context.database) throw new Error('select a database with: use <name>');
}

function flag(tokens, name, fallback = undefined) {
  const index = tokens.indexOf(name);
  if (index < 0) return fallback;
  if (!tokens[index + 1]) throw new Error(`${name} requires a value`);
  return tokens[index + 1];
}

export function parseShellCommand(line, context = {}) {
  const text = line.trim();
  if (!text) return { kind: 'empty' };
  if (text.startsWith('{')) return { kind: 'request', command: JSON.parse(text) };
  if (text.startsWith('request ')) {
    return { kind: 'request', command: JSON.parse(text.slice(8)) };
  }
  if (text === 'exit' || text === 'quit') return { kind: 'exit' };
  if (text === 'help' || text.startsWith('help ')) return { kind: 'help' };
  if (text === 'clear') return { kind: 'clear' };
  if (text === 'history') return { kind: 'history' };
  if (text === 'context show') return { kind: 'contextShow' };
  if (text === 'context clear') return { kind: 'contextClear' };
  if (text === 'status') return { kind: 'request', command: { action: 'ping' } };
  if (text === 'logout') return { kind: 'logout' };
  if (text.startsWith('login ')) return { kind: 'login', username: text.slice(6).trim() };
  if (text === 'whoami') return { kind: 'request', command: { action: 'security_whoami' } };

  let match;
  if ((match = text.match(/^create project (.+)$/)))
    return { kind: 'request', command: { action: 'community_project_create', name: match[1] } };
  if (text === 'list projects')
    return { kind: 'request', command: { action: 'community_project_list' } };
  if ((match = text.match(/^use project (\S+)$/)))
    return { kind: 'useProject', id: match[1] };
  if (text === 'show project') {
    if (!context.projectId) throw new Error('no project selected');
    return { kind: 'request', command: { action: 'community_project_get', id: context.projectId } };
  }
  if ((match = text.match(/^delete project(?: (\S+))?$/))) {
    const id = match[1] || context.projectId;
    if (!id) throw new Error('project id required');
    return { kind: 'request', command: { action: 'community_project_delete', id }, clearProject: id };
  }

  if ((match = text.match(/^create database (\S+)$/)))
    return { kind: 'createDatabase', name: match[1] };
  if (text === 'list databases')
    return { kind: 'request', command: { action: 'listDatabases' } };
  if ((match = text.match(/^use (\S+)$/))) return { kind: 'useDatabase', name: match[1] };
  if (text === 'show database') {
    requireDatabase(context);
    return { kind: 'showDatabase' };
  }
  if ((match = text.match(/^drop database (\S+)$/)))
    return { kind: 'request', command: { action: 'dropDatabase', dbName: match[1] }, clearDatabase: match[1] };
  if ((match = text.match(/^create collection (\S+)$/))) {
    requireDatabase(context);
    return { kind: 'request', command: { action: 'createCollection', collection: match[1] } };
  }
  if (text === 'list collections') {
    requireDatabase(context);
    return { kind: 'request', command: { action: 'listCollections' } };
  }

  if ((match = text.match(/^find media (.+)$/))) return { kind: 'mediaFind', query: match[1] };
  if ((match = text.match(/^delete media (\S+)$/)))
    return { kind: 'request', command: { action: 'community_media_delete', media_id: match[1] } };
  if ((match = text.match(/^delete backup (\S+)$/)))
    return { kind: 'request', command: { action: 'delete_backup', backup_id: match[1] } };

  if ((match = text.match(/^insert (\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    return { kind: 'request', command: { action: 'insert', collection: match[1], data: JSON.parse(match[2]) } };
  }
  if ((match = text.match(/^(findOne|find|count|explain) (\S+)(?:\s+([\s\S]+))?$/))) {
    requireDatabase(context);
    const action = match[1];
    const filter = match[3] ? JSON.parse(match[3]) : {};
    return { kind: 'request', command: { action: action === 'findOne' ? 'find' : action,
      collection: match[2], filter, ...(action === 'findOne' ? { limit: 1 } : {}) } };
  }
  if ((match = text.match(/^update (\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    const parsed = jsonValues(match[2], 2);
    if (parsed.rest) throw new Error('unexpected text after update document');
    return { kind: 'request', command: { action: 'updateOne', collection: match[1],
      filter: parsed.values[0], update: parsed.values[1] } };
  }
  if ((match = text.match(/^delete (\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    return { kind: 'request', command: { action: 'deleteOne', collection: match[1],
      filter: JSON.parse(match[2]) } };
  }
  if ((match = text.match(/^aggregate (\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    return { kind: 'request', command: { action: 'aggregate', collection: match[1],
      pipeline: JSON.parse(match[2]) } };
  }

  if ((match = text.match(/^create backup(?: --name (.+))?$/)))
    return { kind: 'request', command: { action: 'create_backup', description: match[1] || 'manual backup' } };
  if (text === 'list backups') return { kind: 'request', command: { action: 'list_backups' } };
  if ((match = text.match(/^show backup (\S+)$/)))
    return { kind: 'request', command: { action: 'get_backup', backup_id: match[1] } };
  if ((match = text.match(/^restore backup (\S+)$/)))
    return { kind: 'request', command: { action: 'restore_backup', backup_id: match[1] } };
  if (text === 'list restores') return { kind: 'request', command: { action: 'list_restores' } };
  if ((match = text.match(/^backup verify (\S+)$/)))
    return { kind: 'request', command: { action: 'verify_backup', backup_id: match[1] } };
  if ((match = text.match(/^backup export (\S+)(?:\s+(.+))?$/)))
    return { kind: 'backupExport', id: match[1], filename: match[2] };

  if (text.startsWith('create api-key')) {
    const tokens = words(text);
    return { kind: 'request', command: { action: 'api_key_create',
      name: flag(tokens, '--name', 'default'), role: flag(tokens, '--role', 'readwrite') } };
  }
  if (text === 'list api-keys') return { kind: 'request', command: { action: 'api_key_list' } };
  if ((match = text.match(/^show api-key (\S+)$/)))
    return { kind: 'request', command: { action: 'api_key_get', id: match[1] } };
  if ((match = text.match(/^revoke api-key (\S+)$/)))
    return { kind: 'request', command: { action: 'api_key_revoke', id: match[1] } };

  if ((match = text.match(/^upload (image|video|media)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    const tokens = words(match[2]);
    return { kind: 'mediaUpload', filename: tokens[0],
      collection: flag(tokens, '--collection', 'media'), resume: flag(tokens, '--resume'),
      mediaKind: match[1] };
  }
  if ((match = text.match(/^download media (\S+)\s+(.+)$/)))
    return { kind: 'mediaDownload', id: match[1], filename: match[2] };
  if (text === 'list media' || text === 'list media --all')
    return { kind: 'request', command: { action: 'community_media_list', all: text.endsWith('--all') } };
  if ((match = text.match(/^show media (\S+)$/)))
    return { kind: 'request', command: { action: 'community_media_get', media_id: match[1] } };
  if ((match = text.match(/^media cleanup (\S+)$/)))
    return { kind: 'request', command: { action: 'community_media_cleanup', media_id: match[1] } };

  if ((match = text.match(/^put vector (\S+)\s+(\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    return { kind: 'vectorPut', collection: match[1], id: match[2], vector: JSON.parse(match[3]) };
  }
  if ((match = text.match(/^query vector (\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    const parsed = jsonValues(match[2], 1);
    const tokens = words(parsed.rest);
    return { kind: 'vectorQuery', command: { action: 'queryVector',
      collection: match[1], vector: parsed.values[0],
      k: Number(flag(tokens, '--k', 10)), metric: flag(tokens, '--metric', 'cosine') } };
  }
  throw new Error(`unknown command: ${text}`);
}

function defaultCliHome() {
  if (process.env.PACIFICDB_CLI_HOME) return process.env.PACIFICDB_CLI_HOME;
  if (process.platform === 'win32') return path.join(process.env.LOCALAPPDATA || os.homedir(), 'PacificDB');
  if (process.platform === 'darwin') return path.join(os.homedir(), 'Library', 'Application Support', 'PacificDB');
  return path.join(process.env.XDG_STATE_HOME || path.join(os.homedir(), '.local', 'state'), 'pacificdb');
}

async function loadContext(home) {
  try { return JSON.parse(await readFile(path.join(home, 'context.json'), 'utf8')); }
  catch { return {}; }
}

async function saveContext(home, context) {
  await mkdir(home, { recursive: true, mode: 0o700 });
  const file = path.join(home, 'context.json');
  const temporary = file + '.tmp';
  await writeFile(temporary, JSON.stringify(context, null, 2), { mode: 0o600 });
  await rename(temporary, file);
}

function safeHistory(line) {
  return !/(password|"token"\s*:|pdb_[0-9a-f]{12}_)/i.test(line);
}

export async function runShell(client, streams, options = {}) {
  const home = options.cliHome || defaultCliHome();
  const context = await loadContext(home);
  if (!client.database && context.database) client.database = context.database;
  if (!client.token && context.token) client.token = context.token;
  const prompt = readline.createInterface(streams);
  const lines = prompt[Symbol.asyncIterator]();
  streams.output.write('  ≋ PacificDB Community\nType help for commands.\n');
  while (true) {
    streams.output.write('pacificdb> ');
    const next = await lines.next();
    if (next.done) break;
    const line = next.value.trim();
    if (!line) continue;
    try {
      const parsed = parseShellCommand(line, context);
      if (parsed.kind === 'exit') break;
      if (safeHistory(line)) {
        await mkdir(home, { recursive: true, mode: 0o700 });
        await appendFile(path.join(home, 'history'), line + '\n', { mode: 0o600 });
      }
      if (parsed.kind === 'help') streams.output.write(SHELL_HELP);
      else if (parsed.kind === 'clear') streams.output.write('\x1b[2J\x1b[H');
      else if (parsed.kind === 'history') {
        streams.output.write(await readFile(path.join(home, 'history'), 'utf8').catch(() => ''));
      } else if (parsed.kind === 'contextShow') {
        printResponse(streams.output, { database: context.database || null,
          projectId: context.projectId || null, authenticated: Boolean(client.token) });
      } else if (parsed.kind === 'contextClear' || parsed.kind === 'logout') {
        delete context.database;
        delete context.projectId;
        delete context.token;
        client.database = '';
        client.token = '';
        await saveContext(home, context);
        printResponse(streams.output, { status: 'ok' });
      } else if (parsed.kind === 'login') {
        if (!parsed.username) throw new Error('username required');
        streams.output.write('Password: \x1b[8m');
        let password = '';
        try {
          const answer = await lines.next();
          if (answer.done) throw new Error('password required');
          password = answer.value;
        }
        finally { streams.output.write('\x1b[0m\n'); }
        const response = await client.authenticate(parsed.username, password);
        context.token = client.token;
        await saveContext(home, context);
        printResponse(streams.output, { status: 'ok', username: response.username,
          role: response.role }, 'security_authenticate');
      } else if (parsed.kind === 'useProject') {
        context.projectId = parsed.id;
        await saveContext(home, context);
        printResponse(streams.output, { status: 'ok', projectId: parsed.id });
      } else if (parsed.kind === 'useDatabase') {
        context.database = parsed.name;
        client.database = parsed.name;
        await saveContext(home, context);
        printResponse(streams.output, { status: 'ok', database: parsed.name });
      } else if (parsed.kind === 'createDatabase') {
        const response = await client.request({ action: 'createDatabase', dbName: parsed.name });
        if (context.projectId) await client.request({ action: 'community_database_map',
          database: parsed.name, project_id: context.projectId });
        printResponse(streams.output, response, 'createDatabase');
      } else if (parsed.kind === 'showDatabase') {
        const response = await client.request({ action: 'listCollections' });
        printResponse(streams.output, { status: 'ok', database: client.database,
          collections: response.collections ?? response }, 'listCollections');
      } else if (parsed.kind === 'backupExport') {
        const response = await client.request({ action: 'export_backup_manifest',
          backup_id: parsed.id });
        const filename = parsed.filename || `${parsed.id}.json`;
        await writeFile(filename, JSON.stringify(response.backup, null, 2) + '\n', { mode: 0o600 });
        printResponse(streams.output, { status: 'ok', backup_id: parsed.id, filename });
      } else if (parsed.kind === 'mediaUpload') {
        const media = await client.uploadMediaFile(parsed.collection, parsed.filename,
          { resume: parsed.resume });
        printResponse(streams.output, media, 'community_media_finalize');
      } else if (parsed.kind === 'mediaDownload') {
        printResponse(streams.output,
          await client.downloadMediaFile(parsed.id, parsed.filename));
      } else if (parsed.kind === 'mediaFind') {
        const response = await client.request({ action: 'community_media_list', all: true });
        const needle = parsed.query.toLowerCase();
        const media = (response.media || []).filter((item) =>
          [item.id, item.filename, item.content_type, item.collection]
            .some((value) => String(value || '').toLowerCase().includes(needle)));
        printResponse(streams.output, { status: 'ok', count: media.length, media });
      } else if (parsed.kind === 'vectorPut') {
        printResponse(streams.output, await client.putVector(parsed.collection,
          parsed.id, parsed.vector), 'insertVector');
      } else if (parsed.kind === 'vectorQuery') {
        printResponse(streams.output, await client.queryVector(parsed.command.collection,
          parsed.command.vector, { k: parsed.command.k, metric: parsed.command.metric }),
        'queryVector');
      } else if (parsed.kind === 'request') {
        const response = await client.request(parsed.command);
        if (parsed.clearProject && context.projectId === parsed.clearProject) {
          delete context.projectId;
          await saveContext(home, context);
        }
        if (parsed.clearDatabase && context.database === parsed.clearDatabase) {
          delete context.database;
          client.database = '';
          await saveContext(home, context);
        }
        printResponse(streams.output, response, parsed.command.action);
      }
    } catch (error) {
      streams.output.write('error: ' + error.message + '\n');
    }
  }
  prompt.close();
}
