import json,pathlib,datetime,hashlib
r=pathlib.Path(__file__).resolve().parent
load=lambda n:json.loads((r/n).read_text())
checks={}
checks['matrix_coverage']=load('manifest-requirements-audit.json')['complete']
p=load('paired-completion-audit.json')
checks['paired_endpoints_and_concurrency']=p['complete'] and p['verified_endpoint_logs']==294 and p['verified_concurrent_cases']==28
v=load('peer-inventory-verification.json')
checks['peer_file_integrity']=sum(x['files'] for x in v)==3568 and all(not x['mismatches'] for x in v)
archives={'peer-spark-28a3-20261108T044117813137Z.tar.gz':'ed583af7108c8bcf88e7fb721a05a36fc2b52526eab70ed594154f5c9b3e9c02','peer-spark-28a3-20261108T044157394028Z.tar.gz':'9d2fa034dbf9bc290b1f4e77b19e3600c2a6288c8397d6ef00c9caa721cca775'}
checks['archive_integrity']=all(hashlib.sha256((r/n).read_bytes()).hexdigest()==h for n,h in archives.items())
checks['counter_comparisons']=(r/'primary-counter-comparison.json').is_file() and all(not x['claim_mismatches'] for x in load('peer-counter-comparison-verified.json').values())
l=load('local-postrun-complete.json'); pc=load('peer-cleanup-verification.json')
checks['local_network_cleanup']=l['temporary_address_absent'] and l['temporary_profile_removed']=='f0cbbd7c-f1b6-4285-9ff1-f397645e6b7d' and load('20261108T044200Z-spark-7e64-local-cleanup-verification-513435.json')['exit_code']==0
checks['peer_network_cleanup']=pc['temporary_profile_and_address_absent'] and pc['original_first_path_and_default_intact']
checks['services_and_benchmarks_stopped']=load('service-cleanup-verification.json')['complete']
acks=load('completion-acknowledgments.json')
checks['both_agents_acknowledged']=all(acks[k][h] for k in ['LOCAL_COMPLETE','JOB_DONE'] for h in ['spark-7e64','spark-28a3'])
service_logs=[*r.glob('*agent-mailbox-server*.json'),*r.glob('*peer-log-receiver*.json')]
checks['service_logs_finalized']=len(service_logs)==2 and all(load(p.name)['exit_code']==-15 and load(p.name)['end_utc'] for p in service_logs)
assert all(checks.values()),checks
d={'complete':True,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'requirements':{k:bool(v) for k,v in checks.items()},'cases':119,'endpoint_records':294,'concurrent_cases':28,'verified_peer_files':3568,'archive_sha256':archives,'service_shutdown_note':'Intentional SIGTERM, exit -15; raw service logs finalized.','limitations':['Directional performance asymmetry remains unexplained.','PHY lane counters increased; causality and packet-loss implications are unproven.'],'evidence_index':'README.md'}
(r/'FINAL_AUDIT.json').write_text(json.dumps(d,indent=2)+'\n')
s=load('RUN_STATE.json');s.update(goal='complete; full matrix, integrity audit, acknowledgments and cleanup verified',sessions={},completed_utc=d['utc'],final_audit='FINAL_AUDIT.json',temporary_services='stopped');(r/'RUN_STATE.json').write_text(json.dumps(s,indent=2)+'\n')
print(json.dumps(d,indent=2))
