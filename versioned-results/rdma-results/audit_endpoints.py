import datetime, json, pathlib, re, sys
ROOT=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import benchmark_runner as b
PEER_ROOT=pathlib.Path(sys.argv[1]).resolve() if len(sys.argv)>1 else None
checks=[]; records={}
def dt(s):return datetime.datetime.fromisoformat(s)
for host in ('spark-7e64','spark-28a3'):
 for case in b.cases():
  phase='first' if case['paths']==[0] else 'remaining'
  for path in case['paths']:
   stem=f"matrix-v1-{phase}-{host}-{case['id']}-p{path+1}"
   issues=[]
   if host=='spark-7e64':matches=[ROOT/(stem+'.json')]
   else:matches=list(PEER_ROOT.rglob(stem+'.json.completion.json')) if PEER_ROOT else []
   if len(matches)!=1 or not matches[0].exists():
    checks.append({'host':host,'case':case['id'],'path':path+1,'issues':['missing or ambiguous completion metadata']});continue
   file=matches[0];meta=json.loads(file.read_text())
   out=file.parent/(stem+'.stdout');err=file.parent/(stem+'.stderr')
   for f in (out,err):
    if not f.exists():issues.append('missing '+f.name)
   if host=='spark-28a3':
    initial=file.parent/(stem+'.json')
    if not initial.exists():issues.append('missing initial metadata')
    else:
     orig=json.loads(initial.read_text())
     if any(meta.get(k)!=v for k,v in orig.items()):issues.append('initial/completion metadata conflict')
   server='spark-7e64' if case['direction']=='reverse' else 'spark-28a3'
   peer='spark-28a3' if host=='spark-7e64' else 'spark-7e64'
   role='server' if host==server else 'client'
   cmd=['timeout',str(max(180,case['duration']+120)),'ib_write_lat' if case['kind']=='lat' else 'ib_write_bw','-d',b.DEVS[path],'-i','1','-R','-p',str(14000+int(case['id'].split('-')[0])*2+path),'-s',str(case['size'])]
   cmd+=['-n','100000'] if case['kind']=='lat' else ['-D',str(case['duration']),'--report_gbits']
   if case['direction']=='bidir':cmd+=['-b']
   if role=='client':cmd+=[b.IPS[peer][path]]
   for key,wanted in [('command',cmd),('case',case),('hostname',host),('role',role),('exit_code',0)]:
    if meta.get(key)!=wanted:issues.append('incorrect '+key)
   try:
    elapsed=(dt(meta['end_utc'])-dt(meta['start_utc'])).total_seconds()
    if elapsed<case['duration']:issues.append('elapsed shorter than duration')
   except (KeyError,ValueError):issues.append('missing/invalid timestamps')
   text=out.read_text() if out.exists() else ''
   device=re.search(r'Device\s*:\s*(\S+)',text)
   if not device or device[1]!=b.DEVS[path]:issues.append('output device differs from requested path')
   numbers=[line.split() for line in text.splitlines() if re.match(r'^\s*\d+\s+\d+\s+',line)]
   if not numbers:issues.append('missing numeric output')
   else:
    if int(numbers[-1][0])!=case['size']:issues.append('wrong output size')
    if case['kind']=='lat' and int(numbers[-1][1])!=100000:issues.append('wrong latency iterations')
   records[(host,case['id'],path)]=meta
   checks.append({'host':host,'case':case['id'],'path':path+1,'metadata':str(file.relative_to(ROOT)),'issues':issues})
overlap=[]
for case in b.cases():
 if len(case['paths'])!=2:continue
 client='spark-28a3' if case['direction']=='reverse' else 'spark-7e64'
 jobs=[records.get((client,case['id'],p)) for p in case['paths']]
 entry={'case':case['id'],'client':client,'issues':[]}
 try:
  starts=[dt(j['start_utc']) for j in jobs];ends=[dt(j['end_utc']) for j in jobs]
  entry.update(start_spread_seconds=(max(starts)-min(starts)).total_seconds(),overlap_seconds=(min(ends)-max(starts)).total_seconds())
  if entry['start_spread_seconds']>2 or entry['overlap_seconds']<case['duration']:entry['issues'].append('concurrent timing insufficient')
 except (TypeError,KeyError,ValueError):entry['issues'].append('missing concurrent timing evidence')
 overlap.append(entry)
report={'expected_cases':119,'expected_endpoint_logs':294,'verified_endpoint_logs':sum(not c['issues'] for c in checks),'verified_concurrent_cases':sum(not c['issues'] for c in overlap),'expected_concurrent_cases':28,'checks':checks,'concurrent_checks':overlap}
report['complete']=report['verified_endpoint_logs']==294 and report['verified_concurrent_cases']==28
(ROOT/'paired-completion-audit.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ('checks','concurrent_checks')},indent=2))
