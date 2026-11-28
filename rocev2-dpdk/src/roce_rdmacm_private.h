#ifndef ROCE_RDMACM_PRIVATE_H
#define ROCE_RDMACM_PRIVATE_H

struct ibv_context;
struct rdma_event_channel;

/* Borrow the lab application's provider context before creating any CM ID.
 * The caller keeps the context alive until all CM IDs have been destroyed.
 * Returns zero or a positive errno. */
int roce_rdmacm_use_context(struct rdma_event_channel *channel, struct ibv_context *context);

#endif
