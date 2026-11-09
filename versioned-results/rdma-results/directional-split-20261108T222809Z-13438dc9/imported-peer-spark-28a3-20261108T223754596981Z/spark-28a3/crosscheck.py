import control,json,sys,re
R=control.ROOT;rep=int(sys.argv[1]);test=f'{R.name}-r{rep}';messages={}
for f in R.glob('mailbox-*/response.body'):
 try:data=json.loads(f.read_text())
 except ValueError:continue
 for m in data.get('messages',[]):
  if m['sender']!='spark-7e64':continue
  try:x=json.loads(m['text'])
  except ValueError:continue
  if x.get('kind')=='RESULTS_DONE' and x.get('test_id')==test:messages[m['utc']]=x
assert messages,'Await peer results'
peer=messages[sorted(messages)[-1]];assert peer['exit_codes']==[0,0];paths=[]
for path in [1,2]:
 stem=R/(test+f'-p{path}');m=json.loads(pathlib_text:=__import__('pathlib').Path(str(stem)+'.completion.json').read_text());assert m['exit_code']==0
 text=__import__('pathlib').Path(str(stem)+'.stdout').read_text();rows=[l.split() for l in text.splitlines() if re.match(r'^\s*65536\s+\d+',l)];assert len(rows)==3
 values=[float(r[3]) for r in rows];p=peer['results'][path-1]['rows'];assert values[1]==p[2]['gbps'] and values[2]==p[1]['gbps'] and values[0]==p[0]['gbps'];assert abs(values[0]-values[1]-values[2])<.021
 paths.append(dict(path=path,spark28_tx=values[1],spark7_tx=values[2],combined=values[0]))
result=dict(run_id=R.name,kind='CROSSCHECK_ACK',test_id=test,passed=True,paths=paths,aggregate_forward=sum(p['spark7_tx'] for p in paths),aggregate_reverse=sum(p['spark28_tx'] for p in paths),aggregate_combined=sum(p['combined'] for p in paths))
with (R/(test+'.crosscheck.json')).open('x') as f:json.dump(result,f,indent=2)
control.mailbox(json.dumps(result));print(json.dumps(result,indent=2))
