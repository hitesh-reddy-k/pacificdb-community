import copy
import threading
from concurrent.futures import ThreadPoolExecutor

import pytest
from pacificdb import PacificDB, PacificDBError
from test_transport import peer


def test_direct_lifecycle_selection_and_failure_preservation():
    def reply(req):
        if req['action'] == 'listDatabases': return ['app', 'shop']
        if req.get('dbName') == 'forbidden': return {'error': 'permission_denied', 'message': 'denied'}
        return {'status': 'ok'}
    with peer(reply) as server, PacificDB(port=server.port) as db:
        assert db.database == ''
        with pytest.raises(PacificDBError, match='database_required'): db.create_collection('users')
        assert server.frames == []
        assert db.capabilities() == {'status': 'ok'}
        assert db.create_database('app') == {'status': 'ok'}
        assert db.database == 'app'
        assert db.create_collection('users') == {'status': 'ok'}
        with pytest.raises(PacificDBError) as got: db.create_database('forbidden')
        assert got.value.code == 'permission_denied' and db.database == 'app'
        assert db.list_databases() == ['app', 'shop']
        with pytest.raises(PacificDBError, match='database_not_found'): db.use_database('missing')
        assert db.database == 'app'
        assert db.use_database('shop') == ['app', 'shop']
        assert db.database == 'shop'
        with pytest.raises(PacificDBError): db.drop_database('forbidden')
        assert db.database == 'shop'
        db.drop_database('app'); assert db.database == 'shop'
        db.drop_database('shop'); assert db.database == ''
        assert not any('project' in frame['action'].lower() or 'catalog' in frame['action'].lower() for frame in server.frames)


@pytest.mark.parametrize('method,args,wire', [
    ('create_collection', ['items'], {'action':'createCollection','collection':'items'}),
    ('list_collections', [], {'action':'listCollections'}),
    ('drop_collection', ['items'], {'action':'dropCollection','collection':'items'}),
    ('insert', ['items', {'id':'1'}], {'action':'insert','collection':'items','data':{'id':'1'}}),
    ('insert_many', ['items', [{'id':'1'}]], {'action':'insertMany','collection':'items','data':[{'id':'1'}]}),
    ('find', ['items'], {'action':'find','collection':'items','filter':{},'limit':-1,'offset':0}),
    ('find', ['items', {'id':'1'}, 3, 2], {'action':'find','collection':'items','filter':{'id':'1'},'limit':3,'offset':2}),
    ('count', ['items'], {'action':'count','collection':'items','filter':{}}),
    ('aggregate', ['items',[{'$limit':1}]], {'action':'aggregate','collection':'items','pipeline':[{'$limit':1}]}),
    ('explain', ['items'], {'action':'explain','collection':'items','filter':{}}),
    ('update_one',['items',{'id':'1'},{'$set':{'n':2}}], {'action':'updateOne','collection':'items','filter':{'id':'1'},'update':{'$set':{'n':2}}}),
    ('update_many',['items',{}, {'$set':{'n':2}}], {'action':'updateMany','collection':'items','filter':{},'update':{'$set':{'n':2}}}),
    ('delete_one',['items',{'id':'1'}], {'action':'deleteOne','collection':'items','filter':{'id':'1'}}),
    ('delete_many',['items',{}], {'action':'deleteMany','collection':'items','filter':{}}),
    ('bulk_write',['items',[{'action':'insertOne','data':{'id':'1'}}]], {'action':'bulkWrite','collection':'items','ops':[{'action':'insertOne','data':{'id':'1'}}]}),
])
def test_named_operation_wire_and_original_response(method, args, wire):
    original = copy.deepcopy(args)
    with peer(lambda req: {'status':'ok', 'echo':req}) as server, PacificDB(port=server.port, database='app') as db:
        result = getattr(db, method)(*args)
        assert result == {'status':'ok', 'echo':{'userId':'system','dbName':'app',**wire}}
        assert args == original
        assert len(server.frames) == 1


@pytest.mark.parametrize('response,expected', [([{'id':'1'}], {'id':'1'}), ({'data':[{'id':'1'}]}, {'id':'1'}), ([],None)])
def test_find_one_rows_and_wire(response, expected):
    with peer(lambda req: response) as server, PacificDB(port=server.port, database='app') as db:
        assert db.find_one('items') == expected
        assert server.frames[0] == {'userId':'system','dbName':'app','action':'find','collection':'items','filter':{},'limit':1,'offset':0}


@pytest.mark.parametrize('method,args', [('find',['items',[],1,0]), ('find',['items',{},-2,0]), ('find',['items',{},1,-1]), ('find',['items',{},True,0]), ('insert',['items',[]]), ('insert_many',['items',{}]), ('aggregate',['items',{}]), ('bulk_write',['items',{}]), ('update_one',['items',{},[]]), ('create_collection',[''])])
def test_invalid_inputs_are_local(method,args):
    with peer(lambda req: {'status':'ok'}) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(PacificDBError): getattr(db,method)(*args)
        assert server.frames == []


def test_destructive_errors_keep_engine_response_and_drop_does_not_clear_new_selection():
    for method, args in [('update_many',['items',{},{}]), ('delete_many',['items',{}]), ('bulk_write',['items',[]])]:
        with peer(lambda req: {'error':{'code':'conflict','message':'retry explicitly'}, 'detail':17}) as server, PacificDB(port=server.port,database='app') as db:
            with pytest.raises(PacificDBError) as got: getattr(db,method)(*args)
            assert got.value.code == 'conflict' and got.value.response['detail'] == 17
            assert db.database == 'app' and len(server.frames) == 1
    entered, release = threading.Event(), threading.Event()
    def reply(req):
        entered.set(); assert release.wait(2); return {'status':'ok'}
    with peer(reply) as server, PacificDB(port=server.port,database='app') as db, ThreadPoolExecutor(1) as worker:
        future = worker.submit(db.drop_database)
        assert entered.wait(1); db.database = 'new'; release.set(); future.result(timeout=1)
        assert db.database == 'new' and server.frames[0]['dbName'] == 'app'


@pytest.mark.parametrize('response', [{'status':'ok'}, [17], {'data':[17]}])
def test_malformed_lists_and_rows_preserve_context(response):
    with peer(lambda req: response) as server, PacificDB(port=server.port, database='app') as db:
        with pytest.raises(PacificDBError) as got: db.use_database('new')
        assert got.value.code == 'unexpected_response_shape'
        with pytest.raises(PacificDBError) as got: db.find_one('items')
        assert got.value.code == 'unexpected_response_shape'
        assert db.database == 'app'
