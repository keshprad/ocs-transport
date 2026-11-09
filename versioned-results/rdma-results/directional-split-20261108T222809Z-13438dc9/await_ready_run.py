from coord import *
import time
rep=int(sys.argv[1]);case=json.loads((R/'protocol-proposal.json').read_text())['cases'][rep-1];tid=case['test_id']
for _ in range(15):
 msgs=[]
 for line in (R/'agent-messages.jsonl').read_text().splitlines():
  x=json.loads(line)
  if x['sender']!='spark-28a3':continue
  try:d=json.loads(x['text'])
  except ValueError:continue
  if d.get('run_id')==R.name and d.get('test_id')==tid:msgs.append(d)
 ack=next((m for m in msgs if m.get('kind')=='ACK'),None);ready=next((m for m in msgs if m.get('kind')=='READY'),None)
 if ack and ready:
  assert ack['jobs']==case['jobs'];print('ACK/READY observed',json.dumps(ready),flush=True)
  (R/(tid+'-readiness-observed.json')).write_text(json.dumps({'ack':ack,'ready':ready},indent=2)+'\n')
  logged('r'+str(rep)+'-client-supervisor',['python3',str(R/'run_clients.py'),str(rep)]);break
 if any(m.get('kind') in ['ERROR','FAILED'] for m in msgs):raise RuntimeError(msgs)
 time.sleep(20)
else:raise TimeoutError('Peer READY not seen')
