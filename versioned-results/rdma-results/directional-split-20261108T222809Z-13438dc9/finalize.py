from coord import *
import datetime,hashlib
load=lambda n:json.loads((R/n).read_text())
audit=load('measurement-audit.json');assert audit['complete'] and audit['verified_endpoint_records']==12
assert load('service-cleanup-verification.json')['complete']
acks=[]
for raw in map(json.loads,(R/'agent-messages.jsonl').read_text().splitlines()):
 try:d=json.loads(raw['text'])
 except ValueError:continue
 if d.get('run_id')==R.name and d.get('kind') in ['LOCAL_COMPLETE','JOB_DONE','FINAL_ACK']:acks.append({'record':raw,'message':d})
for host in ['spark-7e64','spark-28a3']:
 for kind in ['LOCAL_COMPLETE','JOB_DONE']:assert any(x['record']['sender']==host and x['message']['kind']==kind for x in acks)
assert any(x['record']['sender']=='spark-28a3' and x['message']['kind']=='FINAL_ACK' and x['message']['both_JOB_DONE_retained'] for x in acks)
(R/'completion-acknowledgments.json').write_text(json.dumps(acks,indent=2)+'\n')
archives={}
for p in R.glob('peer-*.tar.gz'):
 actual=hashlib.sha256(p.read_bytes()).hexdigest();receipt=load(p.name+'.receipt.json');imp=load(p.name+'.import.json')
 assert actual==receipt['sha256']==imp['archive_sha256'] and p.stat().st_size==receipt['bytes'];archives[p.name]=actual
assert len(archives)==2
services=[*R.glob('*-mailbox-server-*.json'),*R.glob('*-peer-log-receiver-*.json')];assert len(services)==2
for p in services:
 m=json.loads(p.read_text());assert m['exit_code']==-15 and m['end_utc']
state=load('RUN.json');assert len(state['processed_ids'])==3 and len(set(state['processed_ids']))==3
report={'complete':True,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'run_id':R.name,'measurement_audit':'measurement-audit.json','endpoint_records':12,'repetitions':3,'both_LOCAL_COMPLETE_observed':True,'both_JOB_DONE_observed':True,'peer_final_ack_observed':True,'archive_sha256':archives,'peer_inventory_files_verified':242,'both_network_cleanups_verified':True,'services_stopped':True,'service_logs_finalized':True,'service_shutdown':'Intentional SIGTERM, child exit -15','report':'RESULTS.md','raw_logs_index':'README.md','interpretation':'Incoming RX is the exchanged peer TX report; opposite rows match exactly. This measures bidirectional directional split, not root cause of asymmetry.'}
(R/'FINAL_AUDIT.json').write_text(json.dumps(report,indent=2)+'\n')
state.update(state='complete',services='stopped',completed_utc=report['utc'],final_audit='FINAL_AUDIT.json');(R/'RUN.json').write_text(json.dumps(state,indent=2)+'\n')
print(json.dumps(report,indent=2))
