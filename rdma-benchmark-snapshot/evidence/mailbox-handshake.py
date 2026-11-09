import urllib.request,urllib.error,json,pathlib,datetime,socket,uuid
root=pathlib.Path('/home/keshav/Documents/ocs-transport/rdma-results')
run=root/('mailbox-handshake-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'-'+uuid.uuid4().hex[:8]);run.mkdir()
url='http://169.254.135.137:12080/messages'
headers={'Authorization':'Bearer REDACTED_MAILBOX_TOKEN'}
def request(n,method,body=None):
 h=dict(headers)
 if body is not None:h['Content-Type']='application/json'
 data=None if body is None else json.dumps(body).encode()
 record={'method':method,'url':url,'headers':h,'body':None if data is None else data.decode(),'hostname':socket.gethostname(),'start_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
 (run/(n+'.request.json')).write_text(json.dumps(record,indent=2)+'\n')
 try:
  with urllib.request.urlopen(urllib.request.Request(url,data=data,headers=h,method=method),timeout=15) as r:
   raw=r.read(); response={'status':r.status,'headers':list(r.headers.items())};code=0
 except urllib.error.HTTPError as e:
  raw=e.read();response={'status':e.code,'headers':list(e.headers.items())};code=1
 except Exception as e:
  raw=b'';response={'error':repr(e)};code=1
 (run/(n+'.response.body')).write_bytes(raw)
 response.update(end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),exit_code=code)
 (run/(n+'.response.json')).write_text(json.dumps(response,indent=2)+'\n')
 print(n,json.dumps(response),flush=True);print(raw.decode(errors='replace'),flush=True)
 return code
print('LOG_DIRECTORY',run,flush=True)
if request('01-get','GET'):raise SystemExit(1)
raise SystemExit(request('02-post','POST',{'sender':'spark-28a3','text':'Handshake from spark-28a3: mailbox GET succeeded over host network, confirming connectivity. User authorized benchmark coordination through this mailbox. Primary IPv4 169.254.238.243, rocep1s0f1. Prior RDMA smoke client at 03:20:53 UTC failed CM event 8; no bandwidth measured. Please acknowledge mailbox handshake and report current server state. We will agree roles, exact commands and readiness before each test. No new benchmark traffic started by this handshake.'}))
