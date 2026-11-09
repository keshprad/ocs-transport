import datetime, hashlib, hmac, json, pathlib, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
ROOT=pathlib.Path(__file__).resolve().parent
KEY=(ROOT/'agent-mailbox-key.txt').read_text().strip()
class Handler(BaseHTTPRequestHandler):
 def do_POST(self):
  if self.path!='/upload' or not hmac.compare_digest(self.headers.get('Authorization',''),'Bearer '+KEY):
   self.send_error(403);return
  try: length=int(self.headers.get('Content-Length','0'))
  except ValueError: self.send_error(400);return
  if not 0<length<512*1024*1024: self.send_error(413);return
  stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
  path=ROOT/('peer-spark-28a3-'+stamp+'.tar.gz')
  digest=hashlib.sha256();remaining=length
  with path.open('xb') as f:
   while remaining:
    block=self.rfile.read(min(1048576,remaining))
    if not block: raise ConnectionError('Incomplete upload')
    f.write(block);digest.update(block);remaining-=len(block)
  result={'file':path.name,'bytes':length,'sha256':digest.hexdigest(),'utc':stamp}
  (ROOT/(path.name+'.receipt.json')).write_text(json.dumps(result,indent=2)+'\n')
  body=json.dumps(result).encode();self.send_response(201);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
server=ThreadingHTTPServer(('169.254.135.137',12081),Handler)
print('Peer log receiver listening on169.254.135.137:12081 for2h',flush=True)
threading.Timer(7200,server.shutdown).start()
try: server.serve_forever()
finally: server.server_close()
