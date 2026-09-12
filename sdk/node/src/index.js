import net from 'node:net';
import tls from 'node:tls';
import { createHash } from 'node:crypto';
import { createReadStream, createWriteStream } from 'node:fs';
import { rename, stat, unlink } from 'node:fs/promises';
import { once } from 'node:events';
import path from 'node:path';

const MAX_SOURCE_CHUNK_BYTES = 4 * 1024 * 1024;
const REQUEST_RESERVE_BYTES = 64 * 1024;

function mediaType(filename) {
  const extension = path.extname(filename).toLowerCase();
  return ({ '.gif': 'image/gif', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
    '.png': 'image/png', '.webp': 'image/webp', '.mp4': 'video/mp4',
    '.mov': 'video/quicktime', '.webm': 'video/webm', '.mp3': 'audio/mpeg',
    '.wav': 'audio/wav' })[extension] || 'application/octet-stream';
}

export class PacificDBClient {
  constructor({ host = '127.0.0.1', port = 9000, userId = 'system',
                database = '', tls: useTls = false, ca, timeoutMs = 30000 } = {}) {
    this.host = host;
    this.port = port;
    this.userId = userId;
    this.database = database;
    this.useTls = useTls;
    this.ca = ca;
    this.timeoutMs = timeoutMs;
    this.token = '';
  }

  request(command) {
    const payload = { userId: this.userId, dbName: this.database,
      ...(this.token ? { token: this.token } : {}), ...command };
    return new Promise((resolve, reject) => {
      let response = '';
      let settled = false;
      const options = { host: this.host, port: this.port,
        ...(this.ca ? { ca: this.ca } : {}) };
      const socket = this.useTls ? tls.connect(options) : net.createConnection(options);
      const fail = (error) => {
        if (settled) return;
        settled = true;
        socket.destroy();
        reject(error);
      };
      socket.setTimeout(this.timeoutMs, () => fail(new Error('PacificDB request timed out')));
      socket.on('error', fail);
      socket.on('connect', () => socket.write(JSON.stringify(payload) + '\n'));
      socket.on('data', (chunk) => {
        response += chunk;
        const newline = response.indexOf('\n');
        if (newline < 0 || settled) return;
        settled = true;
        socket.end();
        try {
          const value = JSON.parse(response.slice(0, newline));
          if (value?.error) {
            const code = String(value.error);
            const detail = typeof value.message === 'string' && value.message !== code
              ? `${code}: ${value.message}` : code;
            reject(Object.assign(new Error(detail), { response: value }));
          }
          else resolve(value);
        } catch (error) {
          if (error instanceof SyntaxError) {
            reject(new Error(`PacificDB server at ${this.host}:${this.port} returned ` +
              'a non-JSON response; verify the host and port'));
          } else reject(error);
        }
      });
      socket.on('end', () => {
        if (!settled) fail(new Error('PacificDB closed before returning a JSON response'));
      });
    });
  }

  capabilities() {
    return this.request({ action: 'community_capabilities' });
  }

  _wireBytes(command) {
    const payload = { userId: this.userId, dbName: this.database,
      ...(this.token ? { token: this.token } : {}), ...command };
    return Buffer.byteLength(JSON.stringify(payload)) + 1;
  }

  async authenticate(username, password) {
    const result = await this.request({ action: 'security_authenticate', username, password });
    this.token = result.token;
    return result;
  }

  createDatabase(name = this.database, dbType = 'binary') {
    return this.request({ action: 'createDatabase', dbName: name, dbType });
  }
  createCollection(name) {
    return this.request({ action: 'createCollection', collection: name });
  }
  insert(collection, data) {
    return this.request({ action: 'insert', collection, data });
  }
  find(collection, filter = {}, { limit = -1, offset = 0 } = {}) {
    return this.request({ action: 'find', collection, filter, limit, offset });
  }
  updateOne(collection, filter, update) {
    return this.request({ action: 'updateOne', collection, filter, update });
  }
  deleteOne(collection, filter) {
    return this.request({ action: 'deleteOne', collection, filter });
  }

  putMedia(collection, id, data, metadata = {}) {
    if (typeof id !== 'string' || !id) throw new TypeError('media id is required');
    const bytes = Buffer.from(data);
    return this.insert(collection, { ...metadata, id, kind: 'media',
      dataBase64: bytes.toString('base64'), sizeBytes: bytes.length,
      encoding: 'base64' });
  }
  updateMedia(collection, id, data, metadata = {}) {
    return this.putMedia(collection, id, data, metadata);
  }
  async getMedia(collection, id) {
    const response = await this.find(collection, { id }, { limit: 1 });
    const document = Array.isArray(response) ? response[0] : response?.data?.[0];
    if (!document || document.kind !== 'media' || typeof document.dataBase64 !== 'string') {
      throw new Error(`media not found: ${id}`);
    }
    const { dataBase64, ...metadata } = document;
    return { data: Buffer.from(dataBase64, 'base64'), metadata };
  }
  async uploadMediaFile(collection, filename, {
    contentType = mediaType(filename), chunkBytes, resume
  } = {}) {
    if (!this.database) throw new Error('select a database before uploading media');
    if (typeof collection !== 'string' || !collection) {
      throw new TypeError('media collection is required');
    }
    const file = await stat(filename);
    if (!file.isFile() || file.size === 0) throw new Error('media file must be non-empty');
    const capabilities = await this.capabilities();
    const maxRequestBytes = capabilities.max_request_bytes;
    if (!Number.isSafeInteger(maxRequestBytes) || maxRequestBytes <= REQUEST_RESERVE_BYTES) {
      throw new Error('engine request limit is too small for media chunks');
    }
    const safeChunkBytes = Math.floor((maxRequestBytes - REQUEST_RESERVE_BYTES) * 3 / 4);
    const sourceChunkBytes = Math.min(MAX_SOURCE_CHUNK_BYTES,
      chunkBytes ?? safeChunkBytes, safeChunkBytes);
    if (!Number.isSafeInteger(sourceChunkBytes) || sourceChunkBytes < 64 * 1024) {
      throw new Error('media chunk size must be at least 64 KiB');
    }

    const wholeHash = createHash('sha256');
    for await (const chunk of createReadStream(filename,
      { highWaterMark: sourceChunkBytes })) wholeHash.update(chunk);
    const sha256 = wholeHash.digest('hex');
    const chunkCount = Math.ceil(file.size / sourceChunkBytes);
    const begun = await this.request({ action: 'community_media_begin', collection,
      filename: path.basename(filename), content_type: contentType,
      size_bytes: file.size, chunk_count: chunkCount, sha256,
      ...(resume ? { resume_id: resume } : {}) });
    const media = begun.media;
    if (!media?.id) throw new Error('engine did not return a media id');
    if (media.status === 'ready') return media;

    let index = 0;
    for await (const chunk of createReadStream(filename,
      { highWaterMark: sourceChunkBytes })) {
      const command = { action: 'community_media_put_chunk', media_id: media.id,
        index, data: chunk.toString('base64'), size_bytes: chunk.length,
        sha256: createHash('sha256').update(chunk).digest('hex') };
      if (this._wireBytes(command) > maxRequestBytes) {
        throw new Error('serialized media chunk exceeds engine request limit');
      }
      await this.request(command);
      index += 1;
    }
    const finalized = await this.request({ action: 'community_media_finalize',
      media_id: media.id });
    return finalized.media;
  }

  async downloadMediaFile(mediaId, destination) {
    const response = await this.request({ action: 'community_media_get',
      media_id: mediaId });
    const manifest = response.media;
    if (!manifest || manifest.status !== 'ready') {
      throw new Error(`ready media not found: ${mediaId}`);
    }
    const output = createWriteStream(destination, { mode: 0o600 });
    let outputError;
    output.on('error', (error) => { outputError = error; });
    const wholeHash = createHash('sha256');
    let sizeBytes = 0;
    try {
      for (let index = 0; index < manifest.chunk_count; index += 1) {
        const result = await this.request({ action: 'community_media_get_chunk',
          media_id: mediaId, index });
        const chunk = result.chunk;
        if (!chunk || typeof chunk.data !== 'string') {
          throw new Error(`media chunk missing: ${index}`);
        }
        const bytes = Buffer.from(chunk.data, 'base64');
        const checksum = createHash('sha256').update(bytes).digest('hex');
        if (checksum !== chunk.sha256) throw new Error(`media chunk checksum mismatch: ${index}`);
        wholeHash.update(bytes);
        sizeBytes += bytes.length;
        if (!output.write(bytes)) await once(output, 'drain');
        if (outputError) throw outputError;
      }
      await new Promise((resolve, reject) => {
        if (outputError) return reject(outputError);
        output.once('finish', resolve);
        output.once('error', reject);
        output.end();
      });
      const sha256 = wholeHash.digest('hex');
      if (sizeBytes !== manifest.size_bytes || sha256 !== manifest.sha256) {
        throw new Error('downloaded media does not match its manifest');
      }
      return { id: mediaId, destination, sizeBytes, sha256 };
    } catch (error) {
      output.destroy();
      await unlink(destination).catch(() => {});
      throw error;
    }
  }
  async exportBackup(backupId, destination, { chunkBytes = 1024 * 1024 } = {}) {
    if (typeof backupId !== 'string' || !backupId) throw new TypeError('backup id is required');
    if (!Number.isSafeInteger(chunkBytes) || chunkBytes < 1 || chunkBytes > 1024 * 1024) {
      throw new TypeError('backup chunk size must be between 1 byte and 1 MiB');
    }
    const manifest = await this.request({ action: 'export_backup_manifest',
      backup_id: backupId });
    if (!Array.isArray(manifest.files)) throw new Error('invalid backup export manifest');
    const partial = `${destination}.part`;
    const output = createWriteStream(partial, { mode: 0o600 });
    let outputError;
    output.on('error', (error) => { outputError = error; });
    const write = async (value) => {
      if (!output.write(value)) await once(output, 'drain');
      if (outputError) throw outputError;
    };
    let total = 0;
    try {
      await write(`{"format":${JSON.stringify(manifest.format)},"backup":${JSON.stringify(manifest.backup)},"files":[`);
      for (let fileIndex = 0; fileIndex < manifest.files.length; fileIndex += 1) {
        const file = manifest.files[fileIndex];
        if (typeof file.path !== 'string' || !Number.isSafeInteger(file.size_bytes) ||
            file.size_bytes < 0 || typeof file.sha256 !== 'string') {
          throw new Error('invalid backup file manifest');
        }
        await write(`${fileIndex ? ',' : ''}{"path":${JSON.stringify(file.path)},` +
          `"size_bytes":${file.size_bytes},"sha256":${JSON.stringify(file.sha256)},"chunks":[`);
        const hash = createHash('sha256');
        let offset = 0;
        let chunkIndex = 0;
        while (offset < file.size_bytes) {
          const response = await this.request({ action: 'export_backup_file_chunk',
            backup_id: backupId, path: file.path, offset,
            max_bytes: Math.min(chunkBytes, file.size_bytes - offset) });
          const bytes = Buffer.from(response.data || '', 'base64');
          const checksum = createHash('sha256').update(bytes).digest('hex');
          if (!bytes.length || response.offset !== offset || response.size_bytes !== bytes.length ||
              response.sha256 !== checksum) throw new Error(`invalid backup chunk: ${file.path}`);
          await write(`${chunkIndex ? ',' : ''}${JSON.stringify(response.data)}`);
          hash.update(bytes);
          offset += bytes.length;
          total += bytes.length;
          chunkIndex += 1;
        }
        if (hash.digest('hex') !== file.sha256)
          throw new Error(`backup file checksum mismatch: ${file.path}`);
        await write(']}');
      }
      await write(']}\n');
      await new Promise((resolve, reject) => {
        if (outputError) return reject(outputError);
        output.once('finish', resolve);
        output.once('error', reject);
        output.end();
      });
      await rename(partial, destination);
      return { backupId, destination, files: manifest.files.length, sizeBytes: total };
    } catch (error) {
      output.destroy();
      await unlink(partial).catch(() => {});
      throw error;
    }
  }
  putVector(collection, id, vector, metadata = {}) {
    if (typeof id !== 'string' || !id) throw new TypeError('vector id is required');
    if (!Array.isArray(vector) || !vector.length ||
        vector.some((value) => typeof value !== 'number' || !Number.isFinite(value))) {
      throw new TypeError('vector must be a non-empty array of finite numbers');
    }
    return this.request({ action: 'insertVector', collection,
      data: { ...metadata, id, kind: 'vector', vector } });
  }
  queryVector(collection, vector, { k = 10, metric = 'cosine',
                                    filter = {}, modality } = {}) {
    if (!Array.isArray(vector) || !vector.length ||
        vector.some((value) => typeof value !== 'number' || !Number.isFinite(value))) {
      throw new TypeError('vector must be a non-empty array of finite numbers');
    }
    return this.request({ action: 'queryVector', collection, vector, k, metric,
      filter, ...(modality ? { modality } : {}) });
  }
}
