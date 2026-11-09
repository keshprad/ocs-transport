import base64, hashlib, pathlib, sys, zlib
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import benchmark_runner as b
raw=(b.ROOT/'benchmark_runner.py').read_bytes()
text='RUNNER_UPDATE: Review this deterministic Python runner before executing. Save decoded zlib/base64 as rdma-results/benchmark_runner.py; key must be rdma-results/agent-mailbox-key.txt (existing shared token). Manifest has 119 cases: second-path smoke, 81 bandwidth cases, 36 latency cases, one 300s concurrent bidirectional stability case. PREPARED digest agreement; per-test READY/DONE; bounded failures; all outputs/metadata saved. Use --first for 45 first-path cases while sudo config is pending; --remaining later for the other 74 cases. Do not launch until START_FIRST. Script SHA256='+hashlib.sha256(raw).hexdigest()+' ZLIB_BASE64='+base64.b64encode(zlib.compress(raw)).decode()
print(b.mailbox(text))
