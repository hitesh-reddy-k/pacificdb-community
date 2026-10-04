import base64
import hashlib
import json
from pathlib import Path

import pytest
from pacificdb import PacificDB, PacificDBError, MediaUploadError
from test_transport import peer


def digest(data): return hashlib.sha256(data).hexdigest()


class MediaPeer:
    def __init__(self, source):
        self.source=source; self.chunks={}; self.manifest=None; self.interrupt=False
        self.scope_bad=False; self.corrupt=None; self.after_begin=None
    def __call__(self, req):
        action=req['action']
        if action=='community_capabilities': return {'max_request_bytes':262144,'media_chunk_source_max_bytes':65536}
        if action=='community_media_begin':
            if not self.manifest:
                self.manifest={'id':'media-1','database':req['dbName'],'collection':req['collection'], 'filename':req['filename'],'content_type':req['content_type'],'size_bytes':req['size_bytes'],'chunk_count':req['chunk_count'],'sha256':req['sha256'],'status':'uploading'}
            if self.after_begin: self.after_begin()
            m={**self.manifest,'received_indices':sorted(self.chunks),'received_chunks':len(self.chunks),'received_bytes':sum(len(c['data']) for c in self.chunks.values())}
            if self.scope_bad: m['collection']='other'
            return {'media':m}
        if action=='community_media_put_chunk':
            data=base64.b64decode(req['data'],validate=True)
            assert len(data)<=65536 and digest(data)==req['sha256'] and len(data)==req['size_bytes']
            self.chunks[req['index']]={'data':data,'sha256':req['sha256']}
            if req['index']==1 and self.interrupt: self.interrupt=False; return None
            return {'received_indices':sorted(self.chunks),'received_chunks':len(self.chunks),'received_bytes':sum(len(c['data']) for c in self.chunks.values())}
        if action=='community_media_finalize':
            assert b''.join(self.chunks[i]['data'] for i in range(self.manifest['chunk_count']))==self.source
            self.manifest['status']='ready';return {'media':self.manifest}
        if action=='community_media_get': return {'media':self.manifest}
        if action=='community_media_get_chunk':
            data=self.chunks[req['index']]['data']
            result={'media_id':'media-1','index':req['index'],'size_bytes':len(data),'sha256':digest(data),'data':base64.b64encode(data).decode()}
            if self.corrupt=='base64': result['data']='%%%'
            if self.corrupt=='hash': result['sha256']='0'*64
            if self.corrupt=='index': result['index']+=1
            return {'chunk':result}
        raise AssertionError(action)


def test_upload_resume_download_and_captured_scope(tmp_path):
    source=b'\x00\xff'+b'x'*150000; path=tmp_path/'sample.bin';path.write_bytes(source)
    state=MediaPeer(source); state.interrupt=True
    with peer(state) as server, PacificDB(port=server.port,database='app') as db:
        state.after_begin=lambda: setattr(db,'database','other')
        with pytest.raises(MediaUploadError) as got: db.media.upload_file('assets',path,chunk_bytes=65536)
        assert got.value.upload_id=='media-1' and got.value.next_chunk==1 and got.value.received_chunks==1 and got.value.resumable
        assert all(req['dbName']=='app' for req in server.frames)
        db.database='app';state.after_begin=None
        ready=db.media.upload_file('assets',path,chunk_bytes=65536,resume=got.value.upload_id)
        assert ready['status']=='ready'
        assert [f['index'] for f in server.frames if f['action']=='community_media_put_chunk']==[0,1,2]
        dest=tmp_path/'out.bin';dest.write_bytes(b'old');(tmp_path/'out.bin.part').write_bytes(b'owned by another transfer')
        for corrupt in ('base64','hash','index'):
            state.corrupt=corrupt
            with pytest.raises(PacificDBError): db.media.download_file('media-1',dest,collection='assets')
            assert dest.read_bytes()==b'old'
            assert (tmp_path/'out.bin.part').read_bytes()==b'owned by another transfer'
        state.corrupt=None
        result=db.media.download_file('media-1',dest,collection='assets')
        assert dest.read_bytes()==source and result['sha256']==digest(source)
        assert not any(name.name.startswith('.pacificdb-') for name in tmp_path.iterdir())
        db.database='other'
        with pytest.raises(PacificDBError): db.media.download_file('media-1',dest)
        assert dest.read_bytes()==source


def test_wrong_resume_scope_and_changed_file_fail_before_finalization(tmp_path):
    source=b'x'*150000;path=tmp_path/'input.bin';path.write_bytes(source)
    for mode in ('scope','changed'):
        state=MediaPeer(source)
        if mode=='scope':state.scope_bad=True
        else:state.after_begin=lambda: path.write_bytes(b'y'*len(source))
        with peer(state) as server, PacificDB(port=server.port,database='app') as db:
            with pytest.raises(PacificDBError):db.media.upload_file('assets',path,chunk_bytes=65536,resume='media-1' if mode=='scope' else None)
            assert not any(f['action']=='community_media_finalize' for f in server.frames)
        path.write_bytes(source)


def test_memory_media_round_trip_preserves_original_metadata():
    rows={}
    def reply(req):
        if req['action']=='insert': rows[req['data']['id']]=req['data'];return {'status':'ok'}
        return {'data':[rows[req['filter']['id']]]}
    with peer(reply) as server, PacificDB(port=server.port,database='app') as db:
        metadata={'contentType':'image/png'}
        db.media.put('assets','logo',b'\x00\xff',metadata)
        result=db.media.get('assets','logo')
        assert result['data']==b'\x00\xff' and result['metadata']['contentType']=='image/png'
        assert metadata=={'contentType':'image/png'}


@pytest.mark.parametrize('bad', [{'max_request_bytes':65536,'media_chunk_source_max_bytes':65536},{'max_request_bytes':262144,'media_chunk_source_max_bytes':0},{'max_request_bytes':'bad','media_chunk_source_max_bytes':65536}])
def test_invalid_capabilities_do_not_start_upload(tmp_path,bad):
    path=tmp_path/'input';path.write_bytes(b'x'*70000)
    with peer(lambda req:bad) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(PacificDBError):db.media.upload_file('assets',path)
        assert [f['action'] for f in server.frames]==['community_capabilities']


@pytest.mark.parametrize('bad', ['unsafe','duplicate','offset','zero','hash','oversize'])
def test_backup_export_bounds_and_atomic_destination(tmp_path,bad):
    source=b'backup'*20000; dest=tmp_path/'backup.json';dest.write_text('keep')
    seen=[]
    def reply(req):
        seen.append(req)
        if req['action']=='export_backup_manifest':
            file={'path':'data/a.bin','size_bytes':len(source),'sha256':'0'*64 if bad=='hash' else digest(source)}
            if bad=='unsafe':file['path']='../a.bin'
            return {'format':'pacificdb-full-backup-v1','backup':{'backup_id':'backup-1'},'files':[file,file] if bad=='duplicate' else [file]}
        data=source[req['offset']:req['offset']+req['max_bytes']]
        if bad=='zero':data=b''
        if bad=='oversize':data+=b'x'
        return {'path':req['path'],'offset':req['offset']+1 if bad=='offset' else req['offset'],'next_offset':req['offset']+len(data),'size_bytes':len(data),'sha256':digest(data),'data':base64.b64encode(data).decode()}
    with peer(reply) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(PacificDBError):db.backups.export('backup-1',dest,chunk_bytes=65536)
        assert dest.read_text()=='keep'
        assert list(tmp_path.iterdir())==[dest]


def test_backup_export_streamed_document_and_bounded_reads(tmp_path,monkeypatch):
    source=b'abc'*50000;input_path=tmp_path/'input.bin';input_path.write_bytes(source)
    state=MediaPeer(source)
    original=Path.open
    captured = []
    class BoundedReader:
        def __init__(self,handle):self.handle=handle
        def __getattr__(self,name):return getattr(self.handle,name)
        def __enter__(self):return self
        def __exit__(self,*args):return self.handle.__exit__(*args)
        def read(self,size=-1):
            assert 0<size<=4*1024*1024
            if captured: captured[0].database='changed_during_hashing'
            return self.handle.read(size)
    def bounded(path,*args,**kwargs):
        handle=original(path,*args,**kwargs)
        return BoundedReader(handle) if path==input_path and args and args[0]=='rb' else handle
    monkeypatch.setattr(Path,'open',bounded)
    with peer(state) as server, PacificDB(port=server.port,database='app') as db:
        captured.append(db)
        db.media.upload_file('assets',input_path,chunk_bytes=65536)
        assert all(req['dbName']=='app' for req in server.frames)
    def reply(req):
        if req['action']=='export_backup_manifest':return {'format':'pacificdb-full-backup-v1','backup':{'backup_id':'backup-1'},'files':[{'path':'data/a.bin','size_bytes':len(source),'sha256':digest(source)}]}
        data=source[req['offset']:req['offset']+req['max_bytes']]
        return {'path':req['path'],'offset':req['offset'],'next_offset':req['offset']+len(data),'size_bytes':len(data),'sha256':digest(data),'data':base64.b64encode(data).decode()}
    with peer(reply) as server, PacificDB(port=server.port,database='app') as db:
        dest=tmp_path/'backup.json';result=db.backups.export('backup-1',dest,chunk_bytes=65536)
        document=json.loads(dest.read_text())
        assert b''.join(base64.b64decode(chunk) for chunk in document['files'][0]['chunks'])==source
        assert result['size_bytes']==len(source) and len(document['files'][0]['chunks'])==3


def test_rename_failure_and_missing_input_are_typed_and_leave_existing_destination(tmp_path):
    source=b'x'*70000;input_path=tmp_path/'input';input_path.write_bytes(source)
    state=MediaPeer(source)
    with peer(state) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(PacificDBError) as got: db.media.upload_file('assets',tmp_path/'missing')
        assert got.value.code=='file_io_error' and server.frames==[]
        db.media.upload_file('assets',input_path,chunk_bytes=65536)
        destination=tmp_path/'directory';destination.mkdir();(destination/'sentinel').write_text('keep')
        with pytest.raises(PacificDBError) as got: db.media.download_file('media-1',destination)
        assert got.value.code=='file_io_error'
        assert (destination/'sentinel').read_text()=='keep'
        assert not any(p.name.startswith('.pacificdb-') for p in tmp_path.iterdir())


def test_in_memory_non_byte_memoryview_uses_byte_length():
    import array
    values=array.array('I',[1,2,3]);memory=memoryview(values);rows={}
    def reply(req):
        if req['action']=='insert':rows[req['data']['id']]=req['data'];return {'status':'ok'}
        return {'data':[rows[req['filter']['id']]]}
    with peer(reply) as server, PacificDB(port=server.port,database='app') as db:
        db.media.put('assets','numbers',memory)
        assert db.media.get('assets','numbers')['data']==memory.tobytes()


def test_wire_budget_and_resume_hash_are_checked_before_upload(tmp_path):
    path=tmp_path/'file';source=b'x'*70000;path.write_bytes(source)
    with peer(lambda req: {'max_request_bytes':160000,'media_chunk_source_max_bytes':65536}) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(PacificDBError) as got:db.media.upload_file('assets',path,content_type='x'*160000,chunk_bytes=65536)
        assert got.value.code=='media_request_too_large'
        assert [r['action'] for r in server.frames]==['community_capabilities']
    state=MediaPeer(source)
    state.after_begin=lambda:state.manifest.update(sha256='0'*64)
    with peer(state) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(PacificDBError):db.media.upload_file('assets',path,resume='media-1')
        assert not any(r['action']=='community_media_put_chunk' for r in server.frames)


def test_malformed_progress_and_boolean_chunk_index_are_refused(tmp_path):
    source=b'x'*70000;path=tmp_path/'file';path.write_bytes(source)
    state=MediaPeer(source)
    def reply(req):
        result=state(req)
        if req['action']=='community_media_put_chunk':result['received_indices']=[req['index'],req['index']]
        return result
    with peer(reply) as server, PacificDB(port=server.port,database='app') as db:
        with pytest.raises(MediaUploadError):db.media.upload_file('assets',path)
        assert not any(r['action']=='community_media_finalize' for r in server.frames)
    state=MediaPeer(source)
    def bool_index(req):
        result=state(req)
        if req['action']=='community_media_get_chunk':result['chunk']['index']=bool(req['index'])
        return result
    with peer(bool_index) as server, PacificDB(port=server.port,database='app') as db:
        db.media.upload_file('assets',path)
        with pytest.raises(PacificDBError):db.media.download_file('media-1',tmp_path/'out')
