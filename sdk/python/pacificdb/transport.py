"""Bounded synchronous JSON-line connections with no automatic request replay."""
from collections import deque
import json
import socket
import ssl
import threading
import time
from .errors import PacificDBError


def _remaining(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise PacificDBError('request_timeout', 'PacificDB request timed out; a sent write may have completed')
    return remaining


def _dispose(connection):
    try: connection.shutdown(socket.SHUT_RDWR)
    except OSError: pass
    connection.close()


class ConnectionPool:
    def __init__(self, host, port, *, timeout=30, pool_size=16, use_tls=False,
                 ca_file=None, max_response_bytes=64*1024*1024):
        self._host, self._port = host, port
        self._timeout, self._size, self._limit = timeout, pool_size, max_response_bytes
        self._condition = threading.Condition()
        self._closed = False
        self._leases = 0
        self._idle = deque()
        self._connections = set()
        try: self._tls = ssl.create_default_context(cafile=ca_file) if use_tls else None
        except (OSError, ssl.SSLError): raise PacificDBError('tls_error', 'Could not load TLS trust configuration') from None
        if self._tls:
            self._tls.minimum_version = ssl.TLSVersion.TLSv1_2

    def _connect(self, deadline):
        # DNS is an OS operation; every returned address shares the request budget.
        addresses = socket.getaddrinfo(self._host, self._port, 0, socket.SOCK_STREAM)
        last_error = OSError('No address available')
        for family, kind, protocol, _, address in addresses:
            remaining = _remaining(deadline)
            connection = socket.socket(family, kind, protocol)
            try:
                with self._condition:
                    if self._closed: raise PacificDBError('client_closed')
                    self._connections.add(connection)
                connection.settimeout(remaining)
                connection.connect(address)
                return connection
            except BaseException as error:
                with self._condition: self._connections.discard(connection)
                _dispose(connection)
                if not isinstance(error, OSError): raise
                with self._condition:
                    if self._closed: raise PacificDBError('client_closed') from None
                last_error = error
        raise last_error

    def _acquire(self, deadline):
        with self._condition:
            while self._leases >= self._size:
                if self._closed: raise PacificDBError('client_closed')
                self._condition.wait(_remaining(deadline))
            if self._closed: raise PacificDBError('client_closed')
            self._leases += 1
            if self._idle: return self._idle.popleft()
        connection = None
        try:
            connection = self._connect(deadline)
            # Track connecting sockets too so close() can interrupt a TLS handshake.
            with self._condition:
                if self._closed: raise PacificDBError('client_closed')
                self._connections.add(connection)
            if self._tls:
                raw = connection
                connection = self._tls.wrap_socket(raw, server_hostname=self._host, do_handshake_on_connect=False)
                with self._condition:
                    self._connections.discard(raw)
                    self._connections.add(connection)
                    if self._closed: raise PacificDBError('client_closed')
                connection.settimeout(_remaining(deadline))
                connection.do_handshake()
            return connection
        except BaseException:
            if connection is not None:
                with self._condition: self._connections.discard(connection)
                _dispose(connection)
            with self._condition:
                self._leases -= 1
                self._condition.notify_all()
            raise

    def request(self, wire: bytes, *, deadline=None):
        deadline = time.monotonic() + self._timeout if deadline is None else deadline
        connection = None
        reusable = False
        sent = False
        try:
            connection = self._acquire(deadline)
            connection.settimeout(_remaining(deadline))
            sent = True
            connection.sendall(wire)
            response = bytearray()
            while True:
                connection.settimeout(_remaining(deadline))
                chunk = connection.recv(min(65536, self._limit+1-len(response)))
                if not chunk:
                    raise PacificDBError('connection_lost', 'PacificDB connection closed; a sent write may have completed')
                response.extend(chunk)
                newline = response.find(b'\n')
                if newline >= 0:
                    if newline > self._limit:
                        raise PacificDBError('response_too_large')
                    if newline != len(response)-1:
                        raise PacificDBError('invalid_response', 'Unexpected bytes after the response')
                    break
                if len(response) > self._limit:
                    raise PacificDBError('response_too_large')
            try:
                def invalid_constant(_): raise ValueError('non-finite JSON number')
                value = json.loads(response[:newline].decode('utf-8', errors='strict'), parse_constant=invalid_constant)
            except (ValueError, UnicodeError, RecursionError):
                raise PacificDBError('invalid_response', 'PacificDB returned an invalid JSON response') from None
            if not isinstance(value, (dict, list)):
                raise PacificDBError('invalid_response', 'Expected an object or array response')
            if isinstance(value, dict):
                reusable = value.pop('_pacificdb_connection_keepalive', False) is True
                reusable = value.pop('_pacificdb_connection_close', False) is not True and reusable
            return value
        except PacificDBError as error:
            if sent and 'sent write' not in str(error):
                raise PacificDBError(error.code, f'{error}; a sent write may have completed') from None
            raise
        except (TimeoutError, socket.timeout):
            raise PacificDBError('request_timeout', 'PacificDB request timed out; a sent write may have completed') from None
        except ssl.SSLError:
            raise PacificDBError('tls_error', 'TLS certificate, hostname or handshake verification failed') from None
        except OSError:
            with self._condition: closed = self._closed
            raise PacificDBError('client_closed' if closed else 'connection_lost',
                                 'Client is closed' if closed else 'PacificDB connection failed; a sent write may have completed') from None
        finally:
            if connection is not None:
                with self._condition:
                    self._leases -= 1
                    if reusable and not self._closed: self._idle.append(connection)
                    else: self._connections.discard(connection); _dispose(connection)
                    self._condition.notify_all()

    def close(self):
        with self._condition:
            if self._closed: return
            self._closed = True
            for connection in self._connections: _dispose(connection)
            self._connections.clear(); self._idle.clear()
            self._condition.notify_all()
