import control,json,pathlib,subprocess,time,sys,shlex
R=control.ROOT;rep=int(sys.argv[1]);run=R.name;test=f'{run}-r{rep}'
# Assignments must be supplied and reviewed before invoking this helper.
with (R/(test+'.started')).open('x') as f:f.write(control.stamp())
def post(kind,**kw):control.mailbox(json.dumps(dict(run_id=run,kind=kind,test_id=test,**kw)))
jobs=[]
for path,dev in enumerate(['rocep1s0f1','roceP2p1s0f1'],1):
 port=18200+rep*10+path;cmd=['timeout','180','ib_write_bw','-d',dev,'-i','1','-R','-p',str(port),'-s','65536','-D','30','-m','1024','-b','--report-both','--report_gbits'];stem=R/(test+f'-p{path}')
 meta=dict(command=cmd,exact_invocation=shlex.join(cmd),hostname=control.HOST,start_utc=control.stamp(),test_id=test,path=path,role='server',port=port)
 with pathlib.Path(str(stem)+'.command.json').open('x') as f:json.dump(meta,f,indent=2)
 out=pathlib.Path(str(stem)+'.stdout').open('xb');err=pathlib.Path(str(stem)+'.stderr').open('xb');proc=subprocess.Popen(cmd,stdout=out,stderr=err);jobs.append((proc,out,err,meta,stem))
ready=False
for attempt in range(30):
 result=subprocess.run(['rdma','resource','show','cm_id'],capture_output=True,text=True)
 with (R/(test+f'-listener-{attempt}.json')).open('x') as f:json.dump(dict(command=['rdma','resource','show','cm_id'],hostname=control.HOST,timestamp=control.stamp(),stdout=result.stdout,stderr=result.stderr,exit_code=result.returncode),f)
 if all(any('state LISTEN' in l and f':{j[3]["port"]} ' in l for l in result.stdout.splitlines()) for j in jobs):ready=True;break
 if any(j[0].poll() is not None for j in jobs):break
 time.sleep(.5)
if ready:post('READY',ports=[j[3]['port'] for j in jobs],logs=[str(j[4]) for j in jobs])
else:
 for j in jobs:
  if j[0].poll() is None:j[0].terminate()
last=time.monotonic()
while any(j[0].poll() is None for j in jobs):
 if time.monotonic()-last>=20:control.mailbox();last=time.monotonic()
 time.sleep(.5)
results=[]
for p,o,e,m,stem in jobs:
 code=p.wait();o.close();e.close();m.update(exit_code=code,end_utc=control.stamp())
 with pathlib.Path(str(stem)+'.completion.json').open('x') as f:json.dump(m,f,indent=2)
 results.append(dict(path=m['path'],exit_code=code,log=str(stem),stdout=pathlib.Path(str(stem)+'.stdout').read_text(),stderr=pathlib.Path(str(stem)+'.stderr').read_text()))
post('RESULTS',results=results);post('DONE',exit_codes=[x['exit_code'] for x in results],results=results)
print(json.dumps(results,indent=2),flush=True)
if not ready or any(x['exit_code'] for x in results):sys.exit(1)
