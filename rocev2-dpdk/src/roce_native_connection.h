#ifndef ROCE_NATIVE_CONNECTION_H
#define ROCE_NATIVE_CONNECTION_H

#include <stdbool.h>
#include <stdint.h>

struct ibv_context;
struct ibv_qp;
struct roce_native_queue_config;
struct roce_native_work;

struct ibv_context *roce_native_open_device(void);
int roce_native_init_qp(struct ibv_qp *qp, bool receiver);
int roce_native_configure_qp(struct ibv_qp *qp, int queue_fd, bool receiver);
const struct roce_native_work *roce_native_map_work(int fd, bool receiver,
                                                  struct roce_native_queue_config *config);
int roce_native_bind_work(struct ibv_context *context, int queue_fd);
int roce_native_connect_qp(struct ibv_qp *qp, const struct roce_native_queue_config *config,
                           uint32_t index);

#endif
