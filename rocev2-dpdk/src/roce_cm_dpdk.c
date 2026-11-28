#include "roce_cm_dpdk.h"
#include "roce_app.h"
#include "roce_cm_lab_offer.h"

#include <errno.h>
#include <unistd.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

_Static_assert((int)ROCE_CM_LAB_PRIVATE_SIZE <= ROCE_CM_REQ_PRIVATE_SIZE - ROCE_CMA_IPV4_SIZE,
               "lab private data fits in a CM REQ");
_Static_assert((int)ROCE_CM_LAB_REPLY_SIZE <= ROCE_CM_REP_PRIVATE_SIZE,
               "auxiliary lab target fits in a CM REP");
_Static_assert((int)ROCE_CMA_PRIVATE_MAX <= (int)ROCE_CM_LAB_MAX_APP_DATA,
               "application private data fits in the lab offer");

static uint8_t mtu_code(uint16_t pmtu)
{
    uint8_t code = 1;
    while (pmtu > 256) {
        pmtu /= 2;
        ++code;
    }
    return code;
}

static uint64_t node_guid(const uint8_t mac[ROCE_MAC_LEN])
{
    return (uint64_t)(mac[0] ^ 2) << 56 | (uint64_t)mac[1] << 48 | (uint64_t)mac[2] << 40 |
           UINT64_C(0xfffe000000) | (uint64_t)mac[3] << 16 | (uint64_t)mac[4] << 8 | mac[5];
}

static bool first_write(struct rte_mbuf *mbuf, struct roce_responder *responder)
{
    uint8_t storage[ROCE_MAX_FRAME];
    uint32_t length = rte_pktmbuf_pkt_len(mbuf);
    const void *frame = length <= sizeof(storage) ? rte_pktmbuf_read(mbuf, 0, length, storage) : NULL;
    struct roce_packet packet;
    return frame && roce_decode(&packet, frame, length) && roce_responder_can_establish(responder, &packet);
}

static void receive_cm(uint16_t port, struct roce_cm_port *cm, struct roce_responder *responder,
                        uint64_t now)
{
    struct roce_cm_session *session = &cm->session;
    struct rte_mbuf *received[ROCE_CM_RX_BURST];
    uint16_t count = rte_eth_rx_burst(port, 0, received, RTE_DIM(received));
    uint8_t storage[ROCE_CM_FRAME_SIZE];
    for (uint16_t i = 0; i < count; ++i) {
        struct rte_mbuf *mbuf = received[i];
        const uint8_t *frame = rte_pktmbuf_pkt_len(mbuf) == sizeof(storage) ?
            rte_pktmbuf_read(mbuf, 0, sizeof(storage), storage) : NULL;
        if (frame && roce_cm_session_receive(session, now, frame, sizeof(storage))) {
            rte_pktmbuf_free(mbuf);
            continue;
        }
        if (responder && session->phase == ROCE_CM_PENDING && first_write(mbuf, responder) &&
            roce_cm_session_peer_ready(session))
            puts("RoCEv2 CM confirmed by first RC WRITE");
        /* RTU and the first RC packets may share this RX burst. */
        if (responder && session->phase == ROCE_CM_ESTABLISHED)
            cm->early[cm->early_count++] = mbuf;
        else
            rte_pktmbuf_free(mbuf);
    }
}

void roce_cm_transmit(uint16_t port, struct rte_mempool *pool, struct roce_cm_session *session,
                      uint64_t now)
{
    uint8_t storage[ROCE_CM_FRAME_SIZE];
    size_t length = roce_cm_session_prepare(session, now, storage, sizeof(storage));
    if (!length)
        return;
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf)
        return;
    void *packet = rte_pktmbuf_append(mbuf, (uint16_t)length);
    if (packet) {
        memcpy(packet, storage, length);
        if (rte_eth_tx_burst(port, 0, &mbuf, 1) == 1) {
            roce_cm_session_sent(session, roce_now_ns());
            return;
        }
    }
    rte_pktmbuf_free(mbuf);
}

struct cm_application_wait {
    uint16_t port;
    struct rte_mempool *pool;
    struct roce_cm_port *cm;
};

static void progress_cm(void *context, uint64_t now)
{
    struct cm_application_wait *wait = context;
    receive_cm(wait->port, wait->cm, NULL, now);
    roce_cm_transmit(wait->port, wait->pool, &wait->cm->session, now);
}

static bool passive_accept(struct roce_cma_service *cma, struct roce_cm_session *session,
                           const uint8_t *data, uint8_t length, const struct roce_cma_wait *wait)
{
    if (!roce_cma_service_request(cma, session->config.cma_source_port, session->peer_qpn,
                                  data, length, wait))
        return false;
    const struct roce_cma_message *response = &cma->app_parameters;
    if (response->type != ROCE_CMA_REJECT)
        return true;
    errno = roce_cm_session_reject_request(session, response->private_data, response->private_length) ?
            ECONNREFUSED : EPROTO;
    return false;
}

static bool prepare_application(struct roce_cma_service *cma, struct roce_app_memory *application,
                                const struct roce_lab_options *options, struct roce_lab_offer *local,
                                const struct roce_lab_offer *peer, struct roce_cm_session *session,
                                const uint8_t *private_data, uint8_t private_length,
                                const struct roce_cma_wait *wait)
{
    cma->peer_qpn = session->peer_qpn;
    if (!options->sender && cma->owned_qp) {
        /* The passive application supplies its new QPN in ACCEPT. */
        if (!passive_accept(cma, session, private_data, private_length, wait))
            return false;
        local->qpn = cma->local_qpn;
    }

    int queue_fd = -1;
    if (!roce_app_offer_native(application, options, local, peer, false, cma->owned_qp ? &queue_fd : NULL))
        return false;
    bool ready;
    if (options->sender) {
        ready = roce_cma_service_response(cma, private_data, private_length, queue_fd, wait) &&
                roce_cm_session_establish(session);
    } else if (cma->owned_qp) {
        ready = roce_cma_service_configure_qp(cma, queue_fd, wait) &&
                roce_cm_session_set_local_qpn(session, local->qpn);
    } else {
        /* An external QP is configured through the application socket before ACCEPT. */
        ready = passive_accept(cma, session, private_data, private_length, wait);
    }
    if (queue_fd >= 0) {
        int error = errno;
        close(queue_fd);
        errno = error;
    }
    if (!ready || options->sender)
        return ready;

    const struct roce_cma_message *parameters = &cma->app_parameters;
    session->config.private_length = roce_cm_lab_offer_encode(session->local_private_data,
                                                              sizeof(session->local_private_data), options, local,
                                                              parameters->private_data, parameters->private_length);
    return session->config.private_length != 0;
}

bool roce_cm_lab_exchange(uint16_t port, struct rte_mempool *pool,
                          const struct roce_lab_options *options, struct roce_lab_offer *local,
                          const struct roce_address *destination, struct roce_lab_offer *peer,
                          struct roce_cm_port *cm, struct roce_cma_service *cma,
                          struct roce_app_memory *application)
{
    struct roce_cm_session *session = &cm->session;
    uint32_t comm_id, nonce[2], receive_psn;
    if (!roce_lab_random(&comm_id) || !roce_lab_random(&nonce[0]) || !roce_lab_random(&nonce[1]) ||
        !roce_lab_random(&receive_psn))
        return false;
    if (cma && !roce_cma_service_prepare(cma))
        return false;
    if (cma && options->sender)
        local->qpn = cma->local_qpn;
    uint8_t private_data[ROCE_CM_LAB_REPLY_SIZE];
    const struct roce_cma_message *parameters = cma ? &cma->app_parameters : NULL;
    size_t private_length = roce_cm_lab_offer_encode(private_data, sizeof(private_data), options, local,
                                                     parameters ? parameters->private_data : NULL,
                                                     parameters ? parameters->private_length : 0);
    if (!private_length)
        return false;
    uint16_t source_port = 0xc000u | (comm_id & 0x3fffu);
    const struct roce_cm_session_config config = {
        .initiator = options->sender,
        .manual_establish = cma && options->sender,
        .defer_local_qpn = cma && cma->owned_qp && !options->sender,
        .tid = ((uint64_t)nonce[0] << 32 | nonce[1]) | 1,
        .local_comm_id = comm_id ? comm_id : 1,
        .local = local->data, .peer = *destination,
        .connection = { .local_ca_guid = node_guid(local->data.mac), .local_qpn = local->qpn,
                        .starting_psn = receive_psn & ROCE_PSN_MASK, .flow_control = true },
        .service_id = ROCE_CMA_TCP_SERVICE_BASE | options->control_port,
        .cma_source_port = cma ? cma->local_port : source_port, .udp_source = source_port, .pkey = ROCE_PKEY,
        .mtu = mtu_code(local->pmtu), .private_data = private_data,
        .private_length = private_length, .retry_ns = UINT64_C(250000000),
        .packet_lifetime_ns = UINT64_C(1000000), .max_retries = 7,
    };
    if (!roce_cm_session_init(session, &config))
        return false;

    uint64_t deadline = roce_now_ns() + ROCE_LAB_TIMEOUT * UINT64_C(1000000000);
    bool valid_peer = false;
    struct roce_responder responder;
    struct roce_mr_destination destination_mr = { .anchor = &application->mr };
    uint8_t cma_private_length = 0;
    const uint8_t *cma_private = NULL;
    for (uint64_t now = roce_now_ns(); now < deadline; now = roce_now_ns()) {
        receive_cm(port, cm, valid_peer && !options->sender ? &responder : NULL, now);
        if (session->phase == ROCE_CM_REJECTED) {
            if (options->sender) {
                if (cma && !roce_cma_service_event(cma, ROCE_CMA_REJECTED, session->reject_reason,
                                                   session->peer_private_data, session->peer_private_length))
                    return false;
                errno = ECONNREFUSED;
                return false;
            }
            roce_cm_transmit(port, pool, session, now);
            if (!roce_cm_session_replays_reject(session, now))
                break;
            rte_pause();
            continue;
        }
        if (session->peer_private_length && !valid_peer) {
            *peer = (struct roce_lab_offer) {
                .data = *destination, .qpn = session->peer_qpn, .psn = session->config.connection.starting_psn,
            };
            if (!roce_cm_lab_offer_decode(session->peer_private_data, session->peer_private_length, options,
                                          peer, &cma_private, &cma_private_length) ||
                (!options->sender && session->mtu != mtu_code(RTE_MIN(options->pmtu, peer->pmtu))))
                return false;
            local->psn = session->send_psn;
            if (cma) {
                struct cm_application_wait context = { .port = port, .pool = pool, .cm = cm };
                const struct roce_cma_wait wait = { .deadline = deadline, .progress = progress_cm,
                                                   .context = &context };
                if (!roce_cm_session_hold_response(session, now, deadline))
                    return false;
                if (!prepare_application(cma, application, options, local, peer, session,
                                         cma_private, cma_private_length, &wait)) {
                    if (session->phase == ROCE_CM_REJECTED)
                        continue;
                    return false;
                }
                roce_cm_session_resume_response(session);
            }
            if (!options->sender) {
                responder = (struct roce_responder) {
                    .qp = { .local = local->data, .peer = peer->data, .local_qpn = local->qpn,
                            .pmtu = RTE_MIN(local->pmtu, peer->pmtu) },
                    .region = { .address = local->region.address, .rkey = local->region.rkey,
                                .size = (uint64_t)options->size * options->writes },
                    .expected = peer->psn,
                };
                if (options->multi_mr_targets) {
                    responder.resolve = roce_mr_resolve_write;
                    responder.release = roce_mr_release_write;
                    responder.target_context = &destination_mr;
                }
            }
            valid_peer = true;
            /* Application setup can block. Recheck the exchange deadline before TX. */
            now = roce_now_ns();
            if (now >= deadline)
                return false;
        }
        roce_cm_transmit(port, pool, session, now);
        if (session->phase == ROCE_CM_ESTABLISHED) {
            if (!cma || (options->sender && !cma->owned_qp))
                return true;
            return roce_cma_service_event(cma, ROCE_CMA_ESTABLISHED, 0,
                                          options->sender ? cma_private : NULL,
                                          options->sender ? cma_private_length : 0);
        }
        if (session->phase != ROCE_CM_PENDING)
            return false;
        rte_pause();
    }
    errno = session->phase == ROCE_CM_REJECTED ? ECONNREFUSED : ETIMEDOUT;
    return false;
}

bool roce_cm_lab_close(uint16_t port, struct rte_mempool *pool,
                      struct roce_cm_port *cm, struct roce_cma_service *cma)
{
    struct roce_cm_session *session = &cm->session;
    if (cma && !roce_cma_service_wait_disconnect(cma))
        return false;
    bool initiator = session->config.initiator;
    if (initiator) {
        uint32_t nonce[2];
        if (!roce_lab_random(&nonce[0]) || !roce_lab_random(&nonce[1]))
            return false;
        uint64_t tid = ((uint64_t)nonce[0] << 32 | nonce[1]) | 1;
        if (!roce_cm_session_disconnect(session, tid))
            return false;
    }
    bool notified = false;
    uint64_t deadline = roce_now_ns() + ROCE_LAB_TIMEOUT * UINT64_C(1000000000);
    for (uint64_t now = roce_now_ns(); now < deadline; now = roce_now_ns()) {
        receive_cm(port, cm, NULL, now);
        roce_cm_transmit(port, pool, session, now);
        if (session->phase == ROCE_CM_DISCONNECTED) {
            if (session->disconnect_error)
                return false;
            if (!notified) {
                if (cma && !roce_cma_service_event(cma, ROCE_CMA_DISCONNECTED, 0, NULL, 0))
                    return false;
                notified = true;
                /* A transmitted DREP can enter TIMEWAIT after the poll timestamp. */
                now = roce_now_ns();
            }
            /* The DREP sender keeps QP1 available to replay a lost reply. */
            if (!session->peer_dreq_received || !roce_cm_session_in_timewait(session, now))
                return true;
        }
        rte_pause();
    }
    return false;
}
