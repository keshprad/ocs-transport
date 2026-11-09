#!/usr/bin/env python3
"""Create a credential-redacted, immutable Git snapshot; keep raw evidence intact."""
import hashlib,json,pathlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCE=ROOT/'rdma-results'
DEST=ROOT/'versioned-results'/'rdma-results'
if DEST.exists():raise SystemExit('Refusing to overwrite an existing snapshot')
keys=[p.read_bytes().strip() for p in SOURCE.rglob('agent-mailbox-key.txt')]
assert keys and all(len(k)>=16 for k in keys)
DEST.mkdir(parents=True)
manifest={'source':'rdma-results','snapshot':'versioned-results/rdma-results','redaction':'Exact mailbox credential bytes replaced with [REDACTED_MAILBOX_TOKEN]. Original files remain untouched.','files':[],'excluded':[]}
for p in sorted(SOURCE.rglob('*')):
 if not p.is_file():continue
 rel=p.relative_to(SOURCE)
 if p.name=='agent-mailbox-key.txt' or '__pycache__' in rel.parts or p.suffix in ('.gz','.pyc'):
  manifest['excluded'].append({'path':str(rel),'reason':'credential file' if p.name=='agent-mailbox-key.txt' else 'original archive or generated Python cache'});continue
 original=p.read_bytes();data=original
 for key in keys:data=data.replace(key,b'[REDACTED_MAILBOX_TOKEN]')
 assert not any(key in data for key in keys)
 out=DEST/rel;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data)
 manifest['files'].append({'path':str(rel),'bytes':len(data),'original_sha256':hashlib.sha256(original).hexdigest(),'snapshot_sha256':hashlib.sha256(data).hexdigest(),'redacted':data!=original})
(ROOT/'versioned-results'/'SNAPSHOT-MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'files':len(manifest['files']),'redacted':sum(x['redacted'] for x in manifest['files']),'excluded':len(manifest['excluded'])}))
