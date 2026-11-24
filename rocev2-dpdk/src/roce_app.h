#ifndef ROCE_APP_H
#define ROCE_APP_H

#include "roce_mr_server.h"
#include "roce_doorbell.h"
#include "roce_lab.h"
#include "roce_native_queue.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct roce_app_qp_progress {
    _Alignas(64) _Atomic uint32_t admitted;
    _Atomic uint32_t completed;
};

struct roce_app_work {
    struct roce_doorbell doorbells[ROCE_LAB_MAX_QPS];
    struct roce_app_qp_progress qps[ROCE_LAB_MAX_QPS];
};

struct roce_app_memory {
    int socket;
    uint8_t *buffer;
    size_t length;
    uint32_t qpn;
    struct roce_app_work *work;
    struct roce_native_work *native;
    struct roce_mr_lease mr;
};

struct roce_app_config {
    struct roce_mr_server *registrations;
    uint32_t post_batch;
    bool dma_writable;
    bool in_band_cm;
    bool scatter_gather;
    bool native_queues;
    bool rdmacm;
};

bool roce_app_attach(const char *path, const struct roce_lab_options *options,
                     const struct roce_app_config *config, struct roce_app_memory *memory);
bool roce_app_connected(struct roce_app_memory *memory, const struct roce_lab_options *options);
bool roce_app_offer_native(struct roce_app_memory *memory, const struct roce_lab_options *options,
                           const struct roce_lab_offer *local, const struct roce_lab_offer *peer,
                           bool announce_established);
bool roce_app_complete(struct roce_app_memory *memory, const struct roce_lab_options *options);
void roce_app_flush_native(struct roce_app_memory *memory, const struct roce_lab_options *options);
void roce_app_detach(struct roce_app_memory *memory);
bool roce_app_write(const char *path, const char *registration_device);

#endif
