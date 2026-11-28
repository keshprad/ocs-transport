#ifndef ROCE_LAB_H
#define ROCE_LAB_H

#include "roce_rc.h"

enum { ROCE_LAB_TIMEOUT = 30, ROCE_LAB_RETRIES = 7, ROCE_LAB_MAX_QPS = 16 };
enum { ROCE_LAB_AUXILIARY_SIZE = 16 };

struct roce_lab_options {
    bool sender;
    uint32_t size;
    uint32_t writes;
    uint32_t qps;
    uint32_t workers;
    uint32_t control_ip;
    uint16_t control_port;
    uint32_t data_ip;
    uint16_t pmtu;
    bool multi_mr_targets;
};

struct roce_lab_offer {
    struct roce_address data;
    struct roce_region region;
    struct roce_region auxiliary;
    uint32_t qpn;
    uint32_t psn; /* Starting transmit PSN for this endpoint. */
    uint16_t pmtu;
};

static inline uint64_t roce_lab_auxiliary_bytes(uint32_t qps, uint32_t writes, uint32_t size)
{
    return (uint64_t)qps * (writes / 2) * size;
}

/* Odd WRITEs use the auxiliary region, with independent packed slices per QP. */
static inline uint64_t roce_lab_aux_offset(uint32_t qp, uint32_t write, uint32_t writes, uint32_t size)
{
    return roce_lab_auxiliary_bytes(qp, writes, size) + (uint64_t)(write / 2) * size;
}

bool roce_lab_encode_auxiliary(uint8_t data[ROCE_LAB_AUXILIARY_SIZE], const struct roce_lab_offer *offer,
                               const struct roce_lab_options *options);
bool roce_lab_decode_auxiliary(const uint8_t data[ROCE_LAB_AUXILIARY_SIZE], struct roce_lab_offer *offer,
                               const struct roce_lab_options *options);

bool roce_parse_uint(const char *arg, uint32_t max, uint32_t *value);
bool roce_lab_parse(char **argv, struct roce_lab_options *options);
bool roce_lab_random(uint32_t *value);
bool roce_lab_wait(int fd, short events);
int roce_lab_open(const struct roce_lab_options *options);
bool roce_lab_exchange(int fd, const struct roce_lab_options *options,
                       const struct roce_lab_offer *local, struct roce_lab_offer *peer);
bool roce_lab_ready(int fd);
bool roce_lab_wait_start_gate(uint16_t gate_port, uint16_t control_port);
bool roce_lab_ready_after_gate(int fd, uint16_t gate_port, uint16_t control_port);
int roce_lab_receive(int fd, char expected, bool nonblocking);
bool roce_lab_send(int fd, char message);
void roce_lab_fill(uint8_t *buffer, size_t size, bool sender);
void roce_lab_fill_range(uint8_t *buffer, size_t size, size_t offset, bool sender);
bool roce_lab_verify(const uint8_t *buffer, size_t size);
bool roce_lab_verify_range(const uint8_t *buffer, size_t size, size_t logical_offset, bool written);
uint64_t roce_now_ns(void);

#endif
