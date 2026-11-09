import json, pathlib, subprocess, sys, time
ROOT=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import benchmark_runner as b

def logged(label,cmd):
 result=subprocess.run([sys.executable,str(ROOT/'run_logged.py'),label]+cmd)
 if result.returncode: raise RuntimeError(label+' failed with '+str(result.returncode))
try:
 deadline=time.monotonic()+5400
 while time.monotonic()<deadline:
  completed=set()
  for item in b.mailbox()['messages']:
   try: msg=json.loads(item['text'])
   except (TypeError,ValueError): continue
   if msg.get('run')!='matrix-v1-first':continue
   if msg.get('kind')=='ERROR':raise RuntimeError('First phase failed: '+str(msg))
   if msg.get('kind')=='MATRIX_COMPLETE':completed.add(item['sender'])
  if completed=={'spark-7e64','spark-28a3'}:break
  time.sleep(15)
 else:raise TimeoutError('First-phase completion timeout')
 logged('phase2-route',['ip','route','get','192.168.201.13'])
 logged('phase2-ping',['ping','-I','enP2p1s0f1np1','-c','3','-W','2','192.168.201.13'])
 for dev in ('enp1s0f1np1','enP2p1s0f1np1'):
  logged('phase2-baseline-'+dev,['ethtool','-S',dev])
 print(b.mailbox('START_REMAINING: both first-phase MATRIX_COMPLETE received. Primary second-path route/ping passed and phase2 counter baseline saved. Please launch reviewed benchmark_runner.py --remaining, capturing output/metadata. This runs74 cases with RUN matrix-v1-remaining, starts smoke001 on path2 (your server port14003), then path2/concurrent BW, path2 latency, 300s concurrent bidirectional stability. PREPARED digest barrier will gate startup. Preserve no-probe/no-upload rule during measurements.'))
 logged('remaining-matrix',[sys.executable,'-u',str(ROOT/'benchmark_runner.py'),'--remaining'])
 print(b.mailbox('COORDINATOR_MATRIX_FINISHED: remaining runner exited0. Await both MATRIX_COMPLETE. Capture final counters, then prepare raw log archive with SHA256 upload; do not delete temporary second-path addresses until log transfer and any targeted diagnostics are agreed.'))
except BaseException as exc:
 try: print(b.mailbox('HANDOFF_ERROR: '+repr(exc)))
 finally:raise
