from coord import *
import re
out={}
for iface in ['enp1s0f1np1','enP2p1s0f1np1']:
 files=[next(R.glob('*-'+phase+'-'+iface+'-counters-*.stdout')) for phase in ['baseline','final']]
 vals=[{m[1]:int(m[2]) for m in re.finditer(r'^\s+(\S+):\s+(\d+)\s*$',p.read_text(),re.M)} for p in files]
 assert vals[0].keys()==vals[1].keys();delta={k:vals[1][k]-v for k,v in vals[0].items()}
 out[iface]={'sources':[p.name for p in files],'deltas':delta,'nonzero_error_related':{k:v for k,v in delta.items() if v and any(t in k for t in ['err','drop','discard','crc','fault','link_down','fec','pause'])},'decreased':[k for k,v in delta.items() if v<0]}
files=[next(R.glob('*-'+phase+'-rdma-sysfs-*.stdout')) for phase in ['baseline','final']];vals=[json.loads(p.read_text()) for p in files]
assert vals[0].keys()==vals[1].keys();out['rdma']={'sources':[p.name for p in files],'deltas':{k:int(vals[1][k])-int(v) for k,v in vals[0].items()}}
(R/'local-counter-deltas.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps({k:{kk:vv for kk,vv in v.items() if kk!='deltas'} for k,v in out.items()},indent=2))
