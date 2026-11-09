#!/usr/bin/env python3
"""Create a credential-free Git snapshot without changing original RDMA evidence."""
import argparse,base64,hashlib,json,pathlib,re,stat,zlib

def sha(b): return hashlib.sha256(b).hexdigest()
BEARER=re.compile(rb'(?i)Bearer[ \t]+([A-Za-z0-9_~+/=-]{16,})')
MARKER=b'REDACTED_MAILBOX_TOKEN'
def excluded(p):
 if '__pycache__' in p.parts or p.suffix in ('.pyc','.pyo'):return 'Python cache'
 if p.name.endswith(('.tar.gz','.tgz','.zip','.tar')):return 'Original archive; may contain credentials; original retained outside Git'
 if 'mailbox-key' in p.name.lower() or p.suffix=='.key':return 'Standalone credential file'
 return None

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--source',default='rdma-results');ap.add_argument('--output',default='rdma-benchmark-snapshot');args=ap.parse_args()
 source=pathlib.Path(args.source);out=pathlib.Path(args.output);files=sorted(p for p in source.rglob('*') if p.is_file());originals={str(p.relative_to(source)):sha(p.read_bytes()) for p in files}
 tokens=set()
 for p in files:
  if 'mailbox-key' in p.name.lower():
   b=p.read_bytes().strip()
   if 16<=len(b)<=256 and not re.search(rb'\s',b):tokens.add(b)
  if not excluded(p):tokens.update(m[1] for m in BEARER.finditer(p.read_bytes()) if m[1]!=MARKER)
 if not tokens:raise RuntimeError('No credentials discovered; investigate before exporting')
 def clean(b):
  for token in sorted(tokens,key=len,reverse=True):b=b.replace(token,MARKER)
  return b
 def inspect_encoded(b):
  # Mailbox runner offers embed zlib/base64 Python. Verify their contents too.
  for m in re.finditer(rb'ZLIB_BASE64=([A-Za-z0-9+/=]+)',b):
   decoded=zlib.decompress(base64.b64decode(m[1]))
   if any(t in decoded for t in tokens) or any(x[1]!=MARKER for x in BEARER.finditer(decoded)):raise RuntimeError('Embedded encoded credentials need explicit sanitization')
 entries=[];exclusions=[];redactions=0
 def write(rel,b):
  p=out/rel;p.parent.mkdir(parents=True,exist_ok=True)
  with p.open('xb') as f:f.write(b)
 for p in files:
  rel=str(p.relative_to(source));b=p.read_bytes();reason=excluded(p)
  if reason:exclusions.append(dict(original_path=rel,original_sha256=sha(b),bytes=len(b),reason=reason));continue
  if p.is_symlink():raise RuntimeError('Refuse symlink: '+rel)
  sanitized=clean(b);inspect_encoded(sanitized);target='evidence/'+rel;write(target,sanitized)
  changed=b!=sanitized;redactions+=changed
  entries.append(dict(original_path=rel,snapshot_path=target,original_sha256=sha(b),sanitized_sha256=sha(sanitized),original_bytes=len(b),sanitized_bytes=len(sanitized),redacted=changed))
 # Extract only peer records actually present; do not manufacture missing peer streams.
 messages={}
 for p in files:
  if p.name!='response.body':continue
  try:d=json.loads(p.read_bytes())
  except (ValueError,UnicodeError):continue
  if not isinstance(d,dict):continue
  for m in d.get('messages',[]):
   if m.get('sender')!='spark-7e64':continue
   b=json.dumps(m,ensure_ascii=False,sort_keys=True).encode();key=sha(b)
   if key not in messages:messages[key]=(m,str(p.relative_to(source)))
 extracted=[]
 for number,(key,(m,parent)) in enumerate(sorted(messages.items(),key=lambda x:(x[1][0].get('utc',''),x[0])),1):
  prefix=f'extracted-peer/spark-7e64/{number:04d}-{key[:12]}'
  raw=json.dumps(m,ensure_ascii=False,sort_keys=True,indent=2).encode()+b'\n';sanitized=clean(raw);write(prefix+'.json',sanitized)
  extracted.append(dict(snapshot_path=prefix+'.json',source_path=parent,source_sha256=originals[parent],message_sha256=key,original_payload_sha256=sha(raw),sanitized_sha256=sha(sanitized),kind='mailbox peer record'))
  try:record=json.loads(m['text'])
  except (ValueError,TypeError):continue
  if not isinstance(record,dict):continue
  for i,result in enumerate(record.get('results',[])):
   if not isinstance(result,dict):continue
   for stream in ('stdout','stderr'):
    payload=result.get(stream)
    if not isinstance(payload,str) or (stream=='stdout' and '\n' not in payload):continue # remote filenames are references, not contents
    raw=payload.encode();sanitized=clean(raw);path=prefix+f'-result{i+1}.{stream}';write(path,sanitized)
    extracted.append(dict(snapshot_path=path,source_path=parent,source_sha256=originals[parent],message_sha256=key,original_payload_sha256=sha(raw),sanitized_sha256=sha(sanitized),kind='embedded peer output'))
   rows=result.get('rows')
   if isinstance(rows,list):
    raw=('\n'.join(row['raw'] for row in rows if isinstance(row,dict) and isinstance(row.get('raw'),str))+'\n').encode();sanitized=clean(raw);path=prefix+f'-result{i+1}.reported-rows.txt';write(path,sanitized)
    extracted.append(dict(snapshot_path=path,source_path=parent,source_sha256=originals[parent],message_sha256=key,original_payload_sha256=sha(raw),sanitized_sha256=sha(sanitized),kind='peer-reported raw measurement rows'))
 manifest=dict(schema_version=1,source_directory=str(source),redaction_marker=MARKER.decode(),credential_fingerprints_sha256=sorted(sha(t) for t in tokens),files=entries,exclusions=exclusions,extracted_peer=extracted,notes=['Original evidence is not modified.','Original archives excluded; available evidence already exists separately.','Peer records extracted from mailbox messages available locally. Full peer original raw archives reside on spark-7e64 and are not available in this local source.','Original hashes describe unsanitized evidence; sanitized hashes describe exported bytes. Embedded old hashes in evidence are historical claims, not regenerated hashes.','This manifest does not hash itself; its bytes and documentation are protected by the Git commit.'])
 write('manifest.json',json.dumps(manifest,indent=2).encode()+b'\n')
 # Revalidate source and all exported payloads; never emit secret values.
 assert originals=={str(p.relative_to(source)):sha(p.read_bytes()) for p in files},'Original evidence changed'
 for record in entries+extracted:
  b=(out/record['snapshot_path']).read_bytes();assert sha(b)==record['sanitized_sha256'];assert not any(t in b for t in tokens);assert not any(m[1]!=MARKER for m in BEARER.finditer(b));inspect_encoded(b)
 audit=dict(original_files_verified_unchanged=len(originals),exported_files=len(entries),redacted_files=redactions,excluded_files=len(exclusions),extracted_peer_files=len(extracted),credential_values_detected=len(tokens),all_hashes_verified=True,known_credentials_absent=True,unredacted_bearer_values_absent=True,embedded_runner_payloads_scanned=True)
 write('sanitization-audit.json',json.dumps(audit,indent=2).encode()+b'\n');print(json.dumps(audit,indent=2))
if __name__=='__main__':main()
