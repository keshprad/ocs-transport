#ifndef ROCE_CM_LAB_OFFER_H
#define ROCE_CM_LAB_OFFER_H

#include "roce_lab.h"

enum {
    ROCE_CM_LAB_OFFER_SIZE = 32,
    ROCE_CM_LAB_PRIVATE_SIZE = 56,
    ROCE_CM_LAB_REPLY_SIZE = ROCE_CM_LAB_PRIVATE_SIZE + ROCE_LAB_AUXILIARY_SIZE,
    ROCE_CM_LAB_MAX_APP_DATA = ROCE_CM_LAB_PRIVATE_SIZE - ROCE_CM_LAB_OFFER_SIZE - 1,
};

#define ROCE_CM_LAB_REQUEST_TAG "roce-dpdk-client\0v1"
#define ROCE_CM_LAB_REPLY_TAG "roce-dpdk-server\0v1"

size_t roce_cm_lab_offer_encode(uint8_t *data, size_t capacity, const struct roce_lab_options *options,
                                const struct roce_lab_offer *local, const void *app_data, uint8_t app_length);

/* Decode the application payload in either CM REQ or REP, including fixed-width zero fill. */
bool roce_cm_lab_offer_decode(const void *data, size_t length, const struct roce_lab_options *options,
                              struct roce_lab_offer *peer, const uint8_t **app_data, uint8_t *app_length);

#endif
