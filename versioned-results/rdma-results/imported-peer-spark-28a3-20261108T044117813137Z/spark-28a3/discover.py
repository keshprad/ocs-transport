import subprocess, socket, datetime, pathlib, json, uuid
root=pathlib.Path(__file__).resolve().parent
run=root/('discovery-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')+'-'+uuid.uuid4().hex[:8]); run.mkdir()
commands=[
'hostname', 'hostnamectl', 'cat /etc/os-release', 'uname -a',
'ip -details -statistics link show', 'ip -details address show', 'ip route show table all', 'ip -6 route show table all', 'ip neighbor show',
'rdma link show', 'rdma dev show', 'ibdev2netdev', 'ibstat', 'ibv_devinfo -v',
'for d in /sys/class/infiniband/*; do echo "DEVICE $d"; readlink -f "$d/device"; ls -l "$d/device/net"; for p in "$d"/ports/*; do echo "PORT $p"; for f in state phys_state rate link_layer active_mtu lid; do echo "FILE $p/$f"; cat "$p/$f"; done; for f in "$p"/gid_attrs/ndevs/* "$p"/gid_attrs/types/* "$p"/gids/* "$p"/counters/* "$p"/hw_counters/*; do echo "FILE $f"; cat "$f"; done; done; done',
'for n in /sys/class/net/*; do i=${n##*/}; echo "INTERFACE $i"; ethtool "$i"; ethtool -i "$i"; ethtool -S "$i"; done',
'command -v ib_write_bw ib_read_bw ib_send_bw ib_write_lat ib_read_lat ib_send_lat raw_ethernet_bw raw_ethernet_lat',
'dpkg-query -W perftest rdma-core ibverbs-utils infiniband-diags iproute2 ethtool',
'for t in ib_write_bw ib_read_bw ib_send_bw ib_write_lat ib_read_lat ib_send_lat; do echo "TOOL $t"; "$t" --version; done',
'ib_write_bw --help', 'ib_read_bw --help', 'ib_send_lat --help',
'lspci -nnk', 'lsmod', 'cat /proc/net/dev', 'ip -s -s link show',
]
for i,cmd in enumerate(commands,1):
    stem=f'{i:03d}'; start=datetime.datetime.now(datetime.timezone.utc).isoformat()
    meta={'invocation':['/bin/bash','-c',cmd], 'command':cmd,'hostname':socket.gethostname(),'cwd':str(pathlib.Path.cwd()),'start_utc':start}
    with (run/(stem+'.stdout')).open('xb') as out, (run/(stem+'.stderr')).open('xb') as err:
        p=subprocess.Popen(['/bin/bash','-c',cmd],stdout=out,stderr=err)
        try: code=p.wait(timeout=45)
        except subprocess.TimeoutExpired: p.kill(); code=p.wait(); meta['timeout_seconds']=45
    meta.update(exit_code=code,end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (run/(stem+'.json')).write_text(json.dumps(meta,indent=2)+'\n')
    print(stem,code,cmd,flush=True)
print('LOG_DIRECTORY',run)
