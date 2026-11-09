import csv, json, pathlib, re, statistics
ROOT=pathlib.Path(__file__).resolve().parent
rows=[]
for path in ROOT.glob('matrix-v1-*-spark-7e64-*.json'):
 try: meta=json.loads(path.read_text())
 except ValueError: continue
 if not isinstance(meta,dict) or 'case' not in meta: continue
 case=meta['case']; out=path.with_suffix('.stdout')
 text=out.read_text() if out.exists() else ''
 data=[line.split() for line in text.splitlines() if re.match(r'^\s*\d+\s+\d+\s+',line)]
 row={k:case[k] for k in ('id','kind','size','direction','rep')}
 row.update(path=path.stem.rsplit('-p',1)[-1],role=meta['role'],exit_code=meta.get('exit_code','running'),log=out.name)
 if data:
  values=data[-1]
  if case['kind']=='lat':
   for key,value in zip(('bytes','iterations','min_us','max_us','typical_us','average_us','stdev_us','p99_us','p999_us'),values): row[key]=value
  elif len(values)>=5:
   row.update(iterations=values[1],average_gbps=values[3],million_messages_sec=values[4])
 rows.append(row)
rows.sort(key=lambda r:(r['id'],r['path']))
fields=['id','kind','path','size','direction','rep','role','exit_code','iterations','average_gbps','million_messages_sec','min_us','max_us','typical_us','average_us','stdev_us','p99_us','p999_us','log']
with (ROOT/'matrix-results.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=fields,extrasaction='ignore');w.writeheader();w.writerows(rows)
print(json.dumps({'completed_logs':sum(r['exit_code']==0 for r in rows),'running_logs':sum(r['exit_code']=='running' for r in rows),'failed_logs':[r['id'] for r in rows if r['exit_code'] not in (0,'running')],'latest':rows[-3:]},indent=2))
