from coord import *
import hashlib,re,datetime
P=R/'imported-peer-spark-28a3-20261108T223754596981Z/spark-28a3'
inventories=[]
for inv in R.glob('imported-*/spark-28a3/*.inventory.json'):
 data=json.loads(inv.read_text())
 for item in data['files']:
  rel=pathlib.PurePosixPath(item['path']);assert not rel.is_absolute() and '..' not in rel.parts
  p=inv.parent/rel;assert p.stat().st_size==item['bytes'] and hashlib.sha256(p.read_bytes()).hexdigest()==item['sha256'],str(p)
 inventories.append({'inventory':str(inv.relative_to(R)),'verified_files':len(data['files'])})
assert sorted(i['verified_files'] for i in inventories)==[11,231]
manifest=json.loads((R/'protocol-proposal.json').read_text())['cases'];assert len(manifest)==3
endpoints=[];overlaps=[]
msgs=[]
for raw in map(json.loads,(R/'agent-messages.jsonl').read_text().splitlines()):
 try:d=json.loads(raw['text'])
 except ValueError:continue
 if d.get('run_id')==R.name:msgs.append((raw,d))
for rep,c in enumerate(manifest,1):
 tid=c['test_id'];accepted=json.loads((R/(tid+'-accepted.json')).read_text());intervals=[]
 # All protocol events precede traffic and completion/acceptance precede the next assignment.
 kinds={}
 for raw,d in msgs:
  if d.get('test_id')==tid and d.get('kind') in ['ASSIGNMENT','ACK','READY','RESULTS_DONE','DONE','ACCEPTED']:kinds.setdefault(d['kind'],raw['utc'])
 assert all(k in kinds for k in ['ASSIGNMENT','ACK','READY','RESULTS_DONE','DONE','ACCEPTED'])
 assert kinds['ASSIGNMENT']<kinds['ACK']<kinds['READY']<kinds['RESULTS_DONE']<kinds['ACCEPTED'] and kinds['DONE']<kinds['ACCEPTED']
 if rep>1:assert prev_accept<kinds['ASSIGNMENT']
 prev_accept=kinds['ACCEPTED']
 for j in c['jobs']:
  path=j['path'];lp=next(R.glob(f'*-r{rep}-p{path}-client-*.json'));pm=P/(tid+f'-p{path}.completion.json')
  lm=json.loads(lp.read_text());sm=json.loads(pm.read_text())
  for host,m,cmd,f in [('spark-7e64',lm,j['client_command'],lp),('spark-28a3',sm,j['server_command'],pm)]:
   assert m['command']==cmd and m['hostname']==host and m['exit_code']==0
   start=datetime.datetime.fromisoformat(m['start_utc']);end=datetime.datetime.fromisoformat(m['end_utc']);assert (end-start).total_seconds()>=30
   intervals.append((start,end));endpoints.append({'host':host,'test_id':tid,'path':path,'metadata':str(f.relative_to(R)),'exit_code':0})
  assert lm['start_utc']>kinds['READY']
  ls=lp.with_suffix('.stdout').read_text();ps=(P/(tid+f'-p{path}.stdout')).read_text()
  assert lp.with_suffix('.stderr').exists() and (P/(tid+f'-p{path}.stderr')).exists()
  for s in [ls,ps]:assert 'Local results:' in s and 'Remote results:' in s and 'Mtu             : 1024[B]' in s and j['device'] in s
  rows=lambda s:[line.split() for line in s.splitlines() if re.match(r'^\s*65536\s+',line)]
  a,b=rows(ls),rows(ps);assert len(a)==len(b)==3 and a[1]==b[2] and a[2]==b[1] and a[0][2:]==b[0][2:]
  assert abs(float(a[0][3])-float(a[1][3])-float(a[2][3]))<=.021
 overlap=(min(i[1] for i in intervals)-max(i[0] for i in intervals)).total_seconds();assert overlap>=30;overlaps.append({'test_id':tid,'all_four_processes_overlap_seconds':overlap})
assert len(endpoints)==12
# Independently parse raw peer counter snapshots.
def counters(p):
 s=p.read_text();d={}
 for section in s.split('INTERFACE=')[1:]:
  iface=section.splitlines()[0]
  d.update({iface+'/'+m[1]:int(m[2]) for m in re.finditer(r'^\s+(\S+):\s+(\d+)\s*$',section.split('FILE=')[0],re.M)})
 d.update({m[1]:int(m[2]) for m in re.finditer(r'^FILE=(\S+)\n(\d+)\s*$',s,re.M)})
 return d
before_file=next(P.glob('baseline-*/stdout'));after_file=next(P.glob('final-counters-*/stdout'));b,a=counters(before_file),counters(after_file)
assert b.keys()==a.keys();delta={k:a[k]-v for k,v in b.items()};claimed=json.loads((P/'counter-deltas.json').read_text());assert all(claimed[k]['before']==b[k] and claimed[k]['after']==a[k] and claimed[k]['delta']==v for k,v in delta.items())
peer_counters={'sources':[str(p.relative_to(R)) for p in [before_file,after_file]],'verified_counters':len(delta),'deltas':delta,'nonzero_error_related':{k:v for k,v in delta.items() if v and any(t in k for t in ['err','drop','discard','crc','fault','link_down','fec','pause'])},'decreased':[k for k,v in delta.items() if v<0]}
(R/'peer-counter-deltas-verified.json').write_text(json.dumps(peer_counters,indent=2)+'\n')
for phase in ['down','delete','verify']:
 p=next(P.glob('cleanup-'+phase+'-*/completion.json'));assert json.loads(p.read_text())['exit_code']==0
s=next(P.glob('cleanup-verify-*/stdout')).read_text();assert '192.168.201.13' not in s and '192.168.201.0/24' not in s and 'b64a5439-4de2-4b7c-bd66-3d26a972c871' not in s
assert '169.254.238.243/16' in s and 'default via 10.50.0.1 dev wlP9s9' in s
assert json.loads((R/'local-cleanup-verification.json').read_text())['complete']
report={'complete':True,'verified_endpoint_records':12,'verified_repetitions':3,'inventories':inventories,'endpoints':endpoints,'concurrency':overlaps,'peer_counters_verified':len(delta),'both_network_cleanups_verified':True,'protocol_order_verified':True,'report_semantics':'Local TX, Remote exchanged peer TX/local incoming; reciprocal directional rows exact; combined checked with rounding tolerance0.021Gb/s','coordination_services':'Still available pending final acknowledgment exchange'}
(R/'measurement-audit.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='endpoints'},indent=2));print('Peer nonzero error-related',peer_counters['nonzero_error_related'])
