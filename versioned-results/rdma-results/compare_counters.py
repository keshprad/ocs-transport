import json,pathlib,re
ROOT=pathlib.Path(__file__).resolve().parent
baseline_file=ROOT/'20261108T033032Z-spark-7e64-preflight-3c28e2.stdout'
sections=baseline_file.read_text().split('NIC statistics:')[1:]
def parse(text):
 return {m[1]:int(m[2]) for m in re.finditer(r'^\s+(\S+):\s+(\d+)\s*$',text,re.M)}
result={}
for dev,section in zip(('enp1s0f1np1','enP2p1s0f1np1'),sections):
 candidates=sorted(ROOT.glob('*-final-counters-'+dev+'-*.stdout'))
 if not candidates:
  result[dev]={'status':'awaiting final snapshot','baseline':baseline_file.name};continue
 final_file=candidates[-1];before=parse(section);after=parse(final_file.read_text())
 deltas={key:after[key]-value for key,value in before.items() if key in after}
 interesting={key:delta for key,delta in deltas.items() if any(x in key for x in ('err','drop','discard','crc','fault','link_down','fec','pause')) and delta!=0}
 result[dev]={'baseline':baseline_file.name,'final':final_file.name,'all_deltas':deltas,'nonzero_error_related_deltas':interesting,'decreasing_counters':[k for k,v in deltas.items() if v<0]}
(ROOT/'primary-counter-comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:{n:v for n,v in d.items() if n!='all_deltas'} for k,d in result.items()},indent=2))
