import copy
import importlib.util
import json
from pathlib import Path

import pytest
from pacificdb import PacificDB, PacificDBError
from test_transport import peer

ROOT = Path(__file__).resolve().parents[3]
MATRIX = json.loads((ROOT/'sdk/contracts/community-capabilities.json').read_text())
spec = importlib.util.spec_from_file_location('validator', ROOT/'scripts/test-sdk-capability-matrix.py')
validator = importlib.util.module_from_spec(spec); spec.loader.exec_module(validator)


def invoke(db, row):
    fields = row['sample']
    parts = row['python'].split('.')
    if len(parts) == 2: return getattr(getattr(db,parts[0]),parts[1])(**fields)
    name = parts[0]
    if name in ('capabilities','list_databases','list_collections'): return getattr(db,name)()
    if name in ('create_database','drop_database'): return getattr(db,name)(fields['dbName'])
    if name in ('create_collection','drop_collection'): return getattr(db,name)(fields['collection'])
    args = [fields['collection']]
    for key in {'insert':['data'],'insert_many':['data'],'find':[],'count':[],'explain':[],'aggregate':['pipeline'],'update_one':['filter','update'],'update_many':['filter','update'],'delete_one':['filter'],'delete_many':['filter'],'bulk_write':['ops']}[name]: args.append(fields[key])
    return getattr(db,name)(*args)


def test_entire_matrix_invokes_named_wire_methods():
    def reply(req):
        if req['action'] == 'listDatabases': return ['app']
        return {'status':'ok','token':'fixture-token','echo':req,'data':[{'id':'1'}]}
    with peer(reply) as server:
        for row in MATRIX['actions']:
            if row['classification'] == 'internal': continue
            with PacificDB(port=server.port,database='app') as db:
                db.token = 'fixture-token'
                before = len(server.frames)
                result = invoke(db,row)
                assert len(server.frames) == before+1
                sent = server.frames[-1]
                assert sent['action'] == row.get('alias_of',row['action'])
                assert sent['userId'] == 'system' and sent['dbName'] == 'app' and sent['token'] == 'fixture-token'
                for key,value in row['sample'].items(): assert sent[key] == value
                assert result == reply(sent)
    assert validator.validate(MATRIX) == 135


@pytest.mark.parametrize('key,value', [('action','dropDatabase'),('userId','other'),('dbName','other'),('db','other'),('token','private')])
def test_family_options_cannot_replace_scope_or_action(key,value):
    with peer(lambda req: {'status':'ok'}) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(PacificDBError) as got: db.indexes.create(collection='items',fields={'name':1},**{key:value})
        assert got.value.code == 'invalid_request' and server.frames == []


def test_security_token_lifecycle_and_extended_password_redaction():
    def reply(req):
        if req['action'] == 'security_authenticate': return {'token':'first-token'}
        if req['action'] == 'security_refresh_token': return {'token':'refreshed-token'}
        if req['action'] == 'security_whoami': return {'seen':req['token']}
        return {'error':req['newPassword'], 'message':req['newPassword'], req['newPassword']:True}
    with peer(reply) as server, PacificDB(port=server.port,database='app') as db:
        db.security.authenticate(username='demo',password='private-password')
        assert db.security.whoami() == {'seen':'first-token'}
        db.security.refresh_token()
        assert db.security.whoami() == {'seen':'refreshed-token'}
        db.security.use_token('api-key-token')
        assert db.security.whoami() == {'seen':'api-key-token'}
        with pytest.raises(PacificDBError) as got: db.admin.update_tenant_user_password(tenantId='tenant',username='demo',newPassword='private-new-password',updatedBy='admin')
        assert 'private-new-password' not in str(got.value) + got.value.code + str(got.value.response)


def test_validator_rejects_omissions_duplicates_and_wrong_aliases():
    missing=copy.deepcopy(MATRIX); missing['actions'].pop()
    with pytest.raises(ValueError,match='Dispatch mismatch'): validator.validate(missing,False)
    duplicate=copy.deepcopy(MATRIX); duplicate['actions'].append(duplicate['actions'][0])
    with pytest.raises(ValueError,match='Duplicate'): validator.validate(duplicate,False)
    wrong=copy.deepcopy(MATRIX)
    next(row for row in wrong['actions'] if row['classification']=='alias')['alias_of']='not_a_dispatch'
    with pytest.raises(ValueError,match='Invalid wire alias'): validator.validate(wrong,False)


def test_every_named_method_preserves_engine_error_and_context():
    failure = {'error':{'code':'fixture_denied','message':'denied'}, 'detail':17}
    with peer(lambda req: failure) as server:
        for row in MATRIX['actions']:
            if row['classification']=='internal': continue
            with PacificDB(port=server.port,database='app') as db:
                db.token='fixture-token'; before=len(server.frames)
                with pytest.raises(PacificDBError) as got: invoke(db,row)
                assert got.value.code=='fixture_denied' and got.value.response==failure
                assert got.value.__cause__ is None and got.value.__context__ is None
                assert db.database=='app' and db.token=='fixture-token' and len(server.frames)==before+1
