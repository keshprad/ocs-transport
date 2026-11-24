#ifndef ROCE_CM_SESSION_H
#define ROCE_CM_SESSION_H

#include "roce_cm_wire.h"

enum roce_cm_phase {
    ROCE_CM_PENDING,
    ROCE_CM_ESTABLISHED,
    ROCE_CM_FAILED,
    ROCE_CM_DISCONNECTING,
    ROCE_CM_DISCONNECTED,
};

struct roce_cm_session_config {
    bool initiator;
    bool manual_establish; /* An initiator waits for the application before sending RTU. */
    bool defer_local_qpn; /* A passive CM ID creates its QP after receiving REQ. */
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
    uint64_t packet_lifetime_ns; /* Maximum one-way flight time added to an MRA service timeout. */
    uint8_t max_retries; /* Additional REQ, REP or DREQ sends, 0 through 15. */
};

struct roce_cm_session {
    enum roce_cm_phase phase;
    uint32_t peer_qpn;
    uint32_t send_psn; /* PSN requested by the peer in REQ or REP. */
    uint8_t mtu;
    /* Received fixed-width user area, including its zero-fill. */
    uint8_t peer_private_data[ROCE_CM_REP_PRIVATE_SIZE];
    size_t peer_private_length;
    int disconnect_error; /* Positive ETIMEDOUT if a local DREQ exhausts retries. */

    struct roce_cm_session_config config;
    uint8_t local_private_data[ROCE_CM_REP_PRIVATE_SIZE];
    uint32_t peer_comm_id;
    uint64_t tid;
    uint64_t last_sent;
    uint64_t mra_received_at;
    uint64_t mra_service_ns;
    uint64_t disconnect_tid;
    uint64_t peer_dreq_tid;
    uint64_t disconnected_at;
    uint32_t gsi_psn;
    unsigned sends;
    unsigned dreps_sent;
    enum roce_cm_type prepared;
    bool received_peer;
    bool rtu_allowed;
    bool send_pending;
    bool local_disconnect;
    bool peer_dreq_received;
    bool drep_pending;
};

bool roce_cm_session_init(struct roce_cm_session *session,
                          const struct roce_cm_session_config *config);

/* Permit REP after a passive CM-owned QP is ready for the validated REQ. */
bool roce_cm_session_set_local_qpn(struct roce_cm_session *session, uint32_t qpn);

/* Authorize RTU after a valid REP on an initiator awaiting application approval. */
bool roce_cm_session_establish(struct roce_cm_session *session);

/* A validated first RC packet can confirm communication after a passive REP. */
bool roce_cm_session_peer_ready(struct roce_cm_session *session);

/* Queue a DREQ on an established session. Closing and closed sessions succeed
 * without starting another transaction. A received DREQ queues its own DREP. */
bool roce_cm_session_disconnect(struct roce_cm_session *session, uint64_t tid);

/* Keep the disconnected session available for duplicate DREQs for
 * (max_retries + 1) retry intervals after entering ROCE_CM_DISCONNECTED. */
bool roce_cm_session_in_timewait(const struct roce_cm_session *session, uint64_t now);

/* Pass monotonic nanoseconds for now. Report sent only after TX accepts the
 * frame from prepare. A zero length means there is no frame ready to send. */
size_t roce_cm_session_prepare(struct roce_cm_session *session, uint64_t now,
                               uint8_t *frame, size_t capacity);
void roce_cm_session_sent(struct roce_cm_session *session, uint64_t now);
bool roce_cm_session_receive(struct roce_cm_session *session, uint64_t now,
                             const uint8_t *frame, size_t length);

#endif
