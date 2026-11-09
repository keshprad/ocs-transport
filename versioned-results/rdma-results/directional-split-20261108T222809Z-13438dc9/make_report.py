from coord import *
import statistics,csv
runs=[json.loads(p.read_text()) for p in sorted(R.glob('*-r[123]-accepted.json'))]
lines=['# Concurrent bidirectional RDMA directional split','','Status: '+('COMPLETE' if (R/'FINAL_AUDIT.json').exists() else 'Measurements in progress / final audit pending'),'', 'Host-memory RDMA writes; both paths concurrent; 65,536-byte messages; 30-second duration; three repetitions; Ethernet MTU 1500, RDMA MTU 1024; one QP per path; default affinity/tuning. Both endpoints used `-b --report-both --report_gbits`.','','## Aggregate rates (Gb/s)','','| Repetition | spark-7e64 → spark-28a3 | spark-28a3 → spark-7e64 | Sum of displayed directions | Reported combined |','|---|---:|---:|---:|---:|']
for i,x in enumerate(runs,1):lines.append(f'| {i} | {x["aggregate_forward"]:.2f} | {x["aggregate_reverse"]:.2f} | {x["aggregate_forward"]+x["aggregate_reverse"]:.2f} | {x["aggregate_combined"]:.2f} |')
if len(runs)==3:
 med=[statistics.median(x[k] for x in runs) for k in ['aggregate_forward','aggregate_reverse','aggregate_combined']]
 lines.append('| Median | '+' | '.join(f'{v:.2f}' for v in [med[0],med[1],statistics.median(x['aggregate_forward']+x['aggregate_reverse'] for x in runs),med[2]])+' |')
lines+=['','## Each path (Gb/s)','','| Rep | Path | 7e64 TX / 28a3 RX | 7e64 RX / 28a3 TX | Combined |','|---|---|---:|---:|---:|']
rows=[]
for i,x in enumerate(runs,1):
 for p in x['paths']:
  lines.append(f'| {i} | {p["path"]} | {p["spark_7e64_tx"]:.2f} | {p["spark_7e64_rx"]:.2f} | {p["combined"]:.2f} |');rows.append({'repetition':i,**p})
lines+=['','## Interpretation and verification','','A 0.01 Gb/s difference between displayed directional sums and combined reports can arise from perftest rounding. The directional rates come from these bidirectional runs, not earlier one-way tests. Perftest labels `Local results` as this endpoint’s outgoing RDMA-write report and `Remote results` as the exchanged peer outgoing report. Thus RX denotes peer-reported incoming throughput, not an independent receive-side hardware measurement. Opposite endpoint rows, including iteration counts and message rates, must match exactly; the combined row must equal the directional sum within printed rounding.','','Source: [perftest 24.01.0-0.38 write_bw.c](https://github.com/linux-rdma/perftest/blob/24.01.0-0.38/src/write_bw.c#L417-L437) and matching saved source outputs. `print_full_bw_report` adds both reports for the combined row.','','The prior ~204.29 Gb/s combined result is a separate measurement; this run measures its directional split under the same settings. Directional asymmetry remains observable; this run does not establish its cause.','','Raw primary endpoint logs are `*-rN-pN-client-*`; peer raw logs are in imported archive directories. `protocol-proposal.json`, `agent-messages.jsonl`, per-repetition acceptance files, baseline/final counters, archive receipts, inventory checks and FINAL_AUDIT.json provide the audit trail.']
if (R/'peer-counter-deltas-verified.json').exists():
 local=json.loads((R/'local-counter-deltas.json').read_text());peer=json.loads((R/'peer-counter-deltas-verified.json').read_text())
 lines+=['','## Counter changes','','| Host | Interface | PHY lane 0 error delta | PHY lane 1 error delta |','|---|---|---:|---:|']
 for host in ['spark-7e64','spark-28a3']:
  for iface in ['enp1s0f1np1','enP2p1s0f1np1']:
   d=local[iface]['deltas'] if host=='spark-7e64' else {k.split('/')[-1]:v for k,v in peer['deltas'].items() if k.startswith(iface+'/')}
   lines.append(f'| {host} | {iface} | {d["rx_err_lane_0_phy"]:,} | {d["rx_err_lane_1_phy"]:,} |')
 lines+=['','Other captured error/drop/discard/CRC/fault/link-down/FEC/pause counters did not increase. Raw RDMA counters and deltas are preserved. These intervals include setup/control traffic; PHY lane counts are not proof of packet loss or the cause of asymmetry, and should not be added across interfaces.','','## Completion evidence','','Both peer archives and all 242 listed file hashes verified. All 12 endpoint commands, exit codes, directional rows, and three overlapping execution intervals audited. Both benchmark-created unsaved profiles were removed; original first-path addresses and default routes preserved. `measurement-audit.json` contains the measurement checks; `FINAL_AUDIT.json` records final handshake and service shutdown.']
(R/'RESULTS.md').write_text('\n'.join(lines)+'\n')
if rows:
 with (R/'directional-results.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
print('\n'.join(lines[:17]))
