import json
import pathlib
import sys
import urllib.request
root = pathlib.Path(__file__).resolve().parent
key = (root / 'agent-mailbox-key.txt').read_text().strip()
body = json.dumps({'sender': sys.argv[1], 'text': sys.argv[2]}).encode() if len(sys.argv) == 3 else None
req = urllib.request.Request('http://169.254.135.137:12080/messages', data=body, headers={'Authorization': 'Bearer '+key, 'Content-Type': 'application/json'})
with urllib.request.urlopen(req, timeout=10) as response:
    print(response.read().decode())
