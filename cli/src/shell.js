import readline from 'node:readline/promises';
import { appendFile, mkdir, mkdtemp, readFile, rename, rm, writeFile, chmod } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { MediaUploadError } from '@pacificdb/client';

const shellVersion = JSON.parse(await readFile(new URL('../package.json', import.meta.url), 'utf8')).version;

function parseJson(text) {
  try { return JSON.parse(text); }
  catch { throw new Error('invalid JSON'); }
}

export const SHELL_HELP = `Databases and queries
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
  backup export <id> [file]           Export complete backup JSON

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
  context clear                       Clear database context
  status                              Show connection status
  history                             Show command history
  clear                               Clear screen
  request <json>                      Send a raw request
  exit | quit                         Exit shell
`;

// A terminal-safe interpretation of the PacificDB ring-and-waves mark. Keeping
// this in text makes the identity work over SSH, redirected output, and shells
// that do not support terminal image protocols.
export const SHELL_BANNER = `
             .--------.
          .-'          '-.
         /                \\
         \\____        ____/
       ~~~~~~~~\\______/~~~~~~~~
         ~~~~~~~~~~~~~~~~~~~~
             PacificDB
               v${shellVersion}
       Documents · Vectors · Media
  Type help to see commands.\n`;

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
    values.push(parseJson(rest.slice(0, end)));
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
  if (text.startsWith('{')) return { kind: 'request', command: parseJson(text) };
  if (text.startsWith('request ')) {
    return { kind: 'request', command: parseJson(text.slice(8)) };
  }
  if (text === 'exit' || text === 'quit') return { kind: 'exit' };
  if (text === 'help' || text.startsWith('help ')) return { kind: 'help' };
  if (text === 'clear') return { kind: 'clear' };
  if (text === 'history') return { kind: 'history' };
  if (text === 'context show') return { kind: 'contextShow' };
  if (text === 'context clear') return { kind: 'contextClear' };
  if (text === 'status') return { kind: 'request', command: { action: 'ping' } };
  let match;
  if (/^(create|list|use|show|delete) projects?(?:\s|$)/.test(text)) {
    throw new Error('unknown command; use databases directly or request JSON for legacy APIs');
  }
  if ((match = text.match(/^create database (\S+)$/))) {
    return { kind: 'createDatabase', name: match[1] };
  }
  if (text === 'list databases') {
    return { kind: 'request', command: { action: 'listDatabases' } };
  }
  if ((match = text.match(/^use (\S+)$/))) {
    return { kind: 'useDatabase', name: match[1] };
  }
  if (text === 'show database') {
    requireDatabase(context);
    return { kind: 'showDatabase' };
  }
  if ((match = text.match(/^drop database (\S+)$/))) {
    return { kind: 'request', command: { action: 'dropDatabase', dbName: match[1] }, clearDatabase: match[1] };
  }
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
    return { kind: 'request', command: { action: 'insert', collection: match[1], data: parseJson(match[2]) } };
  }
  if ((match = text.match(/^(findOne|find|count|explain) (\S+)(?:\s+([\s\S]+))?$/))) {
    requireDatabase(context);
    const action = match[1];
    const filter = match[3] ? parseJson(match[3]) : {};
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
      filter: parseJson(match[2]) } };
  }
  if ((match = text.match(/^aggregate (\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    return { kind: 'request', command: { action: 'aggregate', collection: match[1],
      pipeline: parseJson(match[2]) } };
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
    return { kind: 'vectorPut', collection: match[1], id: match[2], vector: parseJson(match[3]) };
  }
  if ((match = text.match(/^query vector (\S+)\s+([\s\S]+)$/))) {
    requireDatabase(context);
    const parsed = jsonValues(match[2], 1);
    const tokens = words(parsed.rest);
    return { kind: 'vectorQuery', command: { action: 'queryVector',
      collection: match[1], vector: parsed.values[0],
      k: Number(flag(tokens, '--k', 10)), metric: flag(tokens, '--metric', 'cosine') } };
  }
  throw new Error('unknown command; type help for available commands');
}

function defaultCliHome() {
  if (process.env.PACIFICDB_CLI_HOME) return process.env.PACIFICDB_CLI_HOME;
  if (process.platform === 'win32') return path.join(process.env.LOCALAPPDATA || os.homedir(), 'PacificDB');
  if (process.platform === 'darwin') return path.join(os.homedir(), 'Library', 'Application Support', 'PacificDB');
  return path.join(process.env.XDG_STATE_HOME || path.join(os.homedir(), '.local', 'state'), 'pacificdb');
}

async function loadContext(home) {
  let context;
  try {
    context = parseJson(await readFile(path.join(home, 'context.json'), 'utf8'));
  }
  catch { return {}; }
  if (!context || Array.isArray(context) || typeof context !== 'object') return {};
  const safe = typeof context.database === 'string' && context.database &&
    !/[\x00-\x1f\x7f]/.test(context.database) ? { database: context.database } : {};
  if (JSON.stringify(context) !== JSON.stringify(safe)) await saveContext(home, safe);
  else if (process.platform !== 'win32') await chmod(path.join(home, 'context.json'), 0o600);
  return safe;
}

async function saveContext(home, context) {
  await mkdir(home, { recursive: true, mode: 0o700 });
  const file = path.join(home, 'context.json');
  const directory = await mkdtemp(path.join(home, '.context-'));
  try {
    const temporary = path.join(directory, 'context.json');
    await writeFile(temporary, JSON.stringify(context.database ? { database: context.database } : {}, null, 2),
      { mode: 0o600, flag: 'wx' });
    await rename(temporary, file);
  } finally { await rm(directory, { recursive: true, force: true }); }
}

function safeHistory(line) {
  return !/(password|\"token\"\s*:|pdb_[0-9a-f]{12}_|pacificdbs?:\/\/)/i.test(line);
}

const DATABASE_ACTIONS = new Set(['createCollection', 'listCollections', 'insert',
  'find', 'updateOne', 'deleteOne', 'aggregate', 'count', 'explain', 'queryVector']);

export async function runShell(client, streams, options = {}) {
  const home = options.cliHome || defaultCliHome();
  const context = await loadContext(home);
  let needsValidation = !client.database && !!context.database;
  if (client.database) context.database = client.database;
  else if (context.database) client.database = context.database;
  const prompt = readline.createInterface(streams);
  const lines = prompt[Symbol.asyncIterator]();
  streams.output.write(SHELL_BANNER);
  while (true) {
    streams.output.write(context.database ? `pacificdb:${context.database}> ` : 'pacificdb> ');
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
        if (process.platform !== 'win32') await chmod(path.join(home, 'history'), 0o600);
      }
      if (options.ensureConnection && !['help', 'clear', 'history', 'contextShow',
        'contextClear'].includes(parsed.kind)) await options.ensureConnection();
      if (needsValidation && (['showDatabase', 'mediaUpload', 'mediaDownload',
          'mediaFind', 'vectorPut', 'vectorQuery'].includes(parsed.kind) ||
          (DATABASE_ACTIONS.has(parsed.command?.action) && !parsed.command.dbName))) {
        const databases = await client.request({ action: 'listDatabases' });
        if (!Array.isArray(databases) || !databases.includes(context.database)) throw new Error('database_not_found');
        needsValidation = false;
      }
      if (parsed.kind === 'help') streams.output.write(SHELL_HELP);
      else if (parsed.kind === 'clear') streams.output.write('\x1b[2J\x1b[H');
      else if (parsed.kind === 'history') {
        streams.output.write((await readFile(path.join(home, 'history'), 'utf8').catch(() => '')).split('\n').filter(safeHistory).join('\n'));
      } else if (parsed.kind === 'contextShow') {
        printResponse(streams.output, { database: context.database || null });
      } else if (parsed.kind === 'contextClear') {
        delete context.database;
        needsValidation = false;
        client.database = '';
        await saveContext(home, context);
        printResponse(streams.output, { status: 'ok' });
      } else if (parsed.kind === 'useDatabase') {
        const databases = await client.request({ action: 'listDatabases' });
        if (!Array.isArray(databases) || !databases.includes(parsed.name)) throw new Error('database_not_found');
        context.database = parsed.name;
        client.database = parsed.name;
        needsValidation = false;
        await saveContext(home, context);
        printResponse(streams.output, { status: 'ok', database: parsed.name });
      } else if (parsed.kind === 'createDatabase') {
        const response = await client.request({ action: 'createDatabase', dbName: parsed.name });
        if (response?.error) throw new Error(String(response.error));
        context.database = parsed.name;
        client.database = parsed.name;
        needsValidation = false;
        await saveContext(home, context);
        printResponse(streams.output, response, 'createDatabase');
      } else if (parsed.kind === 'showDatabase') {
        const response = await client.request({ action: 'listCollections' });
        printResponse(streams.output, { status: 'ok', database: client.database,
          collections: response.collections ?? response }, 'listCollections');
      } else if (parsed.kind === 'backupExport') {
        const filename = parsed.filename || `${parsed.id}.json`;
        const exported = await client.exportBackup(parsed.id, filename);
        printResponse(streams.output, { status: 'ok', backup_id: parsed.id,
          filename, files: exported.files, size_bytes: exported.sizeBytes });
      } else if (parsed.kind === 'mediaUpload') {
        try {
          const media = await client.uploadMediaFile(parsed.collection, parsed.filename,
            { resume: parsed.resume });
          printResponse(streams.output, media, 'community_media_finalize');
        } catch (error) {
          if (!(error instanceof MediaUploadError)) throw error;
          printResponse(streams.output, { status: 'resumable', error: error.code,
            upload_id: error.uploadId, next_chunk: error.nextChunk,
            received_chunks: error.receivedChunks, received_bytes: error.receivedBytes,
            resumable: error.resumable });
        }
      } else if (parsed.kind === 'mediaDownload') {
        printResponse(streams.output,
          await client.downloadMediaFile(parsed.id, parsed.filename));
      } else if (parsed.kind === 'mediaFind') {
        const needle = parsed.query.toLowerCase();
        const media = [];
        let offset = 0;
        do {
          const response = await client.request({ action: 'community_media_list',
            all: true, limit: 100, offset });
          media.push(...(response.media || []).filter((item) =>
            [item.id, item.filename, item.content_type, item.collection]
              .some((value) => String(value || '').toLowerCase().includes(needle))));
          if (!response.has_more) break;
          if (!Number.isSafeInteger(response.next_offset) ||
              response.next_offset <= offset) throw new Error('invalid media page');
          offset = response.next_offset;
        } while (true);
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
        if (response?.error) throw new Error(String(response.error));
        if (parsed.clearDatabase && context.database === parsed.clearDatabase) {
          delete context.database;
          client.database = '';
          await saveContext(home, context);
        }
        printResponse(streams.output, response, parsed.command.action);
      }
    } catch (error) {
      streams.output.write('error: ' + error.message.replace(/pacificdbs?:\/\/[^\s/]*@/g, 'pacificdb://[redacted]@') + '\n');
    }
  }
  prompt.close();
}
