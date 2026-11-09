import pathlib,json,subprocess,sys
R=pathlib.Path(__file__).resolve().parent
def logged(label,cmd):
 p=subprocess.run(['python3',str(R/'run_logged.py'),label,*cmd],capture_output=True,text=True)
 print(p.stdout.splitlines()[0] if p.stdout else '',flush=True)
 if p.returncode: print(p.stdout,p.stderr);raise RuntimeError(label)
def post(kind,**kw):
 logged('post-'+kind,['python3',str(R/'agent_mailbox_client.py'),'spark-7e64',json.dumps({'run_id':R.name,'kind':kind,**kw})])
if __name__=='__main__':post(sys.argv[1],**json.loads(sys.argv[2]))
