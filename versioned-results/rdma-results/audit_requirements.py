import itertools,json,pathlib,sys
ROOT=pathlib.Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import benchmark_runner as b
actual=b.cases()
def key(c):return (c['kind'],tuple(c['paths']),c['size'],c['direction'],c['rep'],c['duration'])
expected=set()
for paths,size,direction,rep in itertools.product(((0,),(1,),(0,1)),(4096,65536,1048576),('forward','reverse','bidir'),(1,2,3)):
 expected.add(('bw',paths,size,direction,rep,30))
for path,size,direction,rep in itertools.product((0,1),(8,64,4096),('forward','reverse'),(1,2,3)):
 expected.add(('lat',(path,),size,direction,rep,0))
expected.add(('smoke',(1,),65536,'forward',1,5))
expected.add(('stability',(0,1),65536,'bidir',1,300))
actual_set={key(c) for c in actual}
report={'required_bandwidth_cases':81,'required_latency_cases':36,'required_second_path_smoke_cases':1,'required_stability_cases':1,'actual_cases':len(actual),'unique_cases':len(actual_set),'missing':sorted(expected-actual_set),'unexpected':sorted(actual_set-expected),'unique_ids':len({c['id'] for c in actual})}
report['complete']=len(actual)==len(actual_set)==report['unique_ids']==119 and not report['missing'] and not report['unexpected']
(ROOT/'manifest-requirements-audit.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
