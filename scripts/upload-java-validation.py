#!/usr/bin/env python3
"""Explicitly upload a tested bundle for USER_MANAGED validation; never publish it."""
import argparse
import json
import os
from pathlib import Path
import ssl
import time
import urllib.request
import urllib.parse
import uuid

BASE='https://central.sonatype.com/api/v1/publisher'
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args,**kwargs):return None

def upload(path):
    token=os.environ.get('CENTRAL_TOKEN')
    if not token:raise ValueError('Central token not configured')
    if not path.is_file() or path.stat().st_size>256*1024*1024:raise ValueError('invalid bundle size')
    opener=urllib.request.build_opener(NoRedirect(),urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    def request(endpoint,data,content_type=None):
        headers={'Authorization':'Bearer '+token}
        if content_type:headers['Content-Type']=content_type
        req=urllib.request.Request(BASE+endpoint,data=data,headers=headers,method='POST')
        try:
            with opener.open(req,timeout=30) as response:
                value=response.read(1024*1024+1)
                if len(value)>1024*1024:raise ValueError('oversized Central response')
                return value
        except Exception:raise RuntimeError('Central upload/status request failed; inspect deployment privately') from None
    boundary='pacificdb-'+uuid.uuid4().hex
    body=(f'--{boundary}\r\nContent-Disposition: form-data; name="bundle"; filename="central-bundle.zip"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()+path.read_bytes()+f'\r\n--{boundary}--\r\n'.encode()
    deployment=str(uuid.UUID(request('/upload?publishingType=USER_MANAGED',body,'multipart/form-data; boundary='+boundary).decode().strip()))
    deadline=time.monotonic()+1800
    while time.monotonic()<deadline:
        result=json.loads(request('/status?id='+urllib.parse.quote(deployment),b''));state=result.get('deploymentState')
        if state=='VALIDATED':print(json.dumps({'deployment_id':deployment,'state':state,'publication':'requires separate owner action'}));return
        if state not in ('PENDING','VALIDATING'):raise RuntimeError('Central validation failed or deployment state changed; inspect deployment privately')
        time.sleep(5)
    raise RuntimeError('Central validation timed out; inspect deployment privately')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('bundle',type=Path);parser.add_argument('--upload',action='store_true');args=parser.parse_args()
    if not args.upload:parser.error('remote validation upload requires explicit --upload')
    upload(args.bundle)

if __name__=='__main__':main()
