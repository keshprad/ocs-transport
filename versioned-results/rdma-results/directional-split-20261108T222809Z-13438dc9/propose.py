import json,pathlib,subprocess
r=pathlib.Path(__file__).resolve().parent
run=r.name
cases=[]
for rep in range(1,4):
 jobs=[]
 for path,dev,ip in [(1,'rocep1s0f1','169.254.238.243'),(2,'roceP2p1s0f1','192.168.201.13')]:
  port=18200+rep*10+path
  cmd=['timeout','180','ib_write_bw','-d',dev,'-i','1','-R','-p',str(port),'-s','65536','-D','30','-m','1024','-b','--report-both','--report_gbits']
  jobs.append({'path':path,'port':port,'device':dev,'server_host':'spark-28a3','server_ip':ip,'server_command':cmd,'client_host':'spark-7e64','client_command':cmd+[ip]})
 cases.append({'test_id':run+'-r'+str(rep),'jobs':jobs})
p={'run_id':run,'kind':'PROTOCOL_PROPOSAL','cases':cases,'settings':{'ethernet_mtu':1500,'rdma_mtu':1024,'memory':'host','affinity':'default','tuning':'unchanged','concurrent_paths':True},'protocol':'First HANDSHAKE and PROTOCOL_ACK confirming exact manifest. Prepare unsaved path2 profile, record UUID, verify both routes/MTUs and capture baseline counters. Coordinator sends ASSIGNMENT per repetition; peer ACKs exact test ID/commands then starts both servers and posts READY with both verified CM listeners. Coordinator starts both clients concurrently only after READY. Both send RESULTS/DONE including exits and raw local/remote output rows. Advance only after both DONE and endpoint cross-check. Persist processed IDs; never rerun a completed ID. Failures get unique retry IDs by agreement. Capture final counters, remove only created profile, consolidate archive+file hashes, LOCAL_COMPLETE then JOB_DONE, observe both before finishing. Poll20s HTTP timeout10s. No traffic until agreement.', 'report_semantics':'Installed help says --report-both reports RX/TX separately. Binary labels Local results / Remote results. Source verification pending; do not assume label mapping until confirmed. Preserve entire output including combined row.'}
(r/'protocol-proposal.json').write_text(json.dumps(p,indent=2)+'\n')
subprocess.run(['python3',str(r/'run_logged.py'),'protocol-proposal','python3',str(r/'agent_mailbox_client.py'),'spark-7e64',json.dumps(p)],check=True)
