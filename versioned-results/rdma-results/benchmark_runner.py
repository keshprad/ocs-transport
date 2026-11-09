import concurrent.futures, datetime, hashlib, json, pathlib, socket, subprocess, sys, time, urllib.request
ROOT=pathlib.Path(__file__).resolve().parent
HOST=socket.gethostname()
PEER='spark-28a3' if HOST=='spark-7e64' else 'spark-7e64'
KEY=(ROOT/'agent-mailbox-key.txt').read_text().strip()
PHASE='first' if '--first' in sys.argv else ('remaining' if '--remaining' in sys.argv else 'all')
RUN='matrix-v1-'+PHASE
DEVS=['rocep1s0f1','roceP2p1s0f1']
IPS={'spark-7e64':['169.254.135.137','192.168.201.12'],'spark-28a3':['169.254.238.243','192.168.201.13']}

def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def mailbox(text=None):
    data=json.dumps({'sender':HOST,'text':text}).encode() if text else None
    req=urllib.request.Request('http://169.254.135.137:12080/messages',data=data,headers={'Authorization':'Bearer '+KEY,'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=15) as response: return json.load(response)
def post(kind, test, **kw):
    msg=json.dumps(dict(run=RUN,kind=kind,test=test,**kw))
    result=mailbox(msg)
    with (ROOT/(RUN+'-'+HOST+'-protocol.jsonl')).open('a') as f: f.write(json.dumps(result)+'\n')
    print(utc(),kind,test,kw,flush=True)
def wait(kind,test,limit=180):
    end=time.monotonic()+limit
    while time.monotonic()<end:
        for item in mailbox()['messages']:
            if item['sender']!=PEER: continue
            try: msg=json.loads(item['text'])
            except (ValueError,TypeError): continue
            if msg.get('run')!=RUN: continue
            if msg.get('kind')=='ERROR': raise RuntimeError('Peer error: '+str(msg))
            if msg.get('kind')==kind and msg.get('test')==test: return msg
        time.sleep(2)
    raise TimeoutError('Peer '+kind+' '+test)
def cases():
    result=[]
    def add(kind,paths,size,direction,rep,duration):
        result.append(dict(id=f'{len(result)+1:03d}-{kind}-'+''.join(str(p+1) for p in paths)+f'-{size}-{direction}-r{rep}',kind=kind,paths=paths,size=size,direction=direction,rep=rep,duration=duration))
    add('smoke',[1],65536,'forward',1,5)
    for paths in ([0],[1],[0,1]):
        for size in (4096,65536,1048576):
            for direction in ('forward','reverse','bidir'):
                for rep in (1,2,3): add('bw',paths,size,direction,rep,30)
    for path in (0,1):
        for size in (8,64,4096):
            for direction in ('forward','reverse'):
                for rep in (1,2,3): add('lat',[path],size,direction,rep,0)
    add('stability',[0,1],65536,'bidir',1,300)
    return result
MANIFEST=[c for c in cases() if PHASE=='all' or (c['paths']==[0] if PHASE=='first' else c['paths']!=[0])]

def start(case,path,server):
    port=14000+int(case['id'].split('-')[0])*2+path
    cmd=['ib_write_lat' if case['kind']=='lat' else 'ib_write_bw','-d',DEVS[path],'-i','1','-R','-p',str(port),'-s',str(case['size'])]
    if case['kind']=='lat': cmd+=['-n','100000']
    else: cmd+=['-D',str(case['duration']),'--report_gbits']
    if case['direction']=='bidir': cmd+=['-b']
    if not server: cmd+=[IPS[PEER][path]]
    cmd=['timeout',str(max(180,case['duration']+120))]+cmd
    stem=ROOT/(RUN+'-'+HOST+'-'+case['id']+f'-p{path+1}')
    meta=dict(command=cmd,hostname=HOST,start_utc=utc(),case=case,role='server' if server else 'client',port=port)
    meta_path=pathlib.Path(str(stem)+'.json')
    with meta_path.open('x') as f: json.dump(meta,f,indent=2)
    out=pathlib.Path(str(stem)+'.stdout').open('xb'); err=pathlib.Path(str(stem)+'.stderr').open('xb')
    p=subprocess.Popen(cmd,stdout=out,stderr=err)
    return (p,out,err,meta,meta_path)
def finish(job):
    p,out,err,meta,path=job
    code=p.wait(); out.close(); err.close()
    meta.update(exit_code=code,end_utc=utc()); path.write_text(json.dumps(meta,indent=2)+'\n')
    return code

def run():
    assert HOST in IPS
    manifest_path=ROOT/(RUN+'-'+HOST+'-manifest.json')
    manifest_path.write_text(json.dumps(MANIFEST,indent=2)+'\n')
    digest=hashlib.sha256(json.dumps(MANIFEST,sort_keys=True).encode()).hexdigest()
    post('PREPARED','all',manifest_sha256=digest,cases=len(MANIFEST))
    other=wait('PREPARED','all',600)
    if other['manifest_sha256']!=digest: raise RuntimeError('Manifest mismatch')
    for case in MANIFEST:
        test=case['id']; jobs=[]
        server_host='spark-7e64' if case['direction']=='reverse' else 'spark-28a3'
        server=HOST==server_host
        try:
            if server:
                jobs=[start(case,path,True) for path in case['paths']]
                deadline=time.monotonic()+20
                while True:
                    check=subprocess.run(['rdma','resource','show','cm_id'],capture_output=True,text=True)
                    with (ROOT/(RUN+'-'+HOST+'-'+test+'-readiness.jsonl')).open('a') as f:
                        f.write(json.dumps(dict(command=['rdma','resource','show','cm_id'],utc=utc(),hostname=HOST,stdout=check.stdout,stderr=check.stderr,exit_code=check.returncode))+'\n')
                    if all(':'+str(j[3]['port'])+' ' in check.stdout and any('state LISTEN' in line and ':'+str(j[3]['port'])+' ' in line for line in check.stdout.splitlines()) for j in jobs): break
                    if any(j[0].poll() is not None for j in jobs) or time.monotonic()>deadline: raise RuntimeError('Server failed readiness')
                    time.sleep(0.5)
                post('READY',test,server=HOST,ports=[j[3]['port'] for j in jobs])
            else:
                wait('READY',test,180)
                jobs=[start(case,path,False) for path in case['paths']]
            codes=[finish(j) for j in jobs]
            if any(codes): raise RuntimeError('Nonzero benchmark exits '+str(codes))
            post('DONE',test,exit_codes=codes)
            wait('DONE',test,max(180,case['duration']+120))
        except BaseException:
            for job in jobs:
                if job[0].poll() is None: job[0].terminate()
                finish(job)
            raise
    post('MATRIX_COMPLETE','all',cases=len(MANIFEST))

if __name__=='__main__':
    if '--manifest' in sys.argv:
        print(json.dumps(MANIFEST,indent=2))
    else:
        try: run()
        except BaseException as exc:
            try: post('ERROR','runner',error=repr(exc))
            finally: raise
