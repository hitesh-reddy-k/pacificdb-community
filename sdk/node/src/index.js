import net from 'node:net';
import tls from 'node:tls';

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
          if (value?.error) reject(Object.assign(new Error(String(value.error)), { response: value }));
          else resolve(value);
        } catch (error) { reject(error); }
      });
      socket.on('end', () => {
        if (!settled) fail(new Error('PacificDB closed before returning a JSON response'));
      });
    });
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
}
