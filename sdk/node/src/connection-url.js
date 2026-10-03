function invalid(field = 'syntax') {
  return Object.assign(new TypeError(`invalid_connection_url: ${field}`), { code: 'invalid_connection_url' });
}

function decoded(value, field) {
  try {
    const result = decodeURIComponent(value);
    if (!result || /[\x00-\x1f\x7f]/.test(result)) throw invalid(field);
    return result;
  } catch { throw invalid(field); }
}

function integer(value, max, field) {
  if (!/^[0-9]+$/.test(String(value))) throw invalid(field);
  const number = Number(value);
  if (!Number.isSafeInteger(number) || number < 1 || number > max) throw invalid(field);
  return number;
}

/** Parse without networking or echoing credential-bearing input in errors. */
export function parseConnectionUrl(input, options = {}) {
  if (typeof input !== 'string' || /[\s\x00-\x1f\x7f]/.test(input) || input.includes('#')) throw invalid();
  const parts = /^(pacificdbs?):\/\/([^/?#]+)(\/[^?#]*)(?:\?([^#]*))?$/.exec(input);
  if (!parts) throw invalid();
  const [, scheme, authority, pathname, query = ''] = parts;
  const database = decoded(pathname.slice(1), 'database');
  if (/[\/\\]/.test(database) || database === '.' || database === '..') throw invalid('database');
  if (authority.split('@').length > 2) throw invalid('credentials');
  const address = authority.slice(authority.lastIndexOf('@') + 1);
  if (address.endsWith(':') || address.includes('%')) throw invalid('host/port');
  let url;
  try { url = new URL(input); } catch { throw invalid('host/port'); }
  if (!url.hostname) throw invalid('host');
  // Retain the supplied IPv6 spelling across languages; native URL otherwise
  // compresses it differently from URI/urllib parsers.
  const host = address.startsWith('[') ? address.slice(1, address.indexOf(']')) : address.split(':')[0];
  if (!address.startsWith('[') && !/^[A-Za-z0-9._-]+$/.test(host)) throw invalid('host');
  const result = { host: host.toLowerCase(),
    port: url.port ? integer(url.port, 65535, 'port') : 9000, database,
    tls: scheme === 'pacificdbs', userId: 'system', timeoutMs: 30000, poolSize: 16 };
  const fixed = new Set(['host', 'port', 'database', 'tls']);
  if (authority.includes('@')) {
    const userinfo = authority.slice(0, authority.indexOf('@'));
    const colon = userinfo.indexOf(':');
    if (colon < 1) throw invalid('credentials');
    result.username = decoded(userinfo.slice(0, colon), 'username');
    result.password = decoded(userinfo.slice(colon + 1), 'password');
    fixed.add('username'); fixed.add('password');
  }
  const seen = new Set();
  for (const pair of query ? query.split('&') : []) {
    const equals = pair.indexOf('=');
    if (equals < 1) throw invalid('query');
    const key = decoded(pair.slice(0, equals).replace(/\+/g, '%20'), 'query');
    const value = decoded(pair.slice(equals + 1).replace(/\+/g, '%20'), 'query');
    if (!['userId', 'timeoutMs', 'poolSize', 'caFile'].includes(key) || seen.has(key)) throw invalid('query');
    seen.add(key); fixed.add(key);
    result[key] = key === 'timeoutMs' ? integer(value, 1800000, key) :
      key === 'poolSize' ? integer(value, 32, key) : value;
  }
  for (const [key, value] of Object.entries(options)) {
    if (value === undefined) continue;
    if (fixed.has(key) && result[key] !== value) throw invalid(`conflicting ${key}`);
    result[key] = value;
  }
  result.timeoutMs = integer(result.timeoutMs, 1800000, 'timeoutMs');
  result.poolSize = integer(result.poolSize, 32, 'poolSize');
  if (result.caFile && !result.tls) throw invalid('caFile requires TLS');
  return result;
}
