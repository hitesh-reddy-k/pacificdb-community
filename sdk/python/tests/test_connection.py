import json
from pathlib import Path
import traceback
from concurrent.futures import ThreadPoolExecutor

import pytest
from pacificdb import PacificDB, PacificDBClient, PacificDBError
from pacificdb.connection import parse_connection_url
from test_transport import peer

FIXTURES = json.loads((Path(__file__).parents[2] / 'contracts/connection-urls.json').read_text())

def python_options(values):
    names = {'tls': 'use_tls', 'userId': 'user_id', 'poolSize': 'pool_size', 'caFile': 'ca_file', 'timeoutMs': 'timeout'}
    return {names.get(key, key): value / 1000 if key == 'timeoutMs' else value for key, value in values.items()}

@pytest.mark.parametrize('case', FIXTURES['valid'])
def test_shared_valid_urls(case):
    assert parse_connection_url(case['url'], python_options(case.get('options', {}))) == python_options(case['expected'])

@pytest.mark.parametrize('case', FIXTURES['invalid'])
def test_invalid_url_opens_no_socket(case, monkeypatch):
    monkeypatch.setattr('socket.create_connection', lambda *a, **k: pytest.fail('invalid URL must not connect'))
    with pytest.raises(PacificDBError) as got:
        PacificDB.from_url(case['url'], **python_options(case.get('options', {})))
    assert got.value.code == case['code']
    assert case['url'] not in str(got.value)


def test_lazy_auth_once_and_secrets_not_in_diagnostics():
    def reply(request):
        if request['action'] == 'security_authenticate':
            assert request['password'] == 'private-password'
            return {'token': 'private-token', '_pacificdb_connection_keepalive': True}
        assert request['token'] == 'private-token'
        return {'data': request['dbName'], '_pacificdb_connection_keepalive': True}
    with peer(reply) as server:
        db = PacificDB.from_url(f'pacificdb://demo:private-password@127.0.0.1:{server.port}/app?poolSize=2')
        assert not server.frames
        with db, ThreadPoolExecutor(max_workers=8) as threads:
            assert list(threads.map(lambda _: db.request({'action': 'find'}), range(8))) == [{'data': 'app'}]*8
            assert 'private-password' not in repr(db)
            assert 'private-token' not in repr(db)
        assert sum(frame['action'] == 'security_authenticate' for frame in server.frames) == 1
        with pytest.raises(PacificDBError): db.request({'action': 'insert'})

@pytest.mark.parametrize('reply', [lambda req: {'error': 'denied', 'message': req['password'], 'token': 'private-token'}, lambda req: {'ok': True}])
def test_eager_auth_failure_closes_and_never_writes(reply):
    with peer(reply) as server:
        url = f'pacificdb://demo:private-password@127.0.0.1:{server.port}/app'
        with pytest.raises(PacificDBError) as got:
            PacificDB.connect(url)
        diagnostic = ''.join(traceback.format_exception(got.value)) + repr(got.value.response)
        assert 'private-password' not in diagnostic
        assert 'private-token' not in diagnostic
        assert got.value.__cause__ is None
        assert got.value.__context__ is None
        assert [frame['action'] for frame in server.frames] == ['security_authenticate']


def test_lazy_auth_failure_wakes_every_caller():
    with peer(lambda req: {'error': 'denied'}) as server:
        db = PacificDB.from_url(f'pacificdb://demo:private-password@127.0.0.1:{server.port}/app')
        with ThreadPoolExecutor(max_workers=5) as threads:
            futures = [threads.submit(db.request, {'action': 'insert'}) for _ in range(5)]
            for future in futures:
                with pytest.raises(PacificDBError): future.result(timeout=2)
        assert [frame['action'] for frame in server.frames] == ['security_authenticate']
