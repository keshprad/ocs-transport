import json, os, pathlib, subprocess, sys, time
ROOT=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import benchmark_runner as b
UUID='f0cbbd7c-f1b6-4285-9ff1-f397645e6b7d'
def logged(label,cmd,allowed=(0,)):
 result=subprocess.run([sys.executable,str(ROOT/'run_logged.py'),label]+cmd)
 if result.returncode not in allowed:raise RuntimeError(label+' exit '+str(result.returncode))
 return result.returncode
try:
 deadline=time.monotonic()+3600
 while time.monotonic()<deadline:
  done=set()
  for item in b.mailbox()['messages']:
   try:msg=json.loads(item['text'])
   except (ValueError,TypeError):continue
   if msg.get('run')!='matrix-v1-remaining':continue
   if msg.get('kind')=='ERROR':raise RuntimeError('Matrix error '+str(msg))
   if msg.get('kind')=='MATRIX_COMPLETE':done.add(item['sender'])
  if done=={'spark-7e64','spark-28a3'}:break
  time.sleep(15)
 else:raise TimeoutError('Remaining phase completion wait expired')
 logged('postrun-primary-audit',[sys.executable,str(ROOT/'audit_primary.py')])
 if not json.loads((ROOT/'primary-completion-audit.json').read_text())['complete']:raise RuntimeError('Primary audit not complete')
 for dev in ('enp1s0f1np1','enP2p1s0f1np1'):
  logged('final-counters-'+dev,['ethtool','-S',dev])
 logged('counter-comparison',[sys.executable,str(ROOT/'compare_counters.py')])
 for name in ('ib_write_bw','ib_write_lat'):
  code=logged('postrun-process-'+name,['pgrep','-u',str(os.getuid()),'-x',name],(0,1))
  if code==0:raise RuntimeError('Perftest process still active; inspect before cleanup')
 logged('cleanup-profile-down',['nmcli','--wait','20','connection','down','uuid',UUID])
 logged('cleanup-profile-delete',['nmcli','connection','delete','uuid',UUID])
 logged('cleanup-network-state',['bash','-c','ip -br address; ip route show; nmcli -f NAME,UUID,DEVICE connection show --active; nmcli -g UUID connection show; rdma resource show cm_id'])
 addr=json.loads(subprocess.check_output(['ip','-j','address','show','dev','enP2p1s0f1np1']))
 profiles=subprocess.check_output(['nmcli','-g','UUID','connection','show'],text=True)
 if any(a.get('local')=='192.168.201.12' for i in addr for a in i.get('addr_info',[])) or UUID in profiles:raise RuntimeError('Temporary config remains')
 (ROOT/'local-postrun-complete.json').write_text(json.dumps({'matrix_verified':True,'final_counters_captured':True,'temporary_profile_removed':UUID,'temporary_address_absent':True,'utc':b.utc()},indent=2)+'\n')
 print(b.mailbox('PRIMARY_POSTRUN_READY: all147 local endpoint records audited, both final counter snapshots saved, no local perftest processes remain, temporary NM profile down/deleted and .12 absent. Please follow POSTRUN_SEQUENCE: final counters, your profile cleanup, archive and integrity-checked upload. This explicitly supersedes any earlier generic handoff text suggesting to defer cleanup. Primary LOCAL_COMPLETE waits for consolidated peer archive audit.'))
except BaseException as exc:
 try:print(b.mailbox('PRIMARY_POSTRUN_ERROR: '+repr(exc)))
 finally:raise
