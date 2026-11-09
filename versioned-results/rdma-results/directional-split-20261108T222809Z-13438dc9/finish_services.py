import pathlib,os,signal,time,subprocess,json,datetime
root=pathlib.Path(__file__).resolve().parent
scripts={str(pathlib.Path('rdma-results')/root.name/n) for n in ['agent_mailbox.py','receive_peer_logs.py','poll_mailbox.py']}
def scan():
 result=[]
 for p in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
  try: a=p.read_bytes().decode().rstrip('\0').split('\0')
  except (OSError,UnicodeError): continue
  if len(a) in (2,3) and pathlib.Path(a[0]).name.startswith('python') and a[-1] in scripts: result.append((int(p.parent.name),a))
 return result
stopped=scan()
for pid,a in stopped:
 print('Stopping',pid,a,flush=True); os.kill(pid,signal.SIGTERM)
for _ in range(50):
 if not scan(): break
 time.sleep(.1)
remaining=scan()
r=subprocess.run(['ss','-ltn'],capture_output=True,text=True);print(r.stdout);print(r.stderr)
assert r.returncode==0
listeners=[x for x in r.stdout.splitlines() if any(x.split()[3].endswith(':'+str(port)) for port in (12080,12081))] if r.stdout else []
bench=[]
for p in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
 try: a=p.read_bytes().decode().rstrip('\0').split('\0')
 except (OSError,UnicodeError): continue
 if a and pathlib.Path(a[0]).name in ('ib_write_bw','ib_write_lat'): bench.append(a)
d={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'stopped':stopped,'remaining_services':remaining,'remaining_listeners':listeners,'remaining_benchmarks':bench,'complete':not(remaining or listeners or bench)}
(root/'service-cleanup-verification.json').write_text(json.dumps(d,indent=2)+'\n');print(json.dumps(d,indent=2));assert d['complete']
