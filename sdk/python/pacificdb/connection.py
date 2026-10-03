"""Parse the shared connection URL grammar without performing I/O."""
import ipaddress
import re
from urllib.parse import unquote_to_bytes
from .errors import PacificDBError


def _invalid():
    return PacificDBError('invalid_connection_url', 'Invalid PacificDB connection URL or conflicting options')


def _decode(value):
    if re.search(r'%(?![0-9a-fA-F]{2})', value):
        raise _invalid()
    try:
        decoded = unquote_to_bytes(value).decode('utf-8', errors='strict')
        if not decoded or any(ord(char) < 32 or ord(char) == 127 for char in decoded):
            raise _invalid()
        return decoded
    except (UnicodeError, ValueError):
        pass
    raise _invalid() from None


def _integer(value, maximum):
    if isinstance(value, bool) or not re.fullmatch('[0-9]+', str(value)):
        raise _invalid()
    try: number = int(value)
    except ValueError: raise _invalid() from None
    if not 1 <= number <= maximum:
        raise _invalid()
    return number


def parse_connection_url(url: str, options: dict | None = None) -> dict:
    if not isinstance(url, str) or re.search(r'[\s\x00-\x1f\x7f#]', url):
        raise _invalid()
    match = re.fullmatch(r'(pacificdbs?)://([^/?#]+)(/[^?#]*)(?:\?([^#]*))?', url)
    if not match:
        raise _invalid()
    scheme, authority, path, query = match.groups()
    database = _decode(path[1:])
    if '/' in database or '\\' in database or database in ('.', '..') or authority.count('@') > 1:
        raise _invalid()
    credentials = {}
    address = authority
    if '@' in authority:
        userinfo, address = authority.split('@')
        if ':' not in userinfo:
            raise _invalid()
        username, password = userinfo.split(':', 1)
        credentials = {'username': _decode(username), 'password': _decode(password)}
    if address.startswith('['):
        host_match = re.fullmatch(r'\[([^\]]+)\](?::([0-9]+))?', address)
        if not host_match: raise _invalid()
        host, port = host_match.groups()
        try: ipaddress.IPv6Address(host)
        except ValueError: raise _invalid() from None
        if '%' in host: raise _invalid()
    else:
        host_match = re.fullmatch(r'([A-Za-z0-9._-]+)(?::([0-9]+))?', address)
        if not host_match: raise _invalid()
        host, port = host_match.groups()
    result = {'host': host.lower(), 'port': _integer(port, 65535) if port else 9000,
              'database': database, 'use_tls': scheme == 'pacificdbs',
              'user_id': 'system', 'timeout': 30.0, 'pool_size': 16, **credentials}
    fixed = {'host', 'port', 'database', 'use_tls', *credentials}
    names = {'userId': 'user_id', 'timeoutMs': 'timeout', 'poolSize': 'pool_size', 'caFile': 'ca_file'}
    seen = set()
    for pair in query.split('&') if query else []:
        if '=' not in pair: raise _invalid()
        key, value = [_decode(piece.replace('+', '%20')) for piece in pair.split('=', 1)]
        if key not in names or key in seen: raise _invalid()
        seen.add(key); fixed.add(names[key])
        result[names[key]] = (_integer(value, 1800000) / 1000 if key == 'timeoutMs' else
                              _integer(value, 32) if key == 'poolSize' else value)
    for key, value in (options or {}).items():
        if key not in {*result, 'ca_file', 'max_response_bytes', 'username', 'password'}:
            raise _invalid()
        if key in fixed and result.get(key) != value:
            raise _invalid()
        result[key] = value
    result['pool_size'] = _integer(result['pool_size'], 32)
    timeout = result['timeout']
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 0.001 <= timeout <= 1800:
        raise _invalid()
    if result.get('ca_file') and not result['use_tls']: raise _invalid()
    return result
