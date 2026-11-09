import datetime, json, pathlib, socket, subprocess, sys, uuid
root=pathlib.Path(__file__).resolve().parent
label=sys.argv[1]
cmd=sys.argv[2:]
stem=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+socket.gethostname()+'-'+label+'-'+uuid.uuid4().hex[:6]
meta={'command':cmd,'hostname':socket.gethostname(),'start_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
with (root/(stem+'.stdout')).open('xb') as out, (root/(stem+'.stderr')).open('xb') as err:
    result=subprocess.run(cmd,stdout=out,stderr=err)
meta.update(exit_code=result.returncode,end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
(root/(stem+'.json')).write_text(json.dumps(meta,indent=2)+'\n')
print(stem, 'exit_code=',result.returncode)
print((root/(stem+'.stdout')).read_text())
print((root/(stem+'.stderr')).read_text())
sys.exit(result.returncode)
