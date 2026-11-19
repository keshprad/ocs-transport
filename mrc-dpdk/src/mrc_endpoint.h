#ifndef MRC_ENDPOINT_H
#define MRC_ENDPOINT_H

#include "mrc_ctl_store.h"
#include "mrc_provider.h"
#include "mrc_wire.h"

#include <rte_ether.h>

enum {
    PATH_COUNT = 2,
    PMTU = 1024,
    WINDOW = 1024,
    MAX_WORK_REQUESTS = 4096,
};

struct mrc_endpoint_path {
    uint16_t port;
    struct rte_ether_addr local;
    struct rte_ether_addr peer;
};

struct mrc_endpoint_write {
    uint32_t first_packet;
    uint32_t packets;
    uint32_t offset;
    uint32_t length;
    uint32_t preceding_immediates;
    uint32_t immediate;
    bool with_immediate;
};

struct mrc_endpoint_config {
    bool sender;
    bool continuous_send;
    bool trim_nack_enabled;
    uint32_t bytes;
    uint32_t packets;
    uint32_t uniform_wr_packets;
    struct mrc_endpoint_write *work;
    uint32_t work_requests;
    uint32_t immediate_requests;
    uint32_t max_wimm_inflight;
    uint32_t initial_psn;
    uint32_t requestor_qpn;
    uint32_t responder_qpn;
    uint32_t remote_key;
    uint32_t window;
    uint32_t rx_ooo_capacity;
    uint32_t rx_staging_capacity;
    uint32_t rx_work_capacity;
    uint64_t remote_address;
    uint16_t partition_key;
    uint16_t peer_partition_key;
    uint32_t local_ip;
    uint32_t peer_ip;
    uint8_t local_ip6[MRC_IPV6_ADDR_LEN];
    uint8_t peer_ip6[MRC_IPV6_ADDR_LEN];
    enum mrc_wire_ip_version ip_version;
    uint8_t local_sid[MRC_IPV6_ADDR_LEN];
    uint8_t peer_sid[MRC_IPV6_ADDR_LEN];
    bool srv6;
    bool srv6_srh;
    uint32_t drop_after;
    uint32_t ev_deny_mask;
    uint64_t profile_id;
    struct mrc_ctl_store profile_store;
    struct mrc_ctl_store_profile profile;
    uint32_t restore_after;
    uint32_t mpr_change_at;
    uint8_t mpr_change_to;
    uint8_t mpr_max;
    bool dynamic_mpr;
    uint32_t trim_after;
    uint32_t final_ack_drops;
    uint16_t udp_port;
    uint8_t trim_dscp;
    unsigned int timeout_exponent;
    unsigned int linear_retries;
    unsigned int exponential_retries;
    uint64_t hz;
    uint64_t idle_timeout;
    uint64_t initial_rto;
    struct mrc_endpoint_path paths[PATH_COUNT];
    struct rte_mempool *pool;
    uint8_t *data;
    struct mrc_provider_pd *pd;
};

struct mrc_endpoint_rx_counters {
    uint64_t frames;
    uint64_t matched;
    uint64_t rejected;
    uint64_t other_peer;
};

struct mrc_endpoint_counters {
    struct mrc_endpoint_rx_counters rx[PATH_COUNT];
    uint32_t sent[PATH_COUNT];
    uint32_t received[PATH_COUNT];
    uint32_t retries[PATH_COUNT];
    uint32_t dropped;
    uint32_t trimmed;
    uint32_t trim_nacks;
    uint32_t nack_sent_mask;
    uint32_t nack_received_mask;
    uint32_t port_status_updates;
    uint32_t port_status_replies;
    uint32_t probe_requests;
    uint32_t probe_replies;
    uint32_t probe_sent[PATH_COUNT];
    uint32_t ev_skipped, ev_failed, ev_recovered, ev_admin_updates;
    uint32_t ev_failed_mask, ev_recovered_mask;
    uint32_t mpr_updates;
    uint32_t fast_retries;
    uint32_t sacks;
    uint32_t transport_acks;
    uint32_t dropped_transport_acks;
    uint32_t duplicates;
    uint32_t reorder_max;
    uint32_t immediate_completions;
    uint32_t next_receive_work;
    uint32_t write_completions;
    uint32_t work_errors;
    uint32_t work_flushed;
    enum mrc_provider_wc_status first_work_error;
    bool qp_error;
};

struct mrc_requester;
struct mrc_responder;

bool mrc_endpoint_append_write(struct mrc_endpoint_config *transfer, size_t capacity,
                               uint32_t length, bool with_immediate,
                               uint32_t immediate);
uint64_t mrc_endpoint_cycles(const struct mrc_endpoint_config *transfer, uint64_t ns);

/* The caller owns the ports, configuration, QP and counters for the lifetime
 * of each endpoint. It posts work and polls its CQs independently. The
 * requester snapshots the currently posted send batch at creation.
 * With continuous_send, it can start with an empty SQ and admits later
 * batches once the preceding batch has retired. Polls on a given endpoint
 * must be serialized with QP teardown and reset. Progress returns 0 while
 * active or waiting for new work, 1 on finite completion, or negative errno
 * after transitioning the QP to ERROR and flushing outstanding work. */
int mrc_requester_create(const struct mrc_endpoint_config *transfer, struct mrc_provider_qp *qp,
                         struct mrc_endpoint_counters *counts, struct mrc_requester **requester);
int mrc_requester_progress(struct mrc_requester *requester);
void mrc_requester_destroy(struct mrc_requester *requester);

/* Set packets to zero for an ongoing responder with no benchmark end check. */
int mrc_responder_create(const struct mrc_endpoint_config *transfer, struct mrc_provider_qp *qp,
                         struct mrc_endpoint_counters *counts, struct mrc_responder **responder);
int mrc_responder_progress(struct mrc_responder *responder);
uint32_t mrc_responder_ack_msn(const struct mrc_responder *responder);
void mrc_responder_destroy(struct mrc_responder *responder);

#endif
