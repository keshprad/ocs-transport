import hashlib,json,pathlib,re
ROOT=pathlib.Path(__file__).resolve().parent
imports=[ROOT/'imported-peer-spark-28a3-20261108T044117813137Z',ROOT/'imported-peer-spark-28a3-20261108T044157394028Z']
reports=[]
for directory in imports:
 for inventory in directory.rglob('*.inventory.json'):
  data=json.loads(inventory.read_text());mismatches=[]
  for item in data['files']:
   path=inventory.parent/item['path']
   if not path.is_file():mismatches.append({'path':item['path'],'error':'missing'});continue
   with path.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
   if path.stat().st_size!=item['bytes'] or digest!=item['sha256']:mismatches.append({'path':item['path'],'error':'hash/size'})
  reports.append({'inventory':str(inventory.relative_to(ROOT)),'files':len(data['files']),'mismatches':mismatches})
(ROOT/'peer-inventory-verification.json').write_text(json.dumps(reports,indent=2)+'\n')
peer=imports[0]/'spark-28a3'
before_file=next(peer.glob('baseline-*/stdout'));after_file=next(peer.glob('final-counters-*/stdout'))
def sections(path):
 return [{m[1]:int(m[2]) for m in re.finditer(r'^\s+(\S+):\s+(\d+)\s*$',section,re.M)} for section in path.read_text().split('NIC statistics:')[1:]]
before=sections(before_file);after=sections(after_file)
assert len(before)==len(after)==2
claimed=json.loads(next(peer.glob('*.counter-deltas.json')).read_text());counters={}
for dev,old,new in zip(('enp1s0f1np1','enP2p1s0f1np1'),before,after):
 deltas={k:new[k]-v for k,v in old.items() if k in new}
 mismatch=[k for k,v in deltas.items() if claimed.get(dev,{}).get(k,{}).get('delta')!=v]
 counters[dev]={'baseline':str(before_file.relative_to(ROOT)),'final':str(after_file.relative_to(ROOT)),'checked_counters':len(deltas),'claim_mismatches':mismatch,'all_deltas':deltas,'nonzero_error_related_deltas':{k:v for k,v in deltas.items() if v and any(x in k for x in ('err','drop','discard','crc','fault','link_down','fec','pause'))},'decreasing_counters':[k for k,v in deltas.items() if v<0]}
(ROOT/'peer-counter-comparison-verified.json').write_text(json.dumps(counters,indent=2)+'\n')
cleanup={}
for label in ('cleanup-profile-down','cleanup-profile-delete','cleanup-verification'):
 directory=next(peer.glob(label+'-*'))
 meta=json.loads((directory/'completion.json').read_text());out=(directory/'stdout').read_text()
 cleanup[label]={'source':str(directory.relative_to(ROOT)),'exit_code':meta.get('exit_code')}
 assert meta.get('exit_code')==0
 if label=='cleanup-profile-delete':assert '2ebec186-c595-400c-a716-7ab7af5ca36e' in out and 'deleted' in out
 if label=='cleanup-verification':
  assert '192.168.201.13' not in out and '2ebec186-c595-400c-a716-7ab7af5ca36e' not in out
  assert '169.254.238.243' in out and 'default via 10.50.0.1 dev wlP9s9' in out
cleanup['temporary_profile_and_address_absent']=True
cleanup['original_first_path_and_default_intact']=True
(ROOT/'peer-cleanup-verification.json').write_text(json.dumps(cleanup,indent=2)+'\n')
assert len(reports)==2 and all(not r['mismatches'] for r in reports)
assert all(not c['claim_mismatches'] for c in counters.values())
print(json.dumps({'inventories':reports,'counter_checks':{d:{k:v for k,v in c.items() if k!='all_deltas'} for d,c in counters.items()},'cleanup':cleanup},indent=2))
