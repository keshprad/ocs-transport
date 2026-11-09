from coord import *
for ip,iface in [('169.254.238.243','enp1s0f1np1'),('192.168.201.13','enP2p1s0f1np1')]:
 logged('route-'+iface,['ip','-j','route','get',ip]); data=json.loads(subprocess.check_output(['ip','-j','route','get',ip])); assert data[0]['dev']==iface
 logged('ping-'+iface,['ping','-I',iface,'-c','3','-W','2',ip])
a=json.loads(subprocess.check_output(['ip','-j','address']))
assert all(next(x for x in a if x['ifname']==i)['mtu']==1500 for i in ['enp1s0f1np1','enP2p1s0f1np1'])
print('Routes and MTUs verified')
