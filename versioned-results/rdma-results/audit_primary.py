import datetime, json, pathlib, re, sys
ROOT=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import benchmark_runner as b
requirements=[]
measurements=[]
for case in b.cases():
 phase='first' if case['paths']==[0] else 'remaining'
 for path in case['paths']:
  stem=f"matrix-v1-{phase}-spark-7e64-{case['id']}-p{path+1}"
  meta_file=ROOT/(stem+'.json'); out_file=ROOT/(stem+'.stdout'); err_file=ROOT/(stem+'.stderr')
  issues=[]
  meta=json.loads(meta_file.read_text()) if meta_file.exists() else {}
  for f in (meta_file,out_file,err_file):
   if not f.exists(): issues.append('missing '+f.name)
  if meta:
   if meta.get('exit_code')!=0:issues.append('not successfully completed')
   if meta.get('case')!=case:issues.append('case differs from manifest')
   if meta.get('hostname')!='spark-7e64':issues.append('wrong host')
   server_host='spark-7e64' if case['direction']=='reverse' else 'spark-28a3'
   if meta.get('role')!=('server' if server_host=='spark-7e64' else 'client'):issues.append('wrong role')
   expected=['timeout',str(max(180,case['duration']+120)),'ib_write_lat' if case['kind']=='lat' else 'ib_write_bw','-d',b.DEVS[path],'-i','1','-R','-p',str(14000+int(case['id'].split('-')[0])*2+path),'-s',str(case['size'])]
   expected+=['-n','100000'] if case['kind']=='lat' else ['-D',str(case['duration']),'--report_gbits']
   if case['direction']=='bidir':expected+=['-b']
   if server_host!='spark-7e64':expected+=[b.IPS['spark-28a3'][path]]
   if meta.get('command')!=expected:issues.append('command differs from requested test')
   if not meta.get('start_utc') or not meta.get('end_utc'):issues.append('missing timestamps')
   elif case['duration']:
    elapsed=(datetime.datetime.fromisoformat(meta['end_utc'])-datetime.datetime.fromisoformat(meta['start_utc'])).total_seconds()
    if elapsed<case['duration']:issues.append('elapsed shorter than requested duration')
  text=out_file.read_text() if out_file.exists() else ''
  device=re.search(r'Device\s*:\s*(\S+)',text)
  if not device or device[1]!=b.DEVS[path]:issues.append('output device differs from requested path')
  data=[line.split() for line in text.splitlines() if re.match(r'^\s*\d+\s+\d+\s+',line)]
  if not data:issues.append('missing numeric result')
  elif int(data[-1][0])!=case['size']:issues.append('wrong message size')
  elif case['kind']=='lat' and int(data[-1][1])!=100000:issues.append('wrong latency iteration count')
  row={'id':case['id'],'path':path+1,'issues':issues,'metadata':meta_file.name}
  requirements.append(row)
  if not issues:measurements.append(row)
report={'expected_cases':len(b.cases()),'expected_endpoint_logs':len(requirements),'verified_endpoint_logs':len(measurements),'complete':len(measurements)==len(requirements),'checks':requirements}
(ROOT/'primary-completion-audit.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='checks'},indent=2))
