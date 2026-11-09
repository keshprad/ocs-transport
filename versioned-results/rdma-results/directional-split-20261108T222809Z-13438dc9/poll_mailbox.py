import pathlib,subprocess,time,datetime
r=pathlib.Path(__file__).resolve().parent
for i in range(90):
 subprocess.run(['python3',str(r/'run_logged.py'),'mailbox-poll', 'python3',str(r/'agent_mailbox_client.py')])
 time.sleep(20)
