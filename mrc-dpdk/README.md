# DPDK multipath transport

`mrc_write` is the in-progress OCP MRC 1.0 endpoint. See the
[conformance status and verification](CONFORMANCE.md).

## OCP WRITE benchmark

From this directory, `make` builds `mrc_write`, `mrc_ctl_profile`, and the original prototype.
`make check` runs the portable format and state tests. With the memif PMD
available, build `make tests/mrc_write_ready`, then run
`python3 tests/check_mrc_write.py` to exercise the OCP endpoint without
physical NICs. This test-only build waits up to three seconds for every
configured rail before starting the requester, so packet-indexed fault
injection cannot silently miss a late memif link. It also waits for the
responder to stop polling before the client closes memif, then closes the
server ports; shutdown waits are bounded by the configured drain/idle time.
Pass `--binary ./mrc_write`
to test the production CLI directly with its asynchronous startup.

The arguments after EAL are
`tx|rx BYTES_OR_WRITE_LIST LOCAL_IP PEER_IP PORT PEER_MAC [PORT PEER_MAC ...]`.
Supply 1 to 32 distinct DPDK ports by ID or name, including PCI BDFs.
Both peers list the corresponding rails in the same order and use
matching QP and memory attributes. The per-rail JSON arrays follow that
order. Run `mrc_write` with valid EAL parameters
and no benchmark arguments for the option list. Use `--work-requests N` for
multiple WRs and `--immediate N` to give the first WR an immediate value.
Subsequent immediate values increment modulo 32 bits. The default receiver
posts one receive credit per WRITEIMM. `--max-wimm-inflight N` sets the agreed
credit limit. `--mpr N` sets the maximum PSN range in units of 128
packets. It accepts 1 through 255 and defaults to 8.
To mix sizes and operation types, pass the same comma-separated list on
both peers, such as `4097,0@42,11,2049@7`. An `@VALUE` suffix makes that
entry a WRITEIMM. Use the list by itself in place of the single size,
`--work-requests`, and `--immediate`. Each WRITEIMM consumes one receive
credit and advances the receive queue message sequence.

Use `--pmtu N` to select a RoCE payload size of 256, 512, 1024, 2048,
or 4096 bytes. The default is 1024. Use the same value on both peers
for manual setup. The standalone command configures each DPDK port and
its packet pool for the offered size. A 2048- or 4096-byte PMTU requires
ports and a network path that can carry jumbo frames.

Add `--oob-host IP --oob-port N` on both endpoints to exchange the QP
parameters over TCP. On the receiver, the IP is the local management address
to bind. On the sender, it is the receiver's management address. The peers
exchange QPN, initial PSN, receive limits, P_Key, trim capability, and
registered-memory information before the data transfer. Both peers use
the smaller PMTU offer to segment their work and validate options that
refer to packet counts. TCP setup uses the provider-generated rkey. Use
`--remote-key` with manual setup. The exchange record
is specific to this benchmark. OCP leaves the out-of-band encoding to the
application. To exercise Dynamic MPR, pass `--dynamic-mpr 1` on both peers
with TCP setup enabled. On the receiver, `--mpr-change-at PACKETS` and
`--mpr-change-to UNITS` advertise a new limit after accepting that many
segments. The new limit uses units of 128 packets, like `--mpr`.

For a manually configured connection, `--p-key` selects the local
P_Key. Both peers use the same nonzero partition number in the low
15 bits, and at least one peer sets the full-member bit. TCP negotiation
also pins the peer's offered key. For a public DPDK QP, the local key
comes from the verbs partition table. A nonzero `route.peer_pkey` pins
the peer to a specific compatible key.

On the sender, `--data-dscp N` sets the DSCP for new WRITEs and
reliability probes. `--retry-dscp N` can select a separate DSCP for
retransmissions. The data DSCP defaults to zero and the retry DSCP
defaults to the data DSCP. Responder SACKs, NACKs, and Transport ACKs
use Control DSCP 48.

The receiver recognizes regular trim on DSCP 49 by default. Use
`--trim-dscp N` to change it and `--trim-last-hop-dscp N` to enable a
distinct last-hop class. The latter defaults to zero, which disables it.
In the public API, call `mrc_dpdk_set_trim_last_hop_dscp(qp, N)` on an
attached responder to enable the same classification. Both classes
leave destination memory and receive completions untouched until an
ordinary WRITE arrives.

The sender uses `--deny-path-mask MASK` to exclude configured paths from its
EV profile. Bit 0 selects path 0 and bit 1 selects path 1. For a transient
in-process path-1 blackhole, pair `--drop-path-after N` with
`--restore-path-after M`. Recovery is driven by a correlated reliability
probe reply.

### Independent wire tests

With DPDK's `net_pcap` PMD available, run `python3 tests/check_mrc_pcap.py`.
A Python codec generates IPv4, IPv6, and preserved SRv6 requests with and
without a final SRH. The replay validates data placement, SACKs and transport
ACKs across PSN wrap, iCRC and routing rejection, and the invalid-request
error for an unsupported opcode. The IPv6 cases also check structured
entropy and CE feedback. The trim replays check distinct regular and
last-hop NACKs, the original UDP length, and timestamp reflection.

With the `net_memif` PMD available, build `tests/mrc_memif_relay` and run
`python3 tests/check_mrc_requester.py`. The Python responder verifies the same
four wire modes. It injects packet-buffer and TRIM NACKs in separate runs
and checks alternate-rail retries, payload across PSN wrap, and that a
WRITEIMM credit stays occupied until its transport ACK.

The expanded requirement checks also include `make check-mpr-retry` for
independent dynamic/static window feedback and `make check-endpoint-rtt` for
exact-clock Service Time arithmetic. With the pinned OCP headers,
`make MRC_INCLUDE_DIR=/path/to/ocp/headers check-public-service-time` verifies
public capability settings reach live endpoints. See [CONFORMANCE.md](CONFORMANCE.md)
for the assertion-level mapping and remaining hardware/specification gaps.

### OCP application API

The optional `libmrc-dpdk.so` exposes the OCP context, device query, CQ,
completion channel, and QP interfaces. Supply the upstream OCP headers,
the libibverbs development package, and a DPDK build.

```sh
make MRC_INCLUDE_DIR=/path/to/OCP-Multipath-Reliable-Connection libmrc-dpdk.so
make MRC_INCLUDE_DIR=/path/to/OCP-Multipath-Reliable-Connection check-mrc-abi
make MRC_INCLUDE_DIR=/path/to/OCP-Multipath-Reliable-Connection tests/mrc_abi_application
python3 tests/check_mrc_abi_application.py
```

An application opens a verbs context and PD, creates the MRC QP and CQs,
and configures the QP through `mrc_modify_qp()`. It exchanges connection
parameters and remote memory keys with the peer. Exchange
`MRC_DEVICE_CAP_SVC_TIME` in each direction and set
`MRC_QP_CAP_SVC_TIME_DEST_UNSUPPORTED` when the remote responder cannot
report Service Time. The requester subtracts a negotiated peer duration
from its stored-timestamp RTT sample. The responder estimates its service
duration from the DPDK RX poll to feedback preparation. This estimate
includes shared ingress queue time and uses 128 ns wire units.
With the DPDK extensions in `src/mrc_dpdk.h`, the application binds the
verbs MR registrations, attaches started DPDK ports to the QP, and
drives progress from its polling worker.
The application posts standard WRITE and WRITEIMM work requests and polls
the public CQs. Keep the verbs context, PD, MRs, and DPDK resources live
until their MRC uses have ended.

Use `mrc_dpdk_attach_qp_rails()` with 1 to 32 rail records. The
two-rail helpers `mrc_dpdk_attach_qp()` and `mrc_dpdk_attach_srv6_qp()`
use the same attachment path. Attach a QP in one direction or set
`bidirectional` after both directions are configured and the QP reaches
RTS. QPs in the same process can share their DPDK ports. The progress
calls dispatch inbound packets by QPN
and opcode and serialize I/O on the attached queues. Drive progress on
each attached QP to process the packets dispatched to it. Use the same
rail order for QPs to the same peer so they share node port status.
Each Port Status bit is the zero-based index of its rail in the route.
An endpoint announces the current mask on reachable rails when it changes
and refreshes it about every 250 ms. A peer report restricts path selection
for one second. When it expires, the requester resumes path selection
using its local link and EV state.
If a TX queue declines a WRITE or retransmission, the requester tries
the other eligible rails during the same poll. It considers that queue
again on the next poll.

Set the QP's service level in `ibv_qp_attr.ah_attr.sl` with `IBV_QP_AV`.
Use `mrc_dpdk_set_sl_dscp(context, sl, data_dscp, retry_dscp)` to
configure that level for the MRC context. A requester snapshots its
selected mapping on attachment. Unconfigured levels use DSCP zero
for both kinds of WRITE. Probes use the data DSCP, and SRv6 egress
copies the selected traffic class into the outer IPv6 header.

To use Dynamic MPR, negotiate `MRC_QP_CAP_DYNAMIC_MPR` on both peers.
An attached responder in RTR or RTS can change `attrs.mpr.mpr_dest`
with `mrc_modify_qp(qp, NULL, 0, &attrs, MRC_QP_MPR_DEST)`. The value
must be between 1 and the negotiated maximum, in units of 128 packets.
The responder advertises the change in its next SACKs. The requester
applies it when the SACK advances the cumulative PSN. Query each side
with `mrc_query_qp()` and its directional MPR mask to read the current
limit. The public QP also defaults to 8 units and supports up to 255
in each direction. Attachment sizes reliability and responder message
storage to the configured values.

To attach an SRv6 QP, supply one `mrc_dpdk_srv6_path` entry per rail in
the counted route's `srv6_paths` array. Each rail
uses its own local SID as the outer source and accepted destination, and
the peer SID as the expected outer source on receive. The peer SID is
also the default outer destination on send. To start at a different uSID
container, set `initial_dst`. An optional `srh_segment` supplies one more
128-bit container. Set `segments_left` to 1 when the fabric must promote
that container, or leave it at 0 to carry the segment for telemetry.
The application supplies packed wire-order containers.

For a controller-managed SRv6 route, attach the QP with the same local
and peer SIDs. The requester's selected EV profile supplies the outer
destination and any SRH on each rail. The responder uses its application
route for replies. The inner entropy still identifies the requesting
rail in reliability feedback.

SRv6 attachments accept IPv4 or IPv6 inner GIDs. The receiver validates
the SIDs on preserved encapsulation and accepts already decapsulated
frames after checking the inner peer, P_Key, and QPN. The fabric advances
transit routes before delivery to the MRC endpoint.

Set the agreed path MTU with `IBV_QP_PATH_MTU` when moving the QP to RTR.
The provider accepts `IBV_MTU_256` through `IBV_MTU_4096`. Configure both
DPDK port MTUs and packet pool data room to fit the payload and its wire
headers before attaching the QP. Attachment returns `EMSGSIZE` if the
pool or a port is too small.

The attachment supports up to 16 SGEs per send or receive WR. A WRITEIMM
places its payload at the remote address in the request. Its receive post
supplies only a completion credit and can include a scatter/gather list.
Use `IBV_SEND_SOLICITED` on a WRITEIMM to trigger an armed solicited-only
receive CQ notification.

At QP creation, `send_ops_flags` accepts `IBV_QP_EX_WITH_RDMA_WRITE` and
`IBV_QP_EX_WITH_RDMA_WRITE_WITH_IMM`. Post the corresponding operations
with `mrc_post_send()`. The remaining APIs are tracked in
[conformance](CONFORMANCE.md).

Request up to 4,096 bytes of `max_inline_data` at QP creation and use
`IBV_SEND_INLINE` on WRITE or WRITEIMM. The provider copies all source
SGEs of an accepted inline WR during `mrc_post_send()` and ignores their
lkeys. Those source buffers can be reused when the call returns.
Retransmissions use the captured data.

A QP hint describes the expected sending and receiving peer counts.
Create it with `mrc_create_qp_hint()` and submit it through
`MRC_QP_HINT` in INIT. Multiple QPs in one MRC context can share
an advisory hint. Clear the association in INIT, reset the QP, or
destroy it before destroying a hint it references.

Provider-triggered QP failures and CQ overruns generate
`IBV_EVENT_QP_FATAL` and `IBV_EVENT_CQ_ERR` on the MRC context. Use
`mrc_get_async_event()` to retrieve events and `mrc_ack_async_event()` to
release them. Destruction waits for returned events to be acknowledged.
Use `mrc_dpdk_get_async_fd()` to poll the queue or set nonblocking retrieval
with `O_NONBLOCK`. An active QP that shares a failed CQ enters ERROR when
its next query, post, or progress call observes the overrun.

On Linux with uverbs asynchronous event allocation, the MRC context
also delivers native device-fatal and port events through the same API
and pollable FD. MRC allocates a dedicated event queue on the verbs
command FD. The application retains its original libibverbs event queue.
Kernel support and permission for this additional queue determine whether
native events are available. [Linux uverbs event queues](https://github.com/torvalds/linux/blob/v6.17/drivers/infiniband/core/uverbs_main.c)

### Connectionless endpoint operations

Use `mrc_dpdk_attach_endpoint_rails()` to bind a controller context to
1 to 31 started DPDK ports and a peer. The two-rail
`mrc_dpdk_attach_endpoint()` helper uses the same attachment path.
Supply the local and peer RoCEv2 GIDs,
the peer MAC on each rail, and the packet pool. Use IPv4-mapped GIDs
for IPv4. Optionally supply per-rail SRv6 SIDs and return routes.
The attachment can share the ports with public DPDK QPs.

An administrator can then submit EV probes or Port Status Updates
through `mrc_ctl_ep_batch_send_wait()` without creating a QP. Supply
distinct request IDs and a configured rail number for each request.
An EV probe takes the UDP, STEV, or SRv6 EV from the request. The
request DSCP is selectable and the response uses the Control class.
Port Status requests and replies both use the Control class. The
controller's 32-bit Port Status mask uses one-based rail numbers as bit
positions, leaving bit 0 clear.
The API reports replies in receive order, with the observed ingress
rail and RTT. A timeout returns any replies that arrived before it.

On the responder, call `mrc_dpdk_progress_endpoint()` to poll the
ports when an attached QP is idle or absent. Endpoint request batches
also poll their local ports while waiting. Detach the endpoint before
destroying its context. The application owns the ports and pool.

### Driving the DPDK endpoint from a provider QP

The internal interface in `src/mrc_endpoint.h` lets the application own the
provider QP and poll its completion queues. The packet engine derives the
lengths, segmentation, and immediate sequence from the posted work and
progresses one round per call. The benchmark requester takes a finite batch.
With `continuous_send`, a requester can start with an empty send queue and
admit further batches after earlier work retires, keeping the same packet
and message sequence. A responder configured without a total packet count
can keep receiving work until the application stops it.

Build `make tests/mrc_endpoint_application` and run
`python3 tests/check_mrc_application.py` with the memif PMD. The application
posts a WRITE and two WRITEIMMs to a running QP and polls its own send CQ.
The test checks registered source data, application work IDs, and ordered
immediate values across batch boundaries and PSN wrap.

### Work completions

The endpoint posts send work and WRITEIMM receive credits on a provider QP.
First sends and retries read the posted work's source and remote address.
Each registered send buffer stays pinned until retirement or flush.
A successful Transport ACK retires completed WRITE requests. Ordered
WRITEIMM placement consumes the next posted receive and publishes the
immediate value through the receive completion queue. A terminal wire error
or exhausted retry budget transitions the QP to ERROR. The affected send
gets its error status and outstanding work gets flush completions.
Failure diagnostics report the first work error and the number flushed.

### Receiver resource limits

The receiver normally reserves tracking slots, staging buffers, and work
records for the configured window. Use `--rx-ooo-capacity`,
`--rx-staging-capacity`, and `--rx-work-capacity` to limit these resources
and exercise their reliability NACKs under reordering. Use
`--rx-cq-capacity` to bound the number of pending receive completions.
A completion queue overrun terminates the transfer with `UNEXP_EVENT`.
The result includes `nack_sent_mask` and `nack_received_mask`. Each bit
position corresponds to its Table 7-3 reason code.

### Persistent EV profiles

The OCP controller ABI exposes device capabilities, built-in format queries,
and profile configuration through `mrc_ctl.h`. It accepts 1 to 32 EVs
per profile in AUTO, explicit, and generated modes. Multiple EVs can
share one rail. Select `MRC_DPDK_EV_FORMAT_UDP` for
a UDP source-port EV or `MRC_DPDK_EV_FORMAT_STEV` for an IPv6 flow label and
UDP source-port EV. The first four bytes of a controller EV hold a big-endian
value. The remaining bytes are zero. The one-based port numbers follow
the attached DPDK route's rail order.

Each slot defaults to the rail with the same one-based index. To place
an explicit EV on a different rail, set `ev_new.port` in
`MRC_CTL_EV_OP_REPLACE_EV`. An empty slot is selected by its default
port number and an all-zero value. For AUTO or generated profiles, set
`MRC_CTL_EV_PROFILE_VENDOR_CFG` while configuring the profile OFFLINE.
Use one byte per EV in `vendor_cfg`, with zero selecting the default
rail for that entry, and zero-fill the remainder. Querying the EV array
or vendor configuration returns the effective one-based ports. Include
every mapped port when attaching the QP.

Select `MRC_DPDK_EV_FORMAT_SRV6` for a uSID destination in the first 16
bytes, or `MRC_DPDK_EV_FORMAT_SRV6_SRH` for a uSID destination followed
by a second 16-byte container. Supply packed IPv6 addresses in wire
order and leave unused bytes zero. Both formats support explicit and
generated profiles. Equal containers start with Segments Left 0 for
telemetry. Different containers start with Segments Left 1.

Point the controller and the application at the same directory with
`mrc_dpdk_set_profile_store()`. The controller needs effective `CAP_NET_ADMIN` to
mutate profiles. For separate service accounts, use a controller-owned
directory with mode `02750` and the application group. Profile snapshots
and a lock with that group get mode `0640`. Application processes take
read-only leases. Keep the directory and lock inode stable. The store
reads legacy snapshots and persists any nondefault port assignments
with the EV profile.

Configure the profile OFFLINE, populate the explicit EVs, and set it
ONLINE. In the application, set `MRC_QP_EV_PROFILE_ID` when bringing the
QP to INIT. The application retains the profile until reset or destruction.
The controller can change GOOD and DENIED while the profile is in use.
EV queries report administrative state. The transport tracks failure and
congestion per QP.

For generated mode, use `MRC_CTL_EV_MODE_GEN` with the built-in format
ID, the EV count, and `MRC_CTL_EV_FIELDS_OP_MODIFY_FIELDS` when moving
to OFFLINE. UDP uses one field. STEV uses two fields in flow-label suffix
and UDP source-port order. Each occupies a 16-bit slot, and narrower
fields are zero-padded. In each field, the compact values between
`min_val` and `max_val` overwrite the masked bits of `init_val`.
Use a zero mask and zero bounds for a fixed field.

The provider fills the profile with valid combinations in increasing
compact-value order, varying the UDP field fastest and skipping UDP
source port zero. It persists the field definitions and generated values.
Query fields or the EV array
before setting the profile ONLINE.

Generated SRv6 uses eight or sixteen 16-bit slots in wire order, with
the last field varying fastest. Fields narrower than their slot are
zero-padded on the left. The provider skips unspecified and multicast
destinations and SRH segments.

To define a different field layout, use `mrc_ctl_modify_ev_fmt_profile()`
with a format ID above 4. Move the format to OFFLINE with its UDP, STEV,
or SRv6 mode. Supply the field widths with
`MRC_CTL_EV_FMT_OP_MODIFY_FIELDS`, then move the format ONLINE. Bind that
format ID when configuring an explicit or generated EV profile.

Custom formats accept up to 16 fields, each 1 to 32 bits wide. The
fields are concatenated in wire order and must total 16 bits for UDP,
32 for STEV, or 128 or 256 for SRv6. In a 256-bit layout, the two
containers have a field boundary between them. Generated fields narrower
than their format slot are zero-padded on the left. Omitted trailing slots
are zero. Explicit values use the same wire representation as the
corresponding built-in mode.

An ONLINE EV profile keeps its format ONLINE. Before editing a custom
format, take its dependent EV profiles OFFLINE. After changing the
layout, rebind the format ID and configure the EV values again before
returning those profiles ONLINE.

Select `MRC_CTL_EV_ASSUMED_BAD`, `MRC_CTL_EV_GOOD`, or both in
`ev_event_mask` while the EV profile is OFFLINE or being moved ONLINE.
With the same profile store as the application, a controller can create
an event CQ with `mrc_ctl_create_ev_event_cq()`. Call
`mrc_ctl_query_device_ex()` with the controller's MRC context before
creating the CQ to receive the event state with the 1.0.1 ABI. Poll with
`mrc_ctl_poll_ev_event()`, or arm a completion channel and acknowledge
its CQ notifications as usual.

Each profile store supports one event CQ. Creating it requires
`CAP_NET_ADMIN` and write access to the store directory. In a setgid
shared store, the controller's event socket allows applications in that
group to publish events. Notifications carry the profile ID, rail port,
and exact EV. When the CQ fills, it keeps the newest events and marks the
next result with `drop_count=1`. The publisher also flags socket
congestion when it can send its next event.

The benchmark CLI uses the same storage. Create a private directory on the
sender and bring a profile online.

```sh
mkdir -m 700 ./ev-profiles
./mrc_ctl_profile ./ev-profiles create 42
./mrc_ctl_profile ./ev-profiles configure 42 explicit 1 0xe499 0xe49a
./mrc_ctl_profile ./ev-profiles online 42
./mrc_ctl_profile ./ev-profiles show 42
```

Pass `--profile-dir ./ev-profiles --profile-id 42` to `mrc_write tx`.
The CLI's numeric profile creation assigns distinct EVs in rail order. For IPv4,
an EV is a nonzero UDP source port. For IPv6, its upper 16 bits give the
lower 16 bits of the flow label and its lower 16 bits give the UDP source port.
`configure ID auto 2 MIN_ACTIVE` uses the benchmark's default EVs.
`generated` accepts values already expanded by the caller, in the same
form as `explicit`. The minimum active setting limits automatic failure
demotion. Administrative denials still apply.

To use an SRv6 profile created with the OCP controller API, also pass the
benchmark's `--srv6-local` and `--srv6-peer` SIDs. `show` displays
the stored EVs, any SRH containers, and any nondefault rail assignments.

Use `deny 42 2` and `allow 42 2` with the same directory to change EV slot 1
while the endpoint runs. The sender refreshes administrative state every
10 ms and reports applied changes as `ev_admin_updates`. A profile in use
stays online until the QP releases it. Use `offline 42` and `remove 42` to
delete it after the run. See [conformance](CONFORMANCE.md) for the
remaining controller and routing features.

### RoCE port administration

`mrc_ctl_query_port()` reports the administrative state of a
one-based verbs port. `mrc_ctl_modify_port()` changes that state with
`CAP_NET_ADMIN` in the network namespace that owns the port. The
controller resolves the primary RoCE netdev through RDMA netlink.
Disabling a port clears `IFF_UP` on that Linux interface, which affects
all traffic on it. Administrative state is shared across processes.

The port number is relative to the verbs device in the MRC context.
On the Sparks, each RoCE device exposes physical port 1. The DPDK
endpoint reads the link state of each attached port. It advertises
reachability to the peer and leaves a down rail out of path selection
until the link returns.
On an unbonded mlx5 RoCE port, `ibv_query_port()` reports the port down
when its netdev is administratively disabled. [Linux mlx5 RoCE port state](https://github.com/torvalds/linux/blob/v6.17/drivers/infiniband/hw/mlx5/main.c)

### Spark double ring

Run the [revision gate](CONFORMANCE.md#verification) immediately before
starting the test. Build on each selected host from its clean test worktree.
On the direct Spark-04 to Spark-02 pair, use the following route mapping:

| End | Local IP | Peer IP | Path 0 PCI / peer MAC | Path 1 PCI / peer MAC |
| --- | --- | --- | --- | --- |
| Spark-04 | 10.61.0.2 | 10.61.0.1 | `0000:01:00.0` / `4c:bb:47:82:4e:db` | `0002:01:00.0` / `4c:bb:47:82:4e:df` |
| Spark-02 | 10.61.0.1 | 10.61.0.2 | `0000:01:00.1` / `4c:bb:47:2e:7e:65` | `0002:01:00.1` / `4c:bb:47:2e:7e:69` |

Use EAL `-a` for each selected PCI address and `--iova-mode=va --no-huge`
for the private mlx5 runtime. Start Spark-02 as `rx` and Spark-04 as `tx`.
The configured IP pair identifies the QP across both circuits, while each
circuit uses its corresponding peer MAC. UDP 4971 is the default and has
been verified on both rails with RoCE enabled on the ConnectX-7 ports.
Use `--udp-port N` on both peers to select another destination port.
With out-of-band negotiation, `--service-time 1` offers local Service
Time reporting and the requester follows the peer's offer. With manual
peer configuration, enable the option on both benchmark endpoints to
report and consume Service Time.

For the healthy baseline, run a 4 MiB WRITE (4194304 bytes) with both paths
active. For a controlled recovery run, use 16777216 bytes and
`--ack-timeout 13` on both peers. Add `--drop-path-after 512
--restore-path-after 2048` on the sender. This injects a temporary path-1
blackhole in the endpoint. Check for retransmissions and traffic resuming
on path 1. Bit 1 in `ev_failed_mask` and `ev_recovered_mask` identifies
that path's failure and recovery. The test uses the existing Polatis
circuits.

On failure, the endpoint prints per-path application RX and DPDK port counter
deltas, together with protocol progress. Pass `--port-stats 1` to include the
same diagnostics on success.

### Persistent congestion-control profiles

With both contexts using the profile directory, a controller with effective
`CAP_NET_ADMIN` can configure or query up to 32 CC profiles. Choose
`uet-1.0-nscc` and set the profile OFFLINE, then supply a
`struct mrc_ctl_cc_nscc_cfg` and set it ONLINE. The provider uses the
configured initial and maximum window, base RTT, target delay, byte and
time thresholds, and increase and decrease coefficients. Set `disable_qa`
to zero to enable Quick Adapt. `qa_gate` ranges from 0 to 31 and divides
the maximum window by a power of two to set the achieved-byte gate.
`qa_threshold` sets the average queuing-delay threshold in nanoseconds.
Trim NACKs can also trigger a reduction. Quick Adapt preserves a smaller
window already imposed by congestion or receiver flow control. A NULL
algorithm and configuration select disabled congestion control while
retaining the QP's reliability window.

Select an ONLINE profile with `MRC_QP_CC_PROFILE_ID` when bringing a public
QP to INIT. The QP leases the profile until reset or destruction. The
controller can move the profile OFFLINE once all leases are released.
An unset ID uses the built-in NSCC defaults with Quick Adapt disabled. A CC query's configuration
pointer is borrowed until the next configuration query on the same thread.

An attached responder can use
`mrc_dpdk_set_rx_flow_control(qp, penalty, restore)` to signal receiver
congestion in its next SACKs. The penalty defaults to zero on each QP.
Values from 1 through 127 reduce the NSCC requester's window according
to the newly acknowledged byte count. Set the penalty back to zero to
disable it. A zero penalty with `restore=true` asks the requester to
restore the saved window. The application chooses when to set or clear
the penalty.

### Concurrent three-Spark tests

The cycle uses Spark-04 → Spark-02 → Spark-03 → Spark-04 on both Polatis
rings. Each Spark receives and sends using separate PCI functions, lcores,
and DPDK runtime directories. Each profile sends 256 MiB per edge.
In `failover`, Spark-04 injects a path-1 blackhole on its outgoing edge
from scheduled segments 512 through 2047. The other two edges keep sending
during the same run.

The test uses the installed root-owned endpoint snapshots. Render the unit
files for each Spark, substituting its host number and snapshot directory.

```sh
python3 tests/render_spark_cycle_units.py --host 04 \
    --snapshot /opt/ocs-mrc/0c53748.ZDc2j9 --output-dir /tmp/spark-04-cycle-units
```

The proven smoke and failover snapshot is `/opt/ocs-mrc/0c53748.ZDc2j9`.
An administrator installs four units per Spark and permits
passwordless `/usr/bin/systemctl start UNIT` and
`/usr/bin/systemctl stop UNIT` for those exact units. The four names are
`ocs-mrc-cycle-rx-smoke.service`, `ocs-mrc-cycle-tx-smoke.service`,
`ocs-mrc-cycle-rx-failover.service`, and `ocs-mrc-cycle-tx-failover.service`.
All four units need `UnsetEnvironment=RUNTIME_DIRECTORY` in `[Service]`,
as emitted by the renderer. Run `sudo systemctl daemon-reload` after updating
the units.

The harness reads each unit's journal. Where ordinary journal access is
unavailable, permit this exact command separately for each installed unit.
Escape the commas in the option value when writing a sudoers rule.

```sh
/usr/bin/journalctl --quiet --no-pager --output=json --output-fields=MESSAGE,_SYSTEMD_INVOCATION_ID,__REALTIME_TIMESTAMP --unit UNIT --lines=200
```

Run the test from Spark-04. Use the full revision of the installed snapshots
and the corresponding detached test worktrees.

```sh
revision=0c537484a96b15e51c9cfd82491de83d65d554f4
python3 tests/check_spark_cycle.py --revision "$revision" --profile smoke --preflight-only
python3 tests/check_spark_cycle.py --revision "$revision" --profile smoke
python3 tests/check_spark_cycle.py --revision "$revision" --profile failover
```

Before each run, the harness checks all three Git revisions and compares the
installed endpoint checksums. It waits for both ports on every receiver,
then starts the senders in parallel. It validates delivered packets on both
rails, successful WRITEs on each edge, overlapping sender intervals from
port initialization to completion, and EV failure and recovery on the
injected edge.

The `negotiated` profile runs eight 32 MiB WRITEIMMs per edge. It uses a TCP
QP handshake with registered memory and two in-flight WRITEIMM credits.
Each receiver advertises an initial PSN four packets before wrap and
reduces its MPR from eight units to one after 65 accepted segments. The
harness checks eight requester completions, eight receiver immediate
completions, and one MPR update on each peer.

For this profile, render the units with `--include-negotiated` and install
`ocs-mrc-cycle-rx-negotiated.service` and
`ocs-mrc-cycle-tx-negotiated.service` on all three Sparks. Apply the same
scoped start, stop, and journal rules to those two units. The controller
opens management SSH tunnels on loopback ports 46002, 46003, and 46004.
Those connections need SSH local and remote forwarding enabled. The QP
handshakes use the tunnels and WRITEIMM packets travel over the optical
rings. Use `--profile negotiated --preflight-only` to check the installed
services and loopback listeners before running the new profile.

## Original multipath prototype

`mrc_bench` is the original prototype. It sends one host-memory buffer over two DPDK Ethernet ports.
It stripes 1,400-byte segments across the ports, places segments by sequence
number, and sends selective ACKs as 64-segment bitmaps. When a segment's ACK
times out, the sender retries it on the other path when available and temporarily
avoids the timed-out path for new segments. The receiver verifies the entire
buffer before reporting success.

This benchmark uses a private frame format with the experimental Ethernet
EtherType `0x88b5`. It requires two paths that can deliver Ethernet frames to
the peer MACs on untagged Ethernet. It implements a single unidirectional transfer with a fixed
send window and an ACK timer. The OCP MRC wire format, RDMA verbs, SRv6, path
probes, and congestion control are outside the scope of this prototype.

## Build and run on two hosts

Install a C compiler, Make, pkg-config, and DPDK development libraries with the
PMD for the target NIC. The build was checked with DPDK 23.11.7 on Linux AArch64. Run the commands below from the `mrc-dpdk` directory.

```sh
make
```

On each host, identify the two data interfaces and their local MAC and PCI
addresses with `ip link` and `ethtool -i`. Match the two DPDK port IDs to
the corresponding peer MACs. The commands below assume DPDK port IDs 0 and 1.
Use the kernel-bound `mlx5_core` driver for ConnectX-7. Use the benchmark's
ports as data interfaces during the run, since the mlx5 PMD's default receive
steering can take over unicast traffic on the same MAC. Make sure the selected
ports have an Ethernet MTU of at least 1,420 bytes. The endpoint logs its DPDK
port IDs and device names at startup. See the [mlx5 PMD documentation](https://doc.dpdk.org/guides/nics/mlx5.html).

Start the receiver with the sender's MAC addresses.

```sh
sudo ./mrc_bench -l 2 -a <RX_PCI_0> -a <RX_PCI_1> -- \
    rx 4194304 1 0 <TX_MAC_0> 1 <TX_MAC_1>
```

Start the sender with the receiver's MAC addresses and the same byte count and
transfer ID.

```sh
sudo ./mrc_bench -l 2 -a <TX_PCI_0> -a <TX_PCI_1> -- \
    tx 4194304 1 0 <RX_MAC_0> 1 <RX_MAC_1>
```

Use an ID unique to the run on those links, and a byte count between 1 and
1,073,741,824. Use the same `--rto-us` at both endpoints if changing the
default of 10,000 µs. `--timeout-ms` is the no-progress timeout and defaults
to 30,000 ms. Add `--drop-path-after 512` to the sender to start silently
dropping path 1 frames after 512 first-send segments have been scheduled.
Both endpoints emit JSON counters. Sender goodput covers transmission through
the final ACK. Use `--trace FILE.csv` to record packet events for comparison.
Tracing adds per-packet work, so leave it off for timing runs.

The [original RoCEv2 baseline](../summary.md) used two logical paths over
one QSFP cable. The current Polatis tests use the separate circuits listed
in [Spark double ring](#spark-double-ring).

## Linux SRv6 fabric verification

Use the AF_PACKET PMD and Linux with user and network namespaces,
veth, iproute2, and SRv6 local actions. Build the controller fixture
against the OCP headers. It seeds the EV profile using an accessible
Ethernet verbs device.

```sh
make mrc_write MRC_INCLUDE_DIR=/path/to/ocp/headers tests/test_mrc_ctl_abi
unshare -Urn python3 tests/check_mrc_srv6_fabric.py
unshare -Urn python3 tests/check_mrc_srv6_fabric.py --fault ecn
unshare -Urn python3 tests/check_mrc_srv6_fabric.py --fault trim
unshare -Urn python3 tests/check_mrc_srv6_fabric.py --fault link
unshare -Urn python3 tests/check_mrc_srv6_fabric.py --jumbo
```

Use `--af-packet-driver /path/to/librte_net_af_packet.so.24` for a
separately built driver that DPDK does not load automatically.
Each transfer uses separate endpoints, a
management link for OOB negotiation, and two independent Linux routers.
The test checks SRH advancement on both rails, terminal decapsulation
on one rail, WRITEIMM completion and payload, and a live Dynamic MPR
update.

The ECN case configures both routers to mark every sixteenth ECT(0)
packet. It reads back each installed CE rule, counts packets with CE after
marking, and checks SKIP SACKs for each reflected EV and the requester's
response. On the decapsulation rail, an explicit nftables rule merges
outer CE into an ECN-capable inner IPv6 header before `End.DX6`; the test
counts CE again on the bare inner packets leaving the router. This policy
is necessary on kernels whose native `End.DX6` action discards outer CE.
A separate four-packet namespace test checks all inner ECN codepoints:
Not-ECT remains Not-ECT, while ECT(0), ECT(1), and CE emerge as CE. Its
counters are separate from the MRC transfer. The merge is restricted to
the fixture's single-container IPv6/SRH/IPv6 layout and does not establish
full RFC 6040 handling of an outer-CE/inner-Not-ECT packet. These checks
verify deterministic software-switch configuration and endpoint feedback,
not queue-driven WRED, native `End.DX6` ECN propagation, or physical-switch
behavior.

The trim case forwards sender-injected trims and checks alternate-rail recovery.
The jumbo case uses the TAP PMD, `/dev/net/tun`, and Linux bridges with
MTU 4500. It negotiates 4096-byte segments and checks that both routers
see full-sized packets, including on the rail that decapsulates the SRH.
Use `--tap-driver /path/to/librte_net_tap.so.24` for a separately built
TAP driver. The ECN, link, and jumbo cases require nftables.
The link case uses nftables and tc netem to observe and pace a 4 MiB
transfer. It brings one sender interface down and back up, checks the
Port Status masks on the surviving rail, and verifies that both rails
forward traffic after restoration. For AF_PACKET ports, the endpoint
reads the Linux interface's administrative and carrier flags.

## Behavioral parity with the emulator

Use an upstream [`srv6-mrc-emulator`](https://github.com/segmentrouting/srv6-mrc-emulator)
checkout. The comparison was developed against
`ccd0f5eb711743ae27a83c236445d33e633ee18d`. The test runs the original
`mrc_bench` over two DPDK `net_memif` virtual links. It requires Linux, two usable CPU
cores, Python 3, and the memif PMD. It needs no physical NICs or hugepages.

```sh
python3 tests/check_dpdk_mrc_parity.py --emulator /path/to/srv6-mrc-emulator
```

- The healthy trace must match the emulator's two-plane EV-spray scheduler
  packet for packet.
- Both receive traces are replayed through the emulator's reorder tracker. The
  duplicate count, reorder count, maximum reorder distance, and per-path counts
  must agree exactly.
- During the blackhole case, every dropped segment must be retried on the
  survivor. After the emulator's EV policy demotes the same failed path, its
  choices must agree with the prototype's steady-state path selection.

The sender's timeout and the emulator's loss-window detection run independently,
so this check compares the resulting path choice. The memif run measures
behavior on one host. It does not measure Spark link throughput or physical
failover timing.
