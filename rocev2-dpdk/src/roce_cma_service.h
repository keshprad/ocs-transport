#ifndef ROCE_CMA_SERVICE_H
#define ROCE_CMA_SERVICE_H

#include "roce_cma_ipc.h"

struct roce_cma_service {
    const char *path;
    int listener;
    int socket;
    bool sender;
    bool owned_qp;
    uint16_t local_port;
    uint16_t peer_port;
    uint32_t local_qpn;
    uint32_t peer_qpn;
    struct roce_cma_lab_address address;
    struct roce_cma_message app_parameters;
};

struct roce_cma_wait {
    uint64_t deadline;
    void (*progress)(void *context, uint64_t now);
    void *context;
};

bool roce_cma_service_listen(struct roce_cma_service *service, const char *path);
bool roce_cma_service_attach(struct roce_cma_service *service, int app_socket,
                             const struct roce_cma_lab_address *address, uint32_t local_qpn, bool sender);
bool roce_cma_service_prepare(struct roce_cma_service *service);
bool roce_cma_service_request(struct roce_cma_service *service, uint16_t peer_port, uint32_t peer_qpn,
                              const uint8_t *private_data, uint8_t private_length,
                              const struct roce_cma_wait *wait);
bool roce_cma_service_configure_qp(const struct roce_cma_service *service, int queue_fd,
                                  const struct roce_cma_wait *wait);
bool roce_cma_service_response(const struct roce_cma_service *service,
                               const uint8_t *private_data, uint8_t private_length, int queue_fd,
                               const struct roce_cma_wait *wait);
bool roce_cma_service_event(const struct roce_cma_service *service, enum roce_cma_message_type type,
                            int status, const uint8_t *private_data, uint8_t private_length);
bool roce_cma_service_wait_disconnect(const struct roce_cma_service *service);
void roce_cma_service_close(struct roce_cma_service *service);

#endif
