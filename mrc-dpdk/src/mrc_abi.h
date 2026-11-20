#ifndef MRC_DPDK_ABI_H
#define MRC_DPDK_ABI_H

#include "mrc_dpdk.h"

#include "mrc_provider.h"
#include "mrc_ctl_store.h"

#include <pthread.h>
#include <stdatomic.h>

#define MRC_PUBLIC __attribute__((visibility("default")))

enum {
    MRC_ABI_MAX_MPR = UINT8_MAX,
    MRC_ABI_MAX_WIMM = 1024,
    MRC_ABI_MAX_QP_HINTS = 1024,
    MRC_ABI_SL_COUNT = 16,
};

struct mrc_abi_route;
struct mrc_abi_endpoint;
struct mrc_endpoint_config;
struct mrc_endpoint_srv6_path;
struct mrc_wire_packet;
struct mrc_wire_srv6;
struct mrc_abi_async_notice;
struct mrc_abi_ev_cq;

struct mrc_abi_async_source {
    struct mrc_context *context;
    struct mrc_abi_async_notice *reserved;
    size_t unacked;
};

struct mrc_abi_async_channel {
    pthread_mutex_t mutex;
    pthread_cond_t acked;
    struct mrc_abi_async_notice *head;
    struct mrc_abi_async_notice *tail;
    int fd;
    int poll_fd;
    int native_fd;
    uint8_t port_count;
    bool native_active;
    bool native_fatal_seen;
};

struct mrc_abi_pd {
    struct mrc_abi_pd *next;
    const struct ibv_pd *verbs;
    struct mrc_provider_pd *provider;
    size_t users;
};

struct mrc_abi_sl_dscp {
    uint8_t data;
    uint8_t retry;
};

struct mrc_context {
    struct ibv_context *verbs; /* Borrowed until all MRC resources are destroyed. */
    pthread_mutex_t mutex;
    size_t children;
    struct mrc_abi_pd *pds;
    struct mrc_qp_hint *hints;
    size_t hint_count;
    struct mrc_abi_sl_dscp sl_dscp[MRC_ABI_SL_COUNT];
    struct mrc_abi_async_channel async;
    struct mrc_ctl_store profile_store;
    atomic_uint ctl_event_layout;
    struct mrc_abi_endpoint *endpoint;
};

struct mrc_comp_channel {
    struct mrc_context *context;
    struct mrc_provider_comp_channel *provider;
};

struct mrc_cq {
    struct mrc_context *context;
    struct mrc_provider_cq *provider;
    void *application_context;
    int capacity;
    struct mrc_abi_async_source async;
    struct mrc_abi_ev_cq *ev_events;
};

struct mrc_qp {
    struct mrc_context *context;
    struct mrc_abi_pd *pd;
    struct mrc_provider_qp *provider;
    pthread_mutex_t mutex;
    struct mrc_qp_init_attr init;
    struct ibv_qp_attr verbs;
    struct mrc_qp_attr attrs;
    union ibv_gid source_gid;
    struct mrc_abi_route *route;
    bool requester;
    bool responder;
    uint32_t qpn;
    struct mrc_ctl_store_ref ev_lease;
    struct mrc_ctl_store_ref cc_lease;
    struct mrc_ctl_store_cc_profile cc_profile;
    struct mrc_abi_async_source async;
};

struct mrc_ctl_store mrc_abi_profile_store(struct mrc_context *context);
bool mrc_abi_controller_capable(void);
bool mrc_abi_ev_event_state_supported(struct mrc_context *context);
void mrc_abi_ev_cq_stop(struct mrc_abi_ev_cq *events);
void mrc_abi_ev_cq_free(struct mrc_abi_ev_cq *events);
enum mrc_ctl_store_format mrc_abi_builtin_ev_format(uint64_t id);
unsigned int mrc_abi_builtin_ev_fields(enum mrc_ctl_store_format format);
int mrc_abi_retain_context(struct mrc_context *context);
void mrc_abi_release_context(struct mrc_context *context);
int mrc_abi_hint_retain(struct mrc_context *context, struct mrc_qp_hint *hint);
void mrc_abi_hint_release(struct mrc_qp_hint *hint);
int mrc_abi_async_open(struct mrc_abi_async_channel *channel, const struct ibv_context *verbs,
                        uint8_t port_count);
void mrc_abi_async_close(struct mrc_abi_async_channel *channel);
int mrc_abi_async_prepare(struct mrc_abi_async_source *source);
void mrc_abi_async_publish(struct mrc_abi_async_source *source, const struct mrc_async_event *event);
void mrc_abi_async_finish(struct mrc_abi_async_source *source);

/* Caller holds the QP mutex. */
void mrc_abi_release_route(struct mrc_qp *qp);
int mrc_abi_set_rx_mpr(struct mrc_qp *qp, uint8_t units);
uint8_t mrc_abi_requester_mpr(const struct mrc_qp *qp);

/* Import a caller-owned verbs registration with the permissions enabled for
 * MRC. Keep the verbs MR live until the provider lkey is deregistered. */
int mrc_abi_bind_mr(const struct mrc_context *context, struct mrc_provider_pd *provider,
                     const struct ibv_pd *verbs_pd, const struct ibv_mr *mr,
                     uint64_t iova, unsigned int access);

int mrc_abi_configure_addresses(struct mrc_endpoint_config *config, const union ibv_gid *local,
                                const union ibv_gid *peer);
int mrc_abi_validate_rails(const struct mrc_dpdk_rail *rails, unsigned int count);
int mrc_abi_prepare_srv6_paths(const struct mrc_dpdk_srv6_path *paths,
                              struct mrc_endpoint_srv6_path *prepared, unsigned int count);

/* Serialize shared DPDK queue I/O and route state. The dispatcher
 * never acquires context or QP locks. */
extern pthread_mutex_t mrc_abi_dispatch_mutex;
void mrc_abi_poll_endpoint_ports(const struct mrc_endpoint_config *config);
bool mrc_abi_dispatch_endpoint(uint16_t port, const struct mrc_wire_packet *packet,
                               const struct mrc_wire_srv6 *routing, uint64_t arrival_cycles);
bool mrc_abi_qp_port_token_in_use(uint16_t token);
bool mrc_abi_endpoint_port_token_in_use(uint16_t token);

#endif
