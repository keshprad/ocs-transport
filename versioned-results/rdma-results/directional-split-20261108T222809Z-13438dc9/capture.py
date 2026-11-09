from coord import *
phase=sys.argv[1]
commands=[('addresses',['ip','-j','address']),('routes',['ip','-j','route']),('rdma-links',['rdma','link','show']),('rdma-devices',['ibv_devinfo']),('profiles',['nmcli','-f','NAME,UUID,DEVICE','connection','show','--active'])]
for iface in ['enp1s0f1np1','enP2p1s0f1np1']:
 commands +=[(iface+'-link',['ethtool',iface]),(iface+'-counters',['ethtool','-S',iface]),(iface+'-ipstats',['ip','-s','-j','link','show','dev',iface])]
commands += [('rdma-sysfs',['python3','-c',"import pathlib,json; d={};\nfor dev in ['rocep1s0f1','roceP2p1s0f1']:\n for folder in ['counters','hw_counters']:\n  for p in sorted((pathlib.Path('/sys/class/infiniband')/dev/'ports/1'/folder).glob('*')):\n   try:d[str(p)]=p.read_text().strip()\n   except OSError as e:d[str(p)]=str(e)\nprint(json.dumps(d,indent=2))"])]
for label,cmd in commands:logged(phase+'-'+label,cmd)
