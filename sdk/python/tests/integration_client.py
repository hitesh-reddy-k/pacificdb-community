"""Real-engine semantic qualification; also runnable against an installed wheel."""
import base64
import hashlib
import json
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from pacificdb import PacificDB, PacificDBError

root=Path(os.environ['PACIFICDB_SDK_TEST_ROOT'])
phase=os.environ['PACIFICDB_SDK_PHASE']
url=os.environ['PACIFICDB_URL']
options={'timeout':120.0}
if os.environ.get('PACIFICDB_SDK_CA'):options['ca_file']=os.environ['PACIFICDB_SDK_CA']
expected=[{'id':str(i),'value':i,'unicode':'నమస్తే','nested':{'enabled':True},'tags':['a','b'],'empty':None} for i in range(128)]
source=bytes(i%251 for i in range(700000))

def refused(operation):
    try:operation()
    except PacificDBError:return
    raise AssertionError('operation should have been refused')

with PacificDB.connect(url,**options) as db:
    if phase=='prepare':
        db.create_database();db.create_collection('docs');db.create_collection('assets');db.create_collection('vectors')
        assert db.database=='sdk_python'
        assert 'sdk_python' in db.list_databases()
        result=db.insert_many('docs',expected);assert result['failed']==0 and result['inserted']==128
        with ThreadPoolExecutor(max_workers=8) as workers:
            list(workers.map(lambda i:db.insert('docs',{'id':f'concurrent-{i}','value':i}),range(32)))
        assert db.count('docs')['count']==160
        assert len(db.find('docs',{},limit=5,offset=2)['data'])==5
        db.update_one('docs',{'id':'0'},{'$set':{'value':500}});expected[0]['value']=500
        db.update_many('docs',{'id':'1'},{'$set':{'value':501}});expected[1]['value']=501
        bulk=db.bulk_write('docs',[{'action':'insertOne','data':{'id':'temporary','value':0}},{'action':'updateOne','filter':{'id':'temporary'},'update':{'value':1}},{'action':'deleteOne','filter':{'id':'temporary'}}]);assert bulk['failed']==0
        assert db.find_one('docs',{'id':'temporary'}) is None
        db.insert('docs',{'id':'delete-many'});db.delete_many('docs',{'id':'delete-many'})
        assert db.aggregate('docs',[{'$match':{'id':'0'}},{'$count':'total'}])['data']==[{'total':1}]
        assert db.explain('docs',{'id':'0'})['status']=='ok'
        db.indexes.create(collection='docs',name='by_value',fields={'value':1})
        assert db.indexes.list(collection='docs')['status']=='ok'
        assert db.indexes.validate(collection='docs')
        db.vectors.insert(collection='vectors',data={'id':'east','vector':[1,0]})
        assert db.vectors.query(collection='vectors',vector=[1,0],k=1)['data'][0]['id']=='east'
        db.media.put('assets','tiny',b'\x00\xff',{'contentType':'application/octet-stream'})
        assert db.media.get('assets','tiny')['data']==b'\x00\xff'
        path=root/'python-source.bin';path.write_bytes(source)
        media=db.media.upload_file('assets',path,chunk_bytes=65536)
        ready=db.media.upload_file('assets',path,chunk_bytes=65536,resume=media['id']);assert ready['id']==media['id']
        refused(lambda:db.use_database('missing_database'))
        refused(lambda:db.create_database('../unsafe'))
        assert db.database=='sdk_python'
        project=db.admin.community_project_create(name='python-legacy')['project']
        assert db.admin.community_project_get(id=project['id'])['project']['id']==project['id']
        db.admin.community_project_delete(id=project['id'])
        key=db.security.create_api_key(name='python-reader',role='read')
        from urllib.parse import urlsplit
        parsed=urlsplit(url)
        public=f'{parsed.scheme}://localhost:{parsed.port}/sdk_python'
        with PacificDB.from_url(public,**options) as reader:
            reader.security.use_token(key['key']);assert reader.find_one('docs',{'id':'0'})['value']==500
            refused(lambda:reader.insert('docs',{'id':'denied'}))
            db.security.revoke_api_key(id=key['id']);refused(lambda:reader.find('docs'))
        with PacificDB.from_url(public,**options) as anonymous:refused(lambda:anonymous.list_databases())
        assert db.security.whoami()['role']=='superadmin'
        assert db.admin.health_check();assert db.admin.raft_status();assert db.admin.op_status(opId=0)
        backup=db.backups.create(description='python-sdk-qualified')['backup_id']
        assert db.backups.verify(backup_id=backup)
        export=root/'python-backup.json';db.backups.export(backup,export,chunk_bytes=65536)
        document=json.loads(export.read_text())
        for file in document['files']:
            data=b''.join(base64.b64decode(chunk,validate=True) for chunk in file['chunks'])
            assert len(data)==file['size_bytes'] and hashlib.sha256(data).hexdigest()==file['sha256']
        restored=db.backups.restore(backup_id=backup,target_dir=str(root/'restore'/'python'))
        assert restored['success'] and (root/'restore'/'python').is_dir()
        (root/'python-state.json').write_text(json.dumps({'expected':expected,'media_id':media['id'],'backup_id':backup}))
    state=json.loads((root/'python-state.json').read_text())
    for document in state['expected']:
        actual=db.find_one('docs',{'id':document['id']});assert {key:actual[key] for key in document}==document
    for i in range(32):assert db.find_one('docs',{'id':f'concurrent-{i}'})['value']==i
    target=root/f'python-{phase}-download.bin';db.media.download_file(state['media_id'],target,collection='assets');assert target.read_bytes()==source
    assert db.media.get('assets','tiny')['data']==b'\x00\xff'
    assert db.backups.verify(backup_id=state['backup_id'])
print(json.dumps({'status':'PASS','language':'python','phase':phase,'exact_documents':160,'media_bytes':len(source)}))
