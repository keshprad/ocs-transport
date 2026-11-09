import collections, csv, datetime, json, pathlib, statistics, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parent
subprocess.run([sys.executable,str(ROOT/'summarize_matrix.py')],check=True,capture_output=True)
rows=list(csv.DictReader((ROOT/'matrix-results.csv').open()))
complete=[r for r in rows if r['exit_code']=='0']
bycase=collections.defaultdict(list)
for row in complete:bycase[row['id']].append(row)
groups=collections.defaultdict(list)
latgroups=collections.defaultdict(list)
for test,parts in bycase.items():
 first=parts[0]; mode=test.split('-')[2]
 if first['kind'] in ('bw','stability'):
  expected=2 if mode=='12' else 1
  if len(parts)==expected and all(p['average_gbps'] for p in parts):
   groups[(first['kind'],mode,int(first['size']),first['direction'])].append(sum(float(p['average_gbps']) for p in parts))
 elif first['kind']=='lat' and first['typical_us']:
  latgroups[(mode,int(first['size']),first['direction'])].append(float(first['typical_us']))
final_audit=json.loads((ROOT/'FINAL_AUDIT.json').read_text()) if (ROOT/'FINAL_AUDIT.json').exists() else {}
status='Status: COMPLETE — both endpoint audits, log integrity, cleanup, and both JOB_DONE acknowledgments verified.' if final_audit.get('complete') else 'Status: IN PROGRESS until both endpoint audits, log integrity, and cleanup are verified.'
lines=['# RDMA benchmark results','', status,'',
'For bandwidth, forward means spark-7e64 writes to spark-28a3; reverse means spark-28a3 writes to spark-7e64. Latency direction labels identify client placement: spark-7e64 for forward, spark-28a3 for reverse; latency is measured through a ping-pong exchange. Bidirectional bandwidth is the combined throughput of both directions. Concurrent values sum the two path results from the same overlapping test case, not sequential measurements.','',
'Host-memory RDMA, one QP per path, Ethernet MTU 1500, active RDMA MTU 1024. Bandwidth cases request 30 seconds; stability requests 300 seconds. Latency uses 100,000 iterations. Raw command metadata and stdout/stderr are authoritative.','',
'## Bandwidth','', '| Test | Paths | Bytes | Direction | Runs | Median Gb/s | Min–max Gb/s |','|---|---|---:|---|---:|---:|---:|']
for (kind,mode,size,direction),values in sorted(groups.items()):
 lines.append(f'| {kind} | {mode if mode != "12" else "1 + 2"} | {size} | {direction} | {len(values)} | {statistics.median(values):.2f} | {min(values):.2f}–{max(values):.2f} |')
lines+=['','## Latency','','The table reports the median across repetitions of perftest’s reported typical latency. This is a half-round-trip estimate; raw outputs retain minimum, maximum, average, deviation, and percentile columns.','','| Path | Bytes | Direction | Runs | Median typical µs | Min–max typical µs |','|---|---:|---|---:|---:|---:|']
for (mode,size,direction),values in sorted(latgroups.items()):
 lines.append(f'| {mode} | {size} | {direction} | {len(values)} | {statistics.median(values):.3f} | {min(values):.3f}–{max(values):.3f} |')
lines+=['','## Evidence','','- `matrix-results.csv`: individual primary endpoint results.','- `primary-completion-audit.json`: required cases and verification status.','- `matrix-v1-*.json`, `.stdout`, `.stderr`: exact benchmark commands and raw output.','- `agent-messages.jsonl`: coordination and readiness history.','- Discovery, baseline, diagnostic, and final-counter logs remain alongside the benchmark logs.','']
if (ROOT/'FINAL_NOTES.md').exists():lines+=['', (ROOT/'FINAL_NOTES.md').read_text()]
(ROOT/'RESULTS.md').write_text('\n'.join(lines))
print(f'Updated RESULTS.md from {len(complete)} completed endpoint records.')
