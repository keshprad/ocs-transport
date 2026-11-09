import pathlib,json,hashlib,tarfile,datetime,socket,urllib.request,uuid,io,sys
ROOT=pathlib.Path(__file__).resolve().parent
now=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
HOST=socket.gethostname(); stem=f'{HOST}-'+('transfer-evidence' if '--supplement' in sys.argv else 'raw')+f'-{now}-{uuid.uuid4().hex[:8]}'
archive=ROOT/(stem+'.tar.gz'); inventory=ROOT/(stem+'.inventory.json')
files=[p for p in ROOT.rglob('*') if p.is_file() and not p.is_symlink() and p.name!='agent-mailbox-key.txt' and not p.name.endswith(('.tar.gz','.inventory.json'))]
if '--supplement' in sys.argv:
 files=[p for p in files if '.upload-' in p.name or any(q.startswith(('archive-upload-','archive-inventory-verify-')) for q in p.relative_to(ROOT).parts)]
records=[];snapshot={}
for p in sorted(files):
 b=p.read_bytes();snapshot[p]=b;records.append(dict(path=str(p.relative_to(ROOT)),bytes=len(b),sha256=hashlib.sha256(b).hexdigest()))
with inventory.open('x') as f:json.dump(dict(hostname=HOST,utc=now,files=records),f,indent=2)
with tarfile.open(archive,'x:gz') as tar:
 for p in sorted(files):
  info=tarfile.TarInfo(f'{HOST}/'+str(p.relative_to(ROOT)));info.size=len(snapshot[p]);info.mode=0o600;tar.addfile(info,io.BytesIO(snapshot[p]))
 tar.add(inventory,arcname=f'{HOST}/'+inventory.name,recursive=False)
archive.chmod(0o600)
raw=archive.read_bytes();digest=hashlib.sha256(raw).hexdigest()
meta=dict(archive=str(archive),bytes=len(raw),sha256=digest,hostname=HOST,start_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),invocation='python3 rdma-results/package_upload.py',method='POST',url='http://169.254.135.137:12081/upload',files=len(records))
key=(ROOT/'agent-mailbox-key.txt').read_text().strip();headers={'Authorization':'Bearer '+key,'Content-Type':'application/gzip','Content-Length':str(len(raw))};meta['headers']=headers
(ROOT/(stem+'.upload-request.json')).write_text(json.dumps(meta,indent=2))
try:
 with urllib.request.urlopen(urllib.request.Request(meta['url'],data=raw,headers=headers,method='POST'),timeout=120) as response:
  body=response.read(); status=response.status;res=dict(status=status,headers=list(response.headers.items()),end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
 (ROOT/(stem+'.upload-response.body')).write_bytes(body);receipt=json.loads(body);res['receipt']=receipt;res['sha256_match']=receipt.get('sha256')==digest;res['exit_code']=0 if status==201 and res['sha256_match'] else 1
except Exception as exc:res=dict(error=repr(exc),exit_code=1,end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
(ROOT/(stem+'.upload-result.json')).write_text(json.dumps(res,indent=2));print(json.dumps(dict(archive=meta['archive'],bytes=meta['bytes'],sha256=meta['sha256'],files=meta['files'],response=res),indent=2));raise SystemExit(res['exit_code'])
