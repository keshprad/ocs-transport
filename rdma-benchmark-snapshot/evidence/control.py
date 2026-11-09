import pathlib,datetime,socket,json,subprocess,uuid,sys,urllib.request,urllib.error
ROOT=pathlib.Path(__file__).resolve().parent
HOST=socket.gethostname()
def stamp():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def directory(kind):
 p=ROOT/(kind+'-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'-'+uuid.uuid4().hex[:8]);p.mkdir();return p
def mailbox(text=None):
 p=directory('mailbox');method='GET' if text is None else 'POST';body=None if text is None else json.dumps({'sender':HOST,'text':text}).encode()
 headers={'Authorization':'Bearer '+(ROOT/'agent-mailbox-key.txt').read_text().strip(),'Content-Type':'application/json'}
 req={'method':method,'url':'http://169.254.135.137:12080/messages','headers':headers,'body':None if body is None else body.decode(),'hostname':HOST,'start_utc':stamp()}
 (p/'request.json').write_text(json.dumps(req,indent=2))
 try:
  with urllib.request.urlopen(urllib.request.Request(req['url'],headers=headers,data=body,method=method),timeout=15) as r:raw=r.read();meta={'status':r.status,'headers':list(r.headers.items()),'exit_code':0}
 except urllib.error.HTTPError as e:raw=e.read();meta={'status':e.code,'exit_code':1}
 except Exception as e:raw=b'';meta={'error':repr(e),'exit_code':1}
 (p/'response.body').write_bytes(raw);meta['end_utc']=stamp();(p/'response.json').write_text(json.dumps(meta,indent=2));print('LOG',p,meta,flush=True)
 if meta['exit_code']:sys.exit(1)
 data=json.loads(raw)
 if text is None:
  state=ROOT/'mailbox-seen.json';seen=set(json.loads(state.read_text())) if state.exists() else set();fresh=[]
  for m in data.get('messages',[]):
   key=json.dumps(m,sort_keys=True)
   if key not in seen:fresh.append(m);seen.add(key)
  state.write_text(json.dumps(sorted(seen)))
  for m in fresh:
   if m.get('sender')==HOST:continue
   try:
    protocol=json.loads(m['text'])
    if isinstance(protocol,dict) and protocol.get('kind')=='DONE':
     print(m['utc'],protocol.get('run'),protocol.get('test'),'DONE',protocol.get('exit_codes'),flush=True);continue
   except (ValueError,TypeError):pass
   print(json.dumps(m),flush=True)
 else:print(raw.decode(),flush=True)
def run(label,cmd):
 p=directory(label);meta={'command':cmd,'invocation':['/bin/bash','-c',cmd],'hostname':HOST,'cwd':str(pathlib.Path.cwd()),'start_utc':stamp()};(p/'command.json').write_text(json.dumps(meta,indent=2));print('LOG',p,flush=True)
 with (p/'stdout').open('xb') as out,(p/'stderr').open('xb') as err:
  proc=subprocess.Popen(['/bin/bash','-c',cmd],stdout=out,stderr=err);print('PID',proc.pid,flush=True);code=proc.wait()
 meta.update(end_utc=stamp(),exit_code=code);(p/'completion.json').write_text(json.dumps(meta,indent=2));print('COMPLETE',code,flush=True);print((p/'stdout').read_text(errors='replace')[-12000:]);print('STDERR',(p/'stderr').read_text(errors='replace')[-4000:])
if __name__=='__main__':
 if sys.argv[1]=='get':mailbox()
 elif sys.argv[1]=='post':mailbox(sys.argv[2])
 elif sys.argv[1]=='run':run(sys.argv[2],sys.argv[3])
