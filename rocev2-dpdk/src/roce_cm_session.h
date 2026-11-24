#ifndef ROCE_CM_SESSION_H
#define ROCE_CM_SESSION_H

#include "roce_cm_wire.h"

enum roce_cm_phase {
    ROCE_CM_PENDING,
    ROCE_CM_ESTABLISHED,
    ROCE_CM_FAILED,
};

struct roce_cm_session_config {
    bool initiator;
    uint64_t tid; /* Initiator's transaction ID. Passive sessions learn it from REQ. */
    uint32_t local_comm_id;
    struct roce_address local;
    struct roce_address peer;
    struct roce_cm_connection connection;
    uint64_t service_id;
    uint16_t cma_source_port; /* Required for initiators. */
    uint16_t udp_source;
    uint16_t pkey;
    uint8_t mtu; /* IB MTU code, 1 through 5. */
    const uint8_t *private_data;
    size_t private_length;
    uint64_t retry_ns;
    uint8_t max_retries; /* Additional REQ or REP sends, 0 through 15. */
};

struct roce_cm_session {
    enum roce_cm_phase phase;
    uint32_t peer_qpn;
    uint32_t peer_psn;
    uint8_t mtu;
    /* Received fixed-width user area, including its zero-fill. */
    uint8_t peer_private_data[ROCE_CM_REP_PRIVATE_SIZE];
    size_t peer_private_length;

    struct roce_cm_session_config config;
    uint8_t local_private_data[ROCE_CM_REP_PRIVATE_SIZE];
    uint32_t peer_comm_id;
    uint64_t tid;
    uint64_t last_sent;
    uint32_t gsi_psn;
    unsigned sends;
    enum roce_cm_type prepared;
    bool received_peer;
    bool send_pending;
};

bool roce_cm_session_init(struct roce_cm_session *session,
                          const struct roce_cm_session_config *config);

/* Pass monotonic nanoseconds for now. Report sent only after TX accepts the
 * frame from prepare. A zero length means there is no frame ready to send. */
size_t roce_cm_session_prepare(struct roce_cm_session *session, uint64_t now,
                               uint8_t *frame, size_t capacity);
void roce_cm_session_sent(struct roce_cm_session *session, uint64_t now);
bool roce_cm_session_receive(struct roce_cm_session *session, uint64_t now,
                             const uint8_t *frame, size_t length);

#endif
