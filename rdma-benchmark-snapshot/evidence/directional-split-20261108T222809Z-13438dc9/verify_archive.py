import pathlib,tarfile,json,hashlib
R=pathlib.Path(__file__).resolve().parent;p=next(R.glob('spark-28a3-raw-*.tar.gz'))
with tarfile.open(p) as t:
 inv=[m for m in t.getmembers() if m.name.endswith('.inventory.json')][0];data=json.load(t.extractfile(inv))
 for r in data['files']:
  b=t.extractfile('spark-28a3/'+r['path']).read();assert len(b)==r['bytes'] and hashlib.sha256(b).hexdigest()==r['sha256'],r['path']
print('Verified all',len(data['files']),'inventory entries; archiveSHA256',hashlib.sha256(p.read_bytes()).hexdigest())
