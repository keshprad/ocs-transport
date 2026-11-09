import control,json,sys
R=control.ROOT;rep=int(sys.argv[1]);test=f'{R.name}-r{rep}';items=[]
for f in R.glob('mailbox-*/response.body'):
 try:items+=json.loads(f.read_text()).get('messages',[])
 except (ValueError,AttributeError):pass
matches=[]
for m in items:
 if m['sender']!='spark-7e64':continue
 try:x=json.loads(m['text'])
 except ValueError:continue
 if x.get('kind')=='ASSIGNMENT' and x.get('test_id')==test:matches.append(x)
assert matches
assignment=matches[-1]
for path,dev in enumerate(['rocep1s0f1','roceP2p1s0f1'],1):
 expected=['timeout','180','ib_write_bw','-d',dev,'-i','1','-R','-p',str(18200+rep*10+path),'-s','65536','-D','30','-m','1024','-b','--report-both','--report_gbits']
 assert assignment['jobs'][path-1]['server_command']==expected
with (R/(test+'.assignment.json')).open('x') as f:json.dump(assignment,f,indent=2)
control.mailbox(json.dumps(dict(run_id=R.name,kind='ACK',test_id=test,jobs=assignment['jobs'],exact_commands_agreed=True)))
