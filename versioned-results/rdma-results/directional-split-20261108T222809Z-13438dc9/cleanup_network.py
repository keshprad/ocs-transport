from coord import *
import datetime
state=json.loads((R/'RUN.json').read_text());uid=state['temporary_profile_uuid']
assert uid=='2799a935-8f8a-4864-9dce-ae1006b5245a'
logged('cleanup-profile-down',['nmcli','connection','down','uuid',uid])
logged('cleanup-profile-delete',['nmcli','connection','delete','uuid',uid])
for lab,cmd in [('cleanup-addresses',['ip','-j','address']),('cleanup-routes',['ip','-j','route']),('cleanup-profiles',['nmcli','-g','UUID','connection','show']),('cleanup-cm',['rdma','resource','show','cm_id'])]: logged(lab,cmd)
a=json.loads(next(R.glob('*-cleanup-addresses-*.stdout')).read_text());routes=json.loads(next(R.glob('*-cleanup-routes-*.stdout')).read_text());profiles=next(R.glob('*-cleanup-profiles-*.stdout')).read_text()
assert uid not in profiles and not any(x.get('local')=='192.168.201.12' for i in a for x in i.get('addr_info',[]))
assert not any(x.get('dst')=='192.168.201.0/24' for x in routes)
assert any(x.get('dst')=='default' and x.get('gateway')=='10.48.0.1' and x.get('dev')=='wlP9s9' for x in routes)
assert any(i['ifname']=='enp1s0f1np1' and any(x.get('local')=='169.254.135.137' for x in i.get('addr_info',[])) for i in a)
assert all(next(x for x in a if x['ifname']==i)['mtu']==1500 for i in ['enp1s0f1np1','enP2p1s0f1np1'])
remaining=[]
for p in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
 try:argv=p.read_bytes().decode().rstrip('\0').split('\0')
 except (OSError,UnicodeError):continue
 if pathlib.Path(argv[0]).name=='ib_write_bw' and any(str(port) in argv for port in [18211,18212,18221,18222,18231,18232]):remaining.append(argv)
assert not remaining
result={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'removed_profile_uuid':uid,'temporary_address_route_absent':True,'original_default_first_path_intact':True,'mtu_preserved':True,'remaining_benchmark_processes':remaining,'complete':True}
(R/'local-cleanup-verification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
