import time,control,pathlib,datetime
end=time.monotonic()+6600
while time.monotonic()<end:
 try:control.mailbox()
 except SystemExit:print('MONITOR_NETWORK_FAILURE',flush=True)
 files=list(control.ROOT.glob('matrix-v1-*-spark-28a3-*.json.completion.json'))
 print('LOCAL_COMPLETED_PATH_JOBS',len(files),datetime.datetime.now(datetime.timezone.utc).isoformat(),flush=True)
 time.sleep(20)
