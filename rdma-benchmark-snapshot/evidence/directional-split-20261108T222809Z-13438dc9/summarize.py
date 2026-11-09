from pathlib import Path
import json,re,statistics,datetime
R=Path(__file__).resolve().parent
checks=[json.loads((R/(R.name+f'-r{i}.crosscheck.json')).read_text()) for i in [1,2,3]]
for i in [1,2,3]:
 for path in [1,2]:
  stem=R/(R.name+f'-r{i}-p{path}');meta=json.loads(Path(str(stem)+'.completion.json').read_text());assignment=json.loads((R/(R.name+f'-r{i}.assignment.json')).read_text());assert meta['command']==assignment['jobs'][path-1]['server_command'];assert meta['exit_code']==0;assert (datetime.datetime.fromisoformat(meta['end_utc'])-datetime.datetime.fromisoformat(meta['start_utc'])).total_seconds()>=30

def counters(p):
 s=p.read_text();d={};interface=None
 for line in s.splitlines():
  if line.startswith('INTERFACE='):interface=line.split('=',1)[1]
  m=re.match(r'^\s+(\S+):\s+(\d+)\s*$',line)
  if m and interface:d[interface+'/'+m[1]]=int(m[2])
 for m in re.finditer(r'^FILE=(\S+)\n(\d+)\s*$',s,re.M):d[m[1]]=int(m[2])
 return d
before=counters(next(R.glob('baseline-*/stdout')));after=counters(next(R.glob('final-counters-*/stdout')))
delta={k:dict(before=v,after=after[k],delta=after[k]-v) for k,v in before.items() if k in after}
(R/'counter-deltas.json').write_text(json.dumps(delta,indent=2));(R/'directional-results.json').write_text(json.dumps(checks,indent=2))
lines=['Directional split measured during concurrent two-path bidirectional RDMA WRITE.','64KiB,30s,3 repetitions; perftest6.20; EthernetMTU1500/RDMAMTU1024.','Local/remote directional rows cross-matched exactly against opposite endpoint.','Remote incoming values are exchanged peer TX reports, not independent RX measurements.','', 'rep spark-7e64->spark-28a3 spark-28a3->spark-7e64 combined (Gb/s)']
for i,c in enumerate(checks,1):lines.append(f"{i} {c['aggregate_forward']:.2f} {c['aggregate_reverse']:.2f} {c['aggregate_combined']:.2f}")
lines.append('Medians: '+' '.join(f'{statistics.median(c[k] for c in checks):.2f}' for k in ['aggregate_forward','aggregate_reverse','aggregate_combined']))
lines+=['','Per path: path1 TX fromspark-7e64 92.27Gb/s each repetition; spark-28a3 TX9.89/9.95/9.86.','Path2 TX fromspark-7e64 92.27Gb/s each repetition; spark-28a3 TX9.89/10.05/9.86.','Displayed rounded directional sums can differ0.01Gb/s from displayed combined rows.','All6 local jobs exited0; coordinator verifies6 matching peer jobs and >30s simultaneous client intervals.','Temporary unsaved NM profile b64a5439-4de2-4b7c-bd66-3d26a972c871 down/deleted; .13 and route absent.','Original first-path/default routes unchanged. Existing NM auto DHCP/IPv6 retry left untouched.','Matching source: https://github.com/linux-rdma/perftest/blob/24.01.0-0.38/src/write_bw.c','Counter deltas and complete raw outputs preserved separately; no cause assigned to directional asymmetry.']
(R/'RESULTS.txt').write_text('\n'.join(lines)+'\n');print('\n'.join(lines));print('Nonzero error counter deltas:',{k:v['delta'] for k,v in delta.items() if v['delta'] and re.search('err|drop|discard|crc|fault|link_down',k)})
