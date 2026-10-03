import json
import math
import threading
import time
from typing import Any
from .connection import parse_connection_url
from .errors import PacificDBError, sanitize
from .transport import ConnectionPool


class PacificDBClient:
    __slots__ = ('_settings', '_database', '_token', '_credentials', '_secrets',
                 '_auth_state', '_lock', '_pool', '_closed')

    def __init__(self, host: str = "127.0.0.1", port: int = 9000,
                 user_id: str = "system", database: str = "",
                 use_tls: bool = False, ca_file: str | None = None,
                 timeout: float = 30.0, *, pool_size: int = 16,
                 max_response_bytes: int = 64*1024*1024,
                 username: str | None = None, password: str | None = None):
        if not isinstance(host, str) or not host or any(c.isspace() for c in host) or '/' in host or '@' in host:
            raise PacificDBError('invalid_connection_options')
        if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
            raise PacificDBError('invalid_connection_options')
        if isinstance(pool_size, bool) or not isinstance(pool_size, int) or not 1 <= pool_size <= 32:
            raise PacificDBError('invalid_connection_options')
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 < timeout <= 1800:
            raise PacificDBError('invalid_connection_options')
        if isinstance(max_response_bytes, bool) or not isinstance(max_response_bytes, int) or not 1 <= max_response_bytes <= 64*1024*1024:
            raise PacificDBError('invalid_connection_options')
        if not isinstance(use_tls, bool) or ca_file and not use_tls:
            raise PacificDBError('invalid_connection_options')
        if not isinstance(user_id, str) or not user_id or not isinstance(database, str):
            raise PacificDBError('invalid_connection_options')
        if (username is not None or password is not None) and (not isinstance(username, str) or not username or not isinstance(password, str) or not password):
            raise PacificDBError('invalid_connection_options')
        self._settings = (host, port, user_id, use_tls, ca_file, timeout, pool_size)
        self._lock = threading.Condition(threading.RLock())
        self._database, self._token, self._closed = database, '', False
        self._credentials = (username, password) if username is not None else None
        self._secrets = {password} if password else set()
        self._auth_state = 'new'
        self._pool = ConnectionPool(host, port, timeout=timeout, pool_size=pool_size,
                                    use_tls=use_tls, ca_file=ca_file, max_response_bytes=max_response_bytes)

    host = property(lambda self: self._settings[0])
    port = property(lambda self: self._settings[1])
    user_id = property(lambda self: self._settings[2])
    use_tls = property(lambda self: self._settings[3])
    ca_file = property(lambda self: self._settings[4])
    timeout = property(lambda self: self._settings[5])
    pool_size = property(lambda self: self._settings[6])

    @property
    def database(self):
        with self._lock: return self._database

    @database.setter
    def database(self, value):
        if not isinstance(value, str): raise PacificDBError('invalid_database')
        with self._lock: self._database = value

    @property
    def token(self):
        with self._lock: return self._token

    @token.setter
    def token(self, value):
        if not isinstance(value, str): raise PacificDBError('invalid_token')
        with self._lock:
            self._token = value
            if value: self._secrets.add(value)

    @classmethod
    def from_url(cls, url, **options):
        return cls(**parse_connection_url(url, options))

    @classmethod
    def connect(cls, url, **options):
        client = cls.from_url(url, **options)
        try:
            client.request({'action': 'ping'})
            return client
        except BaseException:
            client.close()
            raise

    def __repr__(self):
        return f'PacificDBClient(host={self.host!r}, port={self.port}, database={self.database!r}, tls={self.use_tls})'

    def __enter__(self):
        with self._lock:
            if self._closed: raise PacificDBError('client_closed')
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        with self._lock:
            self._closed = True
            self._lock.notify_all()
        self._pool.close()

    def _safe_error(self, error):
        with self._lock: secrets = tuple(self._secrets)
        if not isinstance(error, PacificDBError):
            return PacificDBError('invalid_request', 'Invalid PacificDB request')
        return PacificDBError(sanitize(error.code, secrets), sanitize(str(error), secrets), sanitize(error.response, secrets))

    def _send(self, payload, deadline):
        def learn(value):
            if isinstance(value, dict):
                for field, item in value.items():
                    if any(part in str(field).lower() for part in ('password', 'token', 'authorization')) and isinstance(item, str) and item:
                        self._secrets.add(item)
                    learn(item)
            elif isinstance(value, list):
                for item in value: learn(item)
        with self._lock: learn(payload)
        try: wire = json.dumps(payload, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode('utf-8')+b'\n'
        except (TypeError, ValueError, UnicodeError): raise PacificDBError('invalid_request', 'Command must contain valid JSON values') from None
        value = self._pool.request(wire, deadline=deadline)
        if isinstance(value, dict) and value.get('error'):
            error = value['error']
            code = error.get('code', 'engine_error') if isinstance(error, dict) else str(error)
            message = error.get('message', code) if isinstance(error, dict) else value.get('message', code)
            raise PacificDBError(code, str(message), value)
        return value

    def _ensure_authenticated(self, deadline):
        with self._lock:
            if self._closed: raise PacificDBError('client_closed')
            if not self._credentials: return self._token
            while self._auth_state == 'pending' and not self._closed:
                remaining = deadline-time.monotonic()
                if remaining <= 0: raise PacificDBError('request_timeout')
                self._lock.wait(remaining)
            if self._closed or self._auth_state == 'failed': raise PacificDBError('client_closed')
            if self._auth_state == 'ready': return self._token
            self._auth_state = 'pending'
            username, password = self._credentials
            scope = {'userId': self.user_id, 'dbName': self._database}
        try:
            result = self._send({**scope, 'action': 'security_authenticate', 'username': username, 'password': password}, deadline)
            if not isinstance(result, dict) or not isinstance(result.get('token'), str) or not result['token']:
                raise PacificDBError('invalid_response', 'Invalid authentication response')
            with self._lock:
                if self._closed: raise PacificDBError('client_closed')
                self.token = result['token']; self._auth_state = 'ready'; self._lock.notify_all()
                return self._token
        except BaseException:
            with self._lock: self._auth_state = 'failed'
            self.close()
            raise

    def _scoped_request(self):
        with self._lock:
            scope = {'userId': self.user_id, 'dbName': self._database}
            if self._token: scope['token'] = self._token
        def request(command):
            deadline = time.monotonic()+self.timeout
            try:
                if not isinstance(command, dict): raise PacificDBError('invalid_request')
                payload = json.loads(json.dumps({**scope, **command}, allow_nan=False))
                if command.get('action') != 'security_authenticate':
                    token = self._ensure_authenticated(deadline)
                    if token:
                        scope.setdefault('token', token); payload.setdefault('token', scope['token'])
                return self._send(payload, deadline)
            except (PacificDBError, ValueError, TypeError, UnicodeError) as error:
                failure = self._safe_error(error)
            # Raise outside the handler so __context__ cannot retain a private engine error.
            raise failure from None
        request.wire_bytes = lambda command: len(json.dumps({**scope, **command}, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode('utf-8')) + 1
        return request

    def request(self, command: dict[str, Any]) -> Any:
        return self._scoped_request()(command)

    def authenticate(self, username: str, password: str) -> dict[str, Any]:
        if not isinstance(username, str) or not username or not isinstance(password, str) or not password:
            raise PacificDBError('invalid_credentials')
        with self._lock: self._secrets.add(password)
        result = self.request({'action': 'security_authenticate', 'username': username, 'password': password})
        if not isinstance(result, dict) or not isinstance(result.get('token'), str) or not result['token']:
            raise PacificDBError('invalid_response', 'Invalid authentication response')
        self.token = result['token']
        return result

    @staticmethod
    def _name(value, kind):
        if not isinstance(value, str) or not value:
            raise PacificDBError(f'invalid_{kind}', f'A non-empty {kind} name is required')
        return value

    @staticmethod
    def _object(value):
        if not isinstance(value, dict): raise PacificDBError('invalid_request', 'Expected a JSON object')
        return value

    @staticmethod
    def _array(value):
        if not isinstance(value, list): raise PacificDBError('invalid_request', 'Expected a JSON array')
        return value

    def _database_request(self, command):
        with self._lock:
            if not self._database: raise PacificDBError('database_required', 'database_required: select or create a database first')
            request = self._scoped_request()
        return request(command)

    def _collection_request(self, collection, action, **fields):
        self._name(collection, 'collection')
        return self._database_request({'action': action, 'collection': collection, **fields})

    @property
    def indexes(self):
        from .operations import Indexes
        return Indexes(self)

    @property
    def vectors(self):
        from .operations import Vectors
        return Vectors(self)

    @property
    def media(self):
        from .operations import Media
        return Media(self)

    @property
    def backups(self):
        from .operations import Backups
        return Backups(self)

    @property
    def security(self):
        from .operations import Security
        return Security(self)

    @property
    def admin(self):
        from .operations import Admin
        return Admin(self)

    def capabilities(self):
        return self.request({'action': 'community_capabilities'})

    def create_database(self, name: str | None = None, db_type: str = "binary") -> Any:
        name = self._name(self.database if name is None else name, 'database')
        self._name(db_type, 'database_type')
        response = self.request({'action': 'createDatabase', 'dbName': name, 'dbType': db_type})
        self.database = name
        return response

    def list_databases(self) -> list[str]:
        response = self.request({'action': 'listDatabases'})
        if not isinstance(response, list) or not all(isinstance(name, str) for name in response):
            raise PacificDBError('unexpected_response_shape', 'Expected an array of database names')
        return response

    def use_database(self, name: str):
        self._name(name, 'database')
        names = self.list_databases()
        if name not in names: raise PacificDBError('database_not_found')
        self.database = name
        return names

    def drop_database(self, name: str | None = None):
        with self._lock:
            name = self._name(self._database if name is None else name, 'database')
            request = self._scoped_request()
        response = request({'action': 'dropDatabase', 'dbName': name})
        with self._lock:
            if self._database == name: self._database = ''
        return response

    def create_collection(self, name: str) -> Any:
        return self._collection_request(name, 'createCollection')

    def list_collections(self):
        return self._database_request({'action': 'listCollections'})

    def drop_collection(self, name: str):
        return self._collection_request(name, 'dropCollection')

    def insert(self, collection: str, document: dict[str, Any]) -> Any:
        return self._collection_request(collection, 'insert', data=self._object(document))

    def insert_many(self, collection: str, documents: list[dict]):
        self._array(documents)
        for document in documents: self._object(document)
        return self._collection_request(collection, 'insertMany', data=documents)

    def find(self, collection: str, filter: dict[str, Any] | None = None,
             limit: int = -1, offset: int = 0) -> Any:
        if isinstance(limit, bool) or not isinstance(limit, int) or limit < -1 or isinstance(offset, bool) or not isinstance(offset, int) or offset < 0:
            raise PacificDBError('invalid_pagination', 'limit must be -1 or nonnegative; offset must be nonnegative')
        return self._collection_request(collection, 'find', filter=self._object({} if filter is None else filter), limit=limit, offset=offset)

    def find_one(self, collection: str, filter: dict | None = None):
        response = self.find(collection, filter, limit=1)
        rows = response.get('data') if isinstance(response, dict) else response
        if not isinstance(rows, list) or rows and not isinstance(rows[0], dict):
            raise PacificDBError('unexpected_response_shape', 'Expected an array of documents')
        return rows[0] if rows else None

    def count(self, collection: str, filter: dict | None = None):
        return self._collection_request(collection, 'count', filter=self._object({} if filter is None else filter))

    def aggregate(self, collection: str, pipeline: list):
        return self._collection_request(collection, 'aggregate', pipeline=self._array(pipeline))

    def explain(self, collection: str, filter: dict | None = None):
        return self._collection_request(collection, 'explain', filter=self._object({} if filter is None else filter))

    def update_one(self, collection: str, filter: dict[str, Any], update: dict[str, Any]) -> Any:
        return self._collection_request(collection, 'updateOne', filter=self._object(filter), update=self._object(update))

    def update_many(self, collection: str, filter: dict, update: dict):
        return self._collection_request(collection, 'updateMany', filter=self._object(filter), update=self._object(update))

    def delete_one(self, collection: str, filter: dict[str, Any]) -> Any:
        return self._collection_request(collection, 'deleteOne', filter=self._object(filter))

    def delete_many(self, collection: str, filter: dict):
        return self._collection_request(collection, 'deleteMany', filter=self._object(filter))

    def bulk_write(self, collection: str, operations: list):
        self._array(operations)
        for operation in operations: self._object(operation)
        return self._collection_request(collection, 'bulkWrite', ops=operations)
