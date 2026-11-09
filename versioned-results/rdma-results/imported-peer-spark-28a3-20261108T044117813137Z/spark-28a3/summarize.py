from pathlib import Path
import re,json,statistics,collections,csv,datetime
root=Path(__file__).resolve().parent
records=[];bycase=collections.defaultdict(list)
for p in sorted(root.glob('matrix-v1-*-spark-28a3-*.json.completion.json')):
 x=json.loads(p.read_text());initial=str(p).removesuffix('.completion.json');stdout=Path(initial.removesuffix('.json')+'.stdout')
 rows=[l.split() for l in stdout.read_text().splitlines() if re.match(r'^\s*'+str(x['case']['size'])+r'\s+\d+\s',l)]
 assert x['exit_code']==0 and len(rows)==1,(p,x['exit_code'],rows)
 x['numeric_row']=rows[0];x['output_log']=str(stdout);records.append(x);bycase[x['case']['id']].append(x)
assert len(records)==147 and len(bycase)==119,(len(records),len(bycase))
for group in bycase.values():assert len(group)==len(group[0]['case']['paths'])
now=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ');stem=root/('local-summary-'+now)
Path(str(stem)+'.json').write_text(json.dumps(records,indent=2))
groups=collections.defaultdict(list)
for test,rs in bycase.items():
 c=rs[0]['case'];value=sum(float(r['numeric_row'][3]) for r in rs) if c['kind']!='lat' else float(rs[0]['numeric_row'][5])
 groups[(c['kind'],','.join(str(p+1) for p in c['paths']),c['size'],c['direction'])].append(value)
lines=['Local RDMA matrix verification: spark-28a3','119 cases, 147 endpoint jobs, all exit0, each has one numeric result row.','Bandwidth is perftest reported average Gb/s; concurrent values sum both path results.','Forward = spark-7e64 client writing spark-28a3. Reverse = spark-28a3 client writing spark-7e64.','Bidirectional values are perftest reported combined rates; not per-direction rates.','Latency is perftest average usec from ping-pong, grouped by client placement; not independently measured one-way direction.','Latency uses100000 iterations, one path at a time without concurrent BW.','Single300s stability run is aggregate throughput/exit-code stability; no data-integrity validation flag was used.','All measurements retain Ethernet MTU1500, RDMA MTU1024, perftest6.20, host memory, default CPU affinity and perftest defaults.','', 'kind paths size direction repetitions min median max']
for key,vs in sorted(groups.items()):lines.append(' '.join(map(str,key))+f' {len(vs)} {min(vs):.3f} {statistics.median(vs):.3f} {max(vs):.3f}')
Path(str(stem)+'.txt').write_text('\n'.join(lines)+'\n')
# Baseline/final ethtool ordered snapshots are both p1 then p2.
baseline=next(root.glob('baseline-*/stdout')).read_text().split('NIC statistics:')[1:]
final=next(root.glob('final-counters-*/stdout')).read_text().split('NIC statistics:')[1:]
deltas={}
for interface,a,b in zip(['enp1s0f1np1','enP2p1s0f1np1'],baseline,final):
 def parse(s):return {m[1]:int(m[2]) for m in re.finditer(r'^\s+(\S+):\s+(\d+)\s*$',s,re.M)}
 before,after=parse(a),parse(b);deltas[interface]={k:dict(before=v,after=after[k],delta=after[k]-v) for k,v in before.items() if k in after}
Path(str(stem)+'.counter-deltas.json').write_text(json.dumps(deltas,indent=2))
print('SUMMARY',str(stem)+'.txt');print('\n'.join(lines))
for i,d in deltas.items():
 print('COUNTER_NONZERO_ERROR_DELTAS',i,{k:v for k,v in d.items() if v['delta'] and re.search('err|drop|discard|crc|fault|link_down',k)})
