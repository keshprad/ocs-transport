from coord import *
import re,datetime
rep=int(sys.argv[1]);tid=R.name+'-r'+str(rep)
msgs=[]
for x in map(json.loads,(R/'agent-messages.jsonl').read_text().splitlines()):
 if x['sender']!='spark-28a3':continue
 try:d=json.loads(x['text'])
 except ValueError:continue
 if d.get('test_id')==tid and d.get('kind')=='DONE':msgs.append(d)
assert len(msgs)==1;peer=msgs[0];assert peer['exit_codes']==[0,0]
local=json.loads((R/(tid+'-client-results.json')).read_text());out=[];times=[]
for l in local:
 p=next(x for x in peer['results'] if x['path']==l['path']);assert p['exit_code']==0
 rows=[line.split() for line in p['stdout'].splitlines() if re.match(r'^\s*65536\s+',line)];assert len(rows)==3
 lr=[x['raw'].split() for x in l['rows']]
 assert rows[1]==lr[2] and rows[2]==lr[1]
 assert rows[0][2:]==lr[0][2:]
 m=json.loads((R/l['metadata']).read_text());assert m['exit_code']==0
 times.append([datetime.datetime.fromisoformat(m[k]) for k in ['start_utc','end_utc']])
 out.append({'path':l['path'],'spark_7e64_tx':float(lr[1][3]),'spark_7e64_rx':float(lr[2][3]),'spark_28a3_tx':float(rows[1][3]),'spark_28a3_rx':float(rows[2][3]),'combined':float(rows[0][3]),'opposite_rows_exact_match':True})
overlap=(min(x[1] for x in times)-max(x[0] for x in times)).total_seconds();assert overlap>=30
result={'test_id':tid,'paths':out,'aggregate_forward':sum(x['spark_7e64_tx'] for x in out),'aggregate_reverse':sum(x['spark_28a3_tx'] for x in out),'aggregate_combined':sum(x['combined'] for x in out),'client_process_overlap_seconds':overlap,'peer_done_observed':True}
with (R/(tid+'-accepted.json')).open('x') as f:json.dump(result,f,indent=2)
s=json.loads((R/'RUN.json').read_text());assert tid not in s['processed_ids'];s['processed_ids'].append(tid);(R/'RUN.json').write_text(json.dumps(s,indent=2)+'\n')
post('ACCEPTED',**result);print(json.dumps(result,indent=2))
