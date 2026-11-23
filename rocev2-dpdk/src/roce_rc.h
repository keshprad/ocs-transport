#ifndef ROCE_RC_H
#define ROCE_RC_H

#include "roce_wire.h"

enum {
    ROCE_SEND_WINDOW_DEFAULT = 32,
    ROCE_SEND_WINDOW_MAX = 512,
};

struct roce_qp {
    struct roce_address local;
    struct roce_address peer;
    uint32_t local_qpn;
    uint32_t peer_qpn;
    uint16_t pmtu;
};

struct roce_region {
    uint64_t address;
    uint32_t rkey;
    uint32_t size;
};

struct roce_requester {
    struct roce_qp qp;
    struct roce_region remote;
    const uint8_t *source;
    uint32_t initial_psn;
    uint32_t total;
    uint32_t window;
    uint32_t acked;
    uint32_t next;
    uint32_t sent;
    uint64_t last_progress;
    unsigned int retries;
    unsigned int retries_left;
    unsigned int retry_limit;
    bool failed;
};

struct roce_responder {
    struct roce_qp qp;
    struct roce_region region;
    uint8_t *buffer;
    uint32_t expected;
    uint32_t msn;
    uint32_t offset;
    uint32_t remaining;
    uint32_t accepted;
    bool reported_gap;
    bool failed;
};

bool roce_valid_pmtu(uint16_t pmtu);
bool roce_requester_init(struct roce_requester *r, const struct roce_qp *qp,
                         uint32_t psn, struct roce_region remote,
                         const uint8_t *source, unsigned int retry_limit, uint32_t window);
bool roce_requester_next(const struct roce_requester *r, uint32_t staged,
                         struct roce_packet *packet);
void roce_requester_sent(struct roce_requester *r, uint32_t count, uint64_t now);
void roce_requester_feedback(struct roce_requester *r,
                             const struct roce_packet *packet, uint64_t now);
void roce_requester_poll_timer(struct roce_requester *r, uint64_t now, uint64_t timeout);
bool roce_responder_receive(struct roce_responder *r, const struct roce_packet *packet,
                            struct roce_packet *feedback);

#endif
