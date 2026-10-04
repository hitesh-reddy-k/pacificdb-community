import json
import socket
import socketserver
import ssl
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager

import pytest
from pacificdb import PacificDBClient, PacificDBError

@contextmanager
def peer(reply, tls_context=None):
    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True
        def get_request(self):
            connection, address = super().get_request()
            if tls_context:
                try: connection = tls_context.wrap_socket(connection, server_side=True)
                except ssl.SSLError: connection.close(); raise
            return connection, address
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            with self.server.lock:
                self.server.connections += 1
                self.server.active += 1
                self.server.maximum = max(self.server.maximum, self.server.active)
            try:
                while True:
                    line = self.rfile.readline()
                    if not line: return
                    request = json.loads(line)
                    self.server.frames.append(request)
                    result = reply(request)
                    if result is None: return
                    if isinstance(result, list) and result and all(isinstance(part, bytes) for part in result):
                        for part in result: self.wfile.write(part); self.wfile.flush(); time.sleep(.001)
                    else:
                        data = result if isinstance(result, bytes) else json.dumps(result, ensure_ascii=False).encode()+b'\n'
                        self.wfile.write(data); self.wfile.flush()
                    if not isinstance(result, dict) or result.get('_pacificdb_connection_keepalive') is not True: return
            except (BrokenPipeError, ConnectionResetError): pass
            finally:
                with self.server.lock: self.server.active -= 1
    with Server(('127.0.0.1', 0), Handler) as server:
        server.frames = []; server.connections = server.active = server.maximum = 0
        server.lock = threading.Lock(); server.port = server.server_address[1]
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try: yield server
        finally: server.shutdown(); thread.join()


def test_fragments_utf8_arrays_and_keepalive():
    with peer(lambda req: [b'{"word":"caf\xc3', b'\xa9"}\n']) as server:
        with PacificDBClient(port=server.port) as db: assert db.request({'action': 'find'}) == {'word': 'café'}
    with peer(lambda req: ['app', 'shop']) as server:
        with PacificDBClient(port=server.port) as db: assert db.request({'action': 'listDatabases'}) == ['app', 'shop']
    with peer(lambda req: {'value': 1, '_pacificdb_connection_keepalive': True}) as server:
        with PacificDBClient(port=server.port) as db:
            for _ in range(3): assert db.request({'action': 'ping'}) == {'value': 1}
        assert server.connections == 1
    with peer(lambda req: {'value': 1}) as server:
        with PacificDBClient(port=server.port) as db:
            for _ in range(3): db.request({'action': 'ping'})
        assert server.connections == 3

@pytest.mark.parametrize('bad,code', [(b'garbage\n','invalid_response'), (b'{"x":NaN}\n','invalid_response'), (b'"\xff"\n','invalid_response'), (b'"'+b'x'*200+b'"\n','response_too_large'), (None,'connection_lost')])
def test_failed_frames_never_replay_write(bad, code):
    with peer(lambda req: bad) as server:
        with PacificDBClient(port=server.port, max_response_bytes=128) as db:
            with pytest.raises(PacificDBError) as got: db.request({'action': 'insert'})
            assert got.value.code == code
            assert len(server.frames) == 1


def test_typed_nested_errors_redact_response_and_traceback():
    with peer(lambda req: {'error': {'code': 'denied', 'message': 'private-token'}, 'password': 'private-password'}) as server:
        with PacificDBClient(port=server.port) as db:
            db.token = 'private-token'
            with pytest.raises(PacificDBError) as got: db.request({'action': 'find'})
            assert got.value.code == 'denied'
            assert 'private-token' not in str(got.value)
            assert got.value.response['password'] == '[redacted]'


def test_pool_bound_capture_and_shutdown():
    entered = threading.Event(); release = threading.Event()
    def reply(request):
        if request['action'] == 'hold': entered.set(); release.wait(2)
        return {'scope': request, '_pacificdb_connection_keepalive': True}
    with peer(reply) as server, ThreadPoolExecutor(max_workers=4) as threads:
        db = PacificDBClient(port=server.port, database='app', pool_size=1, timeout=1)
        held = threads.submit(db.request, {'action': 'hold'}); assert entered.wait(1)
        command = {'action': 'insert', 'data': {'value': 'original'}}
        captured = threading.Event()
        original = db._pool.request
        def observe(wire, **options):
            if json.loads(wire)['action'] == 'insert': captured.set()
            return original(wire, **options)
        db._pool.request = observe
        queued = threads.submit(db.request, command)
        assert captured.wait(1)
        db.database = 'other'; command['data']['value'] = 'changed'
        release.set(); held.result(timeout=2)
        wire = queued.result(timeout=2)['scope']
        assert wire['dbName'] == 'app' and wire['data']['value'] == 'original'
        assert server.maximum == 1
        entered.clear(); release.clear()
        active = threads.submit(db.request, {'action': 'hold'}); assert entered.wait(1)
        waiting = threads.submit(db.request, {'action': 'insert'})
        db.close(); db.close(); release.set()
        for future in [active, waiting]:
            with pytest.raises(PacificDBError): future.result(timeout=2)


def test_complete_request_deadline():
    def reply(req): time.sleep(.2); return b'{"incomplete":'
    with peer(reply) as server:
        with PacificDBClient(port=server.port, timeout=.04) as db:
            started = time.monotonic()
            with pytest.raises(PacificDBError) as got: db.request({'action': 'insert'})
            assert got.value.code == 'request_timeout'
            assert time.monotonic()-started < .2


def test_idle_peer_fin_is_not_a_write_retry():
    with peer(lambda req: {'ok': True, '_pacificdb_connection_keepalive': True, '_pacificdb_connection_close': True}) as server:
        with PacificDBClient(port=server.port) as db:
            assert db.request({'action': 'find'}) == {'ok': True}
            assert db.request({'action': 'insert'}) == {'ok': True}
        assert server.connections == 2


def test_tls_trust_and_hostname_before_credentials(tmp_path):
    key = tmp_path/'key.pem'; cert = tmp_path/'cert.pem'
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); context.load_cert_chain(cert, key)
    with peer(lambda req: {'ok': True}, context) as server:
        with PacificDBClient(host='localhost', port=server.port, use_tls=True, ca_file=str(cert)) as db:
            assert db.request({'action': 'ping'}) == {'ok': True}
        before = len(server.frames)
        for host, ca in [('127.0.0.1',str(cert)),('localhost',None)]:
            with PacificDBClient(host=host, port=server.port, use_tls=True, ca_file=ca) as db:
                with pytest.raises(PacificDBError) as got: db.authenticate('demo', 'private-password')
                assert got.value.code == 'tls_error'
                assert 'private-password' not in str(got.value)
        assert len(server.frames) == before


def test_multiple_workers_obey_pool_limit_and_all_finish():
    entered = threading.Event(); release = threading.Event(); lock = threading.Lock()
    count = 0
    def reply(req):
        nonlocal count
        with lock:
            count += 1
            if count == 2: entered.set()
        release.wait(2)
        return {'ok': True, '_pacificdb_connection_keepalive': True}
    with peer(reply) as server, PacificDBClient(port=server.port, pool_size=2) as db, ThreadPoolExecutor(max_workers=8) as threads:
        futures = [threads.submit(db.request, {'action': 'insert'}) for _ in range(8)]
        assert entered.wait(1); assert server.connections == 2
        release.set()
        for future in futures: assert future.result(timeout=2) == {'ok': True}
        assert server.connections == 2 and server.maximum == 2


def test_pool_wait_is_part_of_request_deadline():
    entered = threading.Event(); release = threading.Event()
    def reply(req): entered.set(); release.wait(2); return {'ok': True}
    with peer(reply) as server, PacificDBClient(port=server.port, pool_size=1, timeout=.15) as db, ThreadPoolExecutor(max_workers=2) as threads:
        active = threads.submit(db._pool.request, b'{"action":"hold"}\n', deadline=time.monotonic()+1); assert entered.wait(1)
        started = time.monotonic()
        with pytest.raises(PacificDBError) as got: db.request({'action': 'insert'})
        assert got.value.code == 'request_timeout'
        assert time.monotonic()-started < .3
        release.set()
        assert active.result(timeout=1) == {'ok': True}
        assert [frame['action'] for frame in server.frames] == ['hold']


def test_raw_request_secrets_are_redacted_in_errors():
    with peer(lambda req: {'error': 'denied', 'message': req['token']+' '+req['password']}) as server:
        with PacificDBClient(port=server.port) as db:
            with pytest.raises(PacificDBError) as got:
                db.request({'action': 'private', 'token': 'raw-secret-token', 'password': 'raw-secret-password'})
            assert 'raw-secret-token' not in str(got.value)
            assert 'raw-secret-password' not in repr(got.value.response)


@pytest.mark.parametrize('wire', [b'null\n', b'true\n', b'false\n', b'42\n', b'3.14\n', b'"ok"\n'], ids=['null', 'true', 'false', 'integer', 'float', 'string'])
def test_invalid_response_cannot_select_unconfirmed_database(wire):
    with peer(lambda req: wire) as server, PacificDBClient(port=server.port, database='original') as db:
        with pytest.raises(PacificDBError) as got: db.create_database('unconfirmed')
        assert got.value.code == 'invalid_response'
        assert db.database == 'original'
        assert len(server.frames) == 1


def test_connect_addresses_share_one_deadline(monkeypatch):
    import pacificdb.transport as transport
    now = [0.0]; attempts = []; sockets = []
    monkeypatch.setattr(transport.time, 'monotonic', lambda: now[0])
    monkeypatch.setattr(socket, 'getaddrinfo', lambda *args: [
        (socket.AF_INET, socket.SOCK_STREAM, 6, '', ('192.0.2.'+str(i), 9000)) for i in range(1, 4)])
    class StalledSocket:
        def __init__(self, *args): self.timeout = None; self.closed = False; sockets.append(self)
        def settimeout(self, value): self.timeout = value
        def connect(self, address):
            attempts.append(address); now[0] += self.timeout; raise TimeoutError('connect stalled')
        def shutdown(self, how): pass
        def close(self): self.closed = True
    monkeypatch.setattr(socket, 'socket', StalledSocket)
    with PacificDBClient(host='example.invalid', timeout=.06) as db:
        with pytest.raises(PacificDBError) as got: db.request({'action': 'insert'})
        assert got.value.code == 'request_timeout'
    assert len(attempts) == 1, 'must not reset timeout for each resolved address'
    assert now[0] <= .06
    assert all(sock.closed for sock in sockets)



def test_close_interrupts_connecting_socket(monkeypatch):
    entered = threading.Event(); released = threading.Event(); closed = threading.Event()
    monkeypatch.setattr(socket, 'getaddrinfo', lambda *args: [(socket.AF_INET, socket.SOCK_STREAM, 6, '', ('192.0.2.1', 9000))])
    class PendingSocket:
        def __init__(self, *args): pass
        def settimeout(self, value): pass
        def connect(self, address): entered.set(); released.wait(1); raise OSError('closed')
        def shutdown(self, how): released.set()
        def close(self): closed.set(); released.set()
    monkeypatch.setattr(socket, 'socket', PendingSocket)
    with PacificDBClient(host='example.invalid') as db, ThreadPoolExecutor(max_workers=1) as worker:
        request = worker.submit(db.request, {'action': 'insert'})
        assert entered.wait(1)
        try:
            db.close()
            assert closed.wait(.1), 'connecting socket must be registered before connect blocks'
        finally: released.set()
        with pytest.raises(PacificDBError) as got: request.result(timeout=1)
        assert got.value.code == 'client_closed'
