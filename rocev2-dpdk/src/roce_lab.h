#ifndef ROCE_LAB_H
#define ROCE_LAB_H

#include "roce_rc.h"

enum { ROCE_LAB_TIMEOUT = 30, ROCE_LAB_RETRIES = 7 };

struct roce_lab_options {
    bool sender;
    uint32_t size;
    uint32_t writes;
    uint32_t control_ip;
    uint16_t control_port;
    uint32_t data_ip;
    uint16_t pmtu;
};

struct roce_lab_offer {
    struct roce_address data;
    struct roce_region region;
    uint32_t qpn;
    uint32_t psn;
    uint16_t pmtu;
};

bool roce_parse_uint(const char *arg, uint32_t max, uint32_t *value);
bool roce_lab_parse(char **argv, struct roce_lab_options *options);
bool roce_lab_random(uint32_t *value);
int roce_lab_open(const struct roce_lab_options *options);
bool roce_lab_exchange(int fd, const struct roce_lab_options *options,
                       const struct roce_lab_offer *local, struct roce_lab_offer *peer);
bool roce_lab_ready(int fd);
int roce_lab_receive(int fd, char expected, bool nonblocking);
bool roce_lab_send(int fd, char message);
void roce_lab_fill(uint8_t *buffer, size_t size, bool sender);
bool roce_lab_verify(const uint8_t *buffer, size_t size);
uint64_t roce_now_ns(void);

#endif
