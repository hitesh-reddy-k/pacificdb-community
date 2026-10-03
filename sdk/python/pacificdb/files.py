"""Bounded, verified client-side transfers; no implicit write retry."""
import base64
import hashlib
import json
import mimetypes
import os
import re
import stat
import tempfile
from contextlib import contextmanager
from functools import wraps
from pathlib import Path
from .errors import PacificDBError, MediaUploadError

MAX_CHUNK = 4 * 1024 * 1024
MIN_CHUNK = RESERVE = 64 * 1024
MAX_INTEGER = 2**53 - 1


def _fail(code='invalid_transfer_response'):
    return PacificDBError(code, code)


def _file_io(operation):
    @wraps(operation)
    def run(*args, **kwargs):
        try: return operation(*args, **kwargs)
        except OSError: failure = _fail('file_io_error')
        raise failure from None
    return run


def _integer(value, minimum=0, maximum=MAX_INTEGER):
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise _fail()
    return value


def _hash(value):
    if not isinstance(value, str) or not re.fullmatch('[a-fA-F0-9]{64}', value): raise _fail()
    return value.lower()


def _decode(value, maximum):
    if not isinstance(value, str) or len(value) > 4*((maximum+2)//3): raise _fail()
    try: result = base64.b64decode(value, validate=True)
    except (ValueError, UnicodeError): raise _fail() from None
    if len(result) > maximum or base64.b64encode(result).decode() != value: raise _fail()
    return result


def _capture(client, collection=None):
    with client._lock:
        if not client._database: raise _fail('database_required')
        if collection is not None: client._name(collection, 'collection')
        return client._scoped_request(), client._database


def _manifest(value, database, collection=None, media_id=None):
    if not isinstance(value, dict) or not isinstance(value.get('id'), str) or not value['id']: raise _fail()
    if value.get('database') != database or not isinstance(value.get('collection'), str) or not value['collection']:
        raise _fail('media_scope_mismatch')
    if collection is not None and value['collection'] != collection: raise _fail('media_scope_mismatch')
    if media_id is not None and value['id'] != media_id: raise _fail('media_scope_mismatch')
    size = _integer(value.get('size_bytes'), 1)
    _integer(value.get('chunk_count'), 1, size)
    _hash(value.get('sha256'))
    return value


@contextmanager
def _destination(path):
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix='.pacificdb-', dir=path.absolute().parent)
    try:
        with os.fdopen(fd, 'wb') as output: yield output
        os.replace(temporary, path)
    finally:
        try: os.unlink(temporary)
        except FileNotFoundError: pass


def put(client, collection, media_id, data, metadata=None):
    request, _ = _capture(client, collection)
    client._name(media_id, 'media_id')
    if not isinstance(data, (bytes, bytearray, memoryview)): raise _fail('invalid_media_data')
    data = bytes(data)
    metadata = client._object({} if metadata is None else metadata)
    encoded = base64.b64encode(data).decode()
    return request({'action':'insert', 'collection':collection, 'data':{**metadata, 'id':media_id, 'kind':'media',
        'dataBase64':encoded, 'sizeBytes':len(data), 'encoding':'base64'}})


def get(client, collection, media_id):
    client._name(media_id, 'media_id')
    row = client.find_one(collection, {'id':media_id})
    if not row or row.get('id') != media_id or row.get('kind') != 'media' or row.get('encoding') != 'base64': raise _fail('media_not_found')
    size = _integer(row.get('sizeBytes'), 0, 64 * 1024 * 1024)
    data = _decode(row.get('dataBase64'), size)
    if len(data) != size: raise _fail()
    return {'data':data, 'metadata':{key:value for key,value in row.items() if key != 'dataBase64'}}


@_file_io
def upload_file(client, collection, path, *, content_type=None, chunk_bytes=None, resume=None):
    request, database = _capture(client, collection)
    path = Path(path)
    content_type = content_type or mimetypes.guess_type(path.name)[0] or 'application/octet-stream'
    client._name(content_type, 'content_type')
    if resume is not None: client._name(resume, 'media_id')
    if not path.is_file(): raise _fail('file_io_error')
    with path.open('rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode): raise _fail('media_file_not_regular')
        size = _integer(info.st_size)
        if not size: raise _fail('media_file_empty')
        capabilities = request({'action':'community_capabilities'})
        if not isinstance(capabilities, dict): raise _fail()
        wire_limit = _integer(capabilities.get('max_request_bytes'), RESERVE+1)
        engine_chunk = _integer(capabilities.get('media_chunk_source_max_bytes'), 1)
        safe = (wire_limit - RESERVE)*3//4
        if chunk_bytes is not None: _integer(chunk_bytes, MIN_CHUNK, MAX_CHUNK)
        chunk_size = min(MAX_CHUNK, engine_chunk, safe, chunk_bytes if chunk_bytes is not None else safe)
        if chunk_size < MIN_CHUNK: raise _fail('media_chunk_limit_too_small')
        count = (size + chunk_size - 1)//chunk_size
        whole = hashlib.sha256()
        while chunk := source.read(chunk_size): whole.update(chunk)
        expected = whole.hexdigest()
        source.seek(0)
        def send(command):
            if request.wire_bytes(command) > wire_limit: raise _fail('media_request_too_large')
            return request(command)
        begun = send({'action':'community_media_begin','collection':collection,
            'filename':path.name,'content_type':content_type,'size_bytes':size,'chunk_count':count,
            'sha256':expected, **({'resume_id':resume} if resume else {})})
        media = _manifest(begun.get('media') if isinstance(begun,dict) else None, database, collection, resume)
        if any(media.get(key) != value for key,value in {'filename':path.name,'content_type':content_type,'size_bytes':size,'sha256':expected}.items()):
            raise _fail('media_resume_mismatch')
        if media.get('status') == 'ready': return media
        if media.get('status') != 'uploading' or media['chunk_count'] != count: raise _fail('media_resume_mismatch')
        received = set()
        received_bytes = 0
        def progress(value, added=None):
            nonlocal received, received_bytes
            if not isinstance(value, dict): raise _fail()
            indices = value.get('received_indices')
            if not isinstance(indices, list) or len(indices) > count: raise _fail()
            for index in indices: _integer(index, 0, count-1)
            updated = set(indices)
            if len(updated) != len(indices) or not received <= updated or added is not None and added not in updated: raise _fail()
            actual = sum(min(chunk_size, size-index*chunk_size) for index in updated)
            if _integer(value.get('received_bytes')) != actual or _integer(value.get('received_chunks')) != len(updated): raise _fail()
            received, received_bytes = updated, actual
        progress(media)
        whole = hashlib.sha256()
        try:
            for index in range(count):
                chunk = source.read(min(chunk_size, size-index*chunk_size))
                if len(chunk) != min(chunk_size, size-index*chunk_size): raise _fail('media_file_changed')
                whole.update(chunk)
                if index in received: continue
                stored = send({'action':'community_media_put_chunk','media_id':media['id'],'index':index,
                    'data':base64.b64encode(chunk).decode(),'size_bytes':len(chunk),'sha256':hashlib.sha256(chunk).hexdigest()})
                progress(stored.get('media',stored) if isinstance(stored,dict) else None,index)
            after = os.fstat(source.fileno())
            if whole.hexdigest() != expected or source.read(1) or after.st_size != size or after.st_mtime_ns != info.st_mtime_ns:
                raise _fail('media_file_changed')
            finalized = send({'action':'community_media_finalize','media_id':media['id']})
            ready = _manifest(finalized.get('media') if isinstance(finalized,dict) else None,database,collection,media['id'])
            if ready.get('status') != 'ready' or ready['size_bytes'] != size or _hash(ready['sha256']) != expected or ready['chunk_count'] != count: raise _fail()
            return ready
        except (PacificDBError, OSError) as error:
            next_chunk = next((index for index in range(count) if index not in received),count)
            failure = MediaUploadError('media_upload_interrupted','Media upload interrupted; resume explicitly',
                error.response if isinstance(error,PacificDBError) else None, upload_id=media['id'], next_chunk=next_chunk,
                received_chunks=len(received),received_bytes=received_bytes,
                resumable=getattr(error,'code','') not in ('media_file_changed','media_chunk_conflict','media_upload_expired','media_upload_not_resumable'))
        raise failure from None


@_file_io
def download_file(client, media_id, destination, *, collection=None):
    request, database = _capture(client, collection)
    client._name(media_id, 'media_id')
    response = request({'action':'community_media_get','media_id':media_id})
    manifest = _manifest(response.get('media') if isinstance(response,dict) else None,database,collection,media_id)
    if manifest.get('status') != 'ready': raise _fail('media_not_ready')
    whole = hashlib.sha256(); total = 0
    with _destination(destination) as output:
        for index in range(manifest['chunk_count']):
            response = request({'action':'community_media_get_chunk','media_id':media_id,'index':index})
            chunk = response.get('chunk') if isinstance(response,dict) else None
            if not isinstance(chunk,dict) or chunk.get('media_id') != media_id or _integer(chunk.get('index')) != index: raise _fail()
            data = _decode(chunk.get('data'), min(MAX_CHUNK,manifest['size_bytes']-total))
            if not data or _integer(chunk.get('size_bytes')) != len(data) or _hash(chunk.get('sha256')) != hashlib.sha256(data).hexdigest(): raise _fail()
            output.write(data); whole.update(data); total += len(data)
        checksum = whole.hexdigest()
        if total != manifest['size_bytes'] or checksum != _hash(manifest['sha256']): raise _fail('media_checksum_mismatch')
    return {'id':media_id,'destination':str(destination),'size_bytes':total,'sha256':checksum}


@_file_io
def export_backup(client, backup_id, destination, *, chunk_bytes=1024*1024):
    request = client._scoped_request()
    client._name(backup_id, 'backup_id'); _integer(chunk_bytes,1,1024*1024)
    manifest = request({'action':'export_backup_manifest','backup_id':backup_id})
    if not isinstance(manifest,dict) or manifest.get('format') != 'pacificdb-full-backup-v1' or not isinstance(manifest.get('backup'),dict) or manifest['backup'].get('backup_id') != backup_id or not isinstance(manifest.get('files'),list): raise _fail()
    paths=set()
    for file in manifest['files']:
        if not isinstance(file,dict): raise _fail()
        path=file.get('path')
        if not isinstance(path,str) or not path or len(path)>4096 or '\\' in path or ':' in path or any(ord(c)<32 or ord(c)==127 for c in path) or any(p in ('','.','..') for p in path.split('/')) or path in paths: raise _fail('invalid_backup_path')
        paths.add(path); _integer(file.get('size_bytes')); _hash(file.get('sha256'))
    total=0
    with _destination(destination) as output:
        def write(value): output.write(value.encode('utf-8'))
        def encode(value): return json.dumps(value,ensure_ascii=False,separators=(',',':'))
        write('{"format":'+encode(manifest['format'])+',"backup":'+encode(manifest['backup'])+',"files":[')
        for number,file in enumerate(manifest['files']):
            write((',' if number else '')+'{"path":'+encode(file['path'])+',"size_bytes":'+str(file['size_bytes'])+',"sha256":'+encode(file['sha256'])+',"chunks":[')
            offset=0; checksum=hashlib.sha256(); chunk_index=0
            while offset < file['size_bytes']:
                maximum=min(chunk_bytes,file['size_bytes']-offset)
                chunk=request({'action':'export_backup_file_chunk','backup_id':backup_id,'path':file['path'],'offset':offset,'max_bytes':maximum})
                if not isinstance(chunk,dict): raise _fail()
                data=_decode(chunk.get('data'),maximum)
                if not data or chunk.get('path') != file['path'] or _integer(chunk.get('offset')) != offset or _integer(chunk.get('next_offset')) != offset+len(data) or _integer(chunk.get('size_bytes')) != len(data) or _hash(chunk.get('sha256')) != hashlib.sha256(data).hexdigest(): raise _fail()
                write((',' if chunk_index else '')+encode(chunk['data'])); checksum.update(data)
                offset+=len(data); total+=len(data); chunk_index+=1
            if checksum.hexdigest() != _hash(file['sha256']): raise _fail('backup_checksum_mismatch')
            write(']}')
        write(']}\n')
    return {'backup_id':backup_id,'destination':str(destination),'files':len(manifest['files']),'size_bytes':total}
