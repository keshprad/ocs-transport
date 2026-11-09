import hashlib,json,pathlib,shutil,sys,tarfile
ROOT=pathlib.Path(__file__).resolve().parent
archive=pathlib.Path(sys.argv[1]).resolve()
expected=sys.argv[2]
actual=hashlib.file_digest(archive.open('rb'),'sha256').hexdigest()
if actual!=expected: raise ValueError('Archive SHA256 mismatch')
dest=ROOT/('imported-'+archive.name.removesuffix('.tar.gz'))
if dest.exists():raise FileExistsError('Preserving existing import: '+str(dest))
with tarfile.open(archive,'r:gz') as tar:
 members=tar.getmembers()
 if sum(m.size for m in members)>4*1024**3:raise ValueError('Expanded archive exceeds4GiB; inspect before importing')
 paths=set()
 for m in members:
  path=pathlib.PurePosixPath(m.name)
  if path.is_absolute() or '..' in path.parts or not (m.isfile() or m.isdir()):raise ValueError('Unsafe archive entry '+m.name)
  if m.name in paths:raise ValueError('Duplicate archive entry '+m.name)
  paths.add(m.name)
 dest.mkdir()
 manifest=[]
 for m in members:
  path=dest/m.name
  if m.isdir():path.mkdir(parents=True,exist_ok=True);continue
  path.parent.mkdir(parents=True,exist_ok=True)
  with tar.extractfile(m) as src,path.open('xb') as out:shutil.copyfileobj(src,out)
  with path.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
  manifest.append({'path':m.name,'bytes':path.stat().st_size,'sha256':digest})
report={'archive':archive.name,'archive_sha256':actual,'destination':str(dest),'files':manifest}
(ROOT/(archive.name+'.import.json')).write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'archive':archive.name,'sha256':actual,'destination':str(dest),'files':len(manifest)},indent=2))
