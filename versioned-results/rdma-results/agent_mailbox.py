import datetime
import hmac
import json
import pathlib
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent
KEYFILE = ROOT / 'agent-mailbox-key.txt'
if not KEYFILE.exists():
    KEYFILE.write_text(secrets.token_urlsafe(24))
    KEYFILE.chmod(0o600)
KEY = KEYFILE.read_text().strip()
LOG = ROOT / 'agent-messages.jsonl'
LOCK = threading.Lock()

class Handler(BaseHTTPRequestHandler):
    def reply(self, status, data):
        payload = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def authorized(self):
        if self.path != '/messages':
            self.reply(404, {'error': 'unknown route'})
            return False
        if not hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + KEY):
            self.reply(401, {'error': 'unauthorized'})
            return False
        return True

    def do_GET(self):
        if not self.authorized(): return
        with LOCK:
            messages = [json.loads(line) for line in LOG.read_text().splitlines()] if LOG.exists() else []
        self.reply(200, {'messages': messages})

    def do_POST(self):
        if not self.authorized(): return
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 16384: raise ValueError('invalid size')
            data = json.loads(self.rfile.read(length))
            if data.get('sender') not in ('spark-7e64', 'spark-28a3'): raise ValueError('invalid sender')
            if not isinstance(data.get('text'), str) or not data['text'].strip(): raise ValueError('missing text')
        except (ValueError, TypeError, AttributeError):
            self.reply(400, {'error': 'expected sender and text, maximum 16384 bytes'})
            return
        with LOCK:
            record = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'sender': data['sender'], 'text': data['text']}
            with LOG.open('a') as f: f.write(json.dumps(record) + '\n')
        self.reply(201, record)

server = ThreadingHTTPServer(('169.254.135.137', 12080), Handler)
server.timeout = 1
print('Mailbox listening on 169.254.135.137:12080 for two hours', flush=True)
threading.Timer(7200, server.shutdown).start()
try:
    server.serve_forever()
finally:
    server.server_close()
