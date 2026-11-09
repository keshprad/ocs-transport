from coord import *
import concurrent.futures,datetime,re
rep=int(sys.argv[1]);case=json.loads((R/'protocol-proposal.json').read_text())['cases'][rep-1];tid=case['test_id']
# This script is invoked only after coordinator has observed ACK and READY.
state=R/(tid+'-client-started.json')
with state.open('x') as f:json.dump({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'case':case},f,indent=2)
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
 fs=[ex.submit(logged,f'r{rep}-p{j["path"]}-client',j['client_command']) for j in case['jobs']]
 for f in fs:f.result()
results=[]
for j in case['jobs']:
 p=next(R.glob(f'*-r{rep}-p{j["path"]}-client-*.stdout'));s=p.read_text(); rows=[];section='combined'
 for line in s.splitlines():
  if 'Local results:' in line:section='local_tx'
  if 'Remote results:' in line:section='remote_tx_local_rx'
  if re.match(r'^\s*65536\s+',line):
   fields=line.split();rows.append({'section':section,'raw':line,'gbps':float(fields[3])})
 assert len(rows)==3 and {x['section'] for x in rows}=={'combined','local_tx','remote_tx_local_rx'},rows
 assert abs(rows[0]['gbps']-sum(x['gbps'] for x in rows[1:]))<=.021
 results.append({'path':j['path'],'rows':rows,'stdout':p.name,'metadata':p.with_suffix('.json').name})
(R/(tid+'-client-results.json')).write_text(json.dumps(results,indent=2)+'\n')
post('RESULTS_DONE',test_id=tid,results=results,exit_codes=[0,0])
print(json.dumps(results,indent=2))
