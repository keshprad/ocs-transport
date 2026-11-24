#include "roce_cm_dpdk.h"

#include <arpa/inet.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

enum { CM_RX_BURST = 16, CM_LAB_WORDS = 8, CM_LAB_MAGIC = 0x52434d31 };
enum { CM_LAB_OFFER_SIZE = CM_LAB_WORDS * sizeof(uint32_t) };
_Static_assert(CM_LAB_OFFER_SIZE + 1 + ROCE_CMA_PRIVATE_MAX <= ROCE_CM_REQ_PRIVATE_SIZE - ROCE_CMA_IPV4_SIZE,
               "lab offer and application private data fit in a CM REQ");

static size_t store_cma_private(uint8_t *data, const struct roce_cma_message *app)
{
    data[CM_LAB_OFFER_SIZE] = app->private_length;
    memcpy(data + CM_LAB_OFFER_SIZE + 1, app->private_data, app->private_length);
    return CM_LAB_OFFER_SIZE + 1 + app->private_length;
}

static bool load_cma_private(const struct roce_cm_session *session, uint8_t *length)
{
    if (session->peer_private_length < CM_LAB_OFFER_SIZE + 1)
        return false;
    *length = session->peer_private_data[CM_LAB_OFFER_SIZE];
    return *length <= ROCE_CMA_PRIVATE_MAX &&
           CM_LAB_OFFER_SIZE + 1u + *length <= session->peer_private_length;
}

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

/* Lab clients carry the remote WRITE buffer and verification workload in CM private data. */
static bool peer_offer(const struct roce_cm_session *session, const struct roce_lab_options *options,
                       const struct roce_address *address, struct roce_lab_offer *peer)
{
    uint32_t words[CM_LAB_WORDS];
    if (session->peer_private_length < sizeof(words))
        return false;
    memcpy(words, session->peer_private_data, sizeof(words));
    for (size_t i = 0; i < CM_LAB_WORDS; ++i)
        words[i] = ntohl(words[i]);

    uint64_t remote = (uint64_t)words[4] << 32 | words[5];
    uint64_t bytes = (uint64_t)options->size * options->writes;
    if (words[0] != CM_LAB_MAGIC || words[1] != (uint32_t)!options->sender ||
        words[2] != options->size || words[3] != options->writes ||
        bytes - 1 > UINT64_MAX - remote || words[7] > ROCE_MAX_PMTU ||
        !roce_valid_pmtu((uint16_t)words[7]) ||
        (!options->sender && session->mtu != mtu_code(RTE_MIN(options->pmtu, words[7]))))
        return false;

    *peer = (struct roce_lab_offer) {
        .data = *address, .qpn = session->peer_qpn, .psn = session->peer_psn,
        .pmtu = (uint16_t)words[7], .region = { .address = remote, .size = words[2], .rkey = words[6] },
    };
    return true;
}

static void receive_cm(uint16_t port, struct roce_cm_session *session, uint64_t now)
{
    struct rte_mbuf *received[CM_RX_BURST];
    uint16_t count = rte_eth_rx_burst(port, 0, received, RTE_DIM(received));
    uint8_t storage[ROCE_CM_FRAME_SIZE];
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *frame = rte_pktmbuf_read(received[i], 0, sizeof(storage), storage);
        if (frame)
            roce_cm_session_receive(session, now, frame, sizeof(storage));
        rte_pktmbuf_free(received[i]);
    }
}

static void transmit_cm(uint16_t port, struct rte_mempool *pool, struct roce_cm_session *session,
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
            roce_cm_session_sent(session, now);
            return;
        }
    }
    rte_pktmbuf_free(mbuf);
}

bool roce_cm_lab_exchange(uint16_t port, struct rte_mempool *pool, int control,
                          const struct roce_lab_options *options, const struct roce_lab_offer *local,
                          const struct roce_address *destination, struct roce_lab_offer *peer,
                          struct roce_cm_session *session, struct roce_cma_service *cma)
{
    uint32_t comm_id, nonce[2];
    if (!roce_lab_random(&comm_id) || !roce_lab_random(&nonce[0]) || !roce_lab_random(&nonce[1]))
        return false;
    if (cma && !roce_cma_service_prepare(cma))
        return false;
    uint32_t offer[CM_LAB_WORDS] = {
        htonl(CM_LAB_MAGIC), htonl(options->sender), htonl(options->size), htonl(options->writes),
        htonl((uint32_t)(local->region.address >> 32)), htonl((uint32_t)local->region.address),
        htonl(local->region.rkey), htonl(local->pmtu),
    };
    uint8_t private_data[CM_LAB_OFFER_SIZE + 1 + ROCE_CMA_PRIVATE_MAX];
    memcpy(private_data, offer, sizeof(offer));
    size_t private_length = cma ? store_cma_private(private_data, &cma->app_parameters) : sizeof(offer);
    uint16_t source_port = 0xc000u | (comm_id & 0x3fffu);
    const struct roce_cm_session_config config = {
        .initiator = options->sender,
        .manual_establish = cma && options->sender,
        .tid = ((uint64_t)nonce[0] << 32 | nonce[1]) | 1,
        .local_comm_id = comm_id ? comm_id : 1,
        .local = local->data, .peer = *destination,
        .connection = { .local_ca_guid = node_guid(local->data.mac), .local_qpn = local->qpn,
                        .starting_psn = local->psn, .flow_control = true },
        .service_id = ROCE_CMA_TCP_SERVICE_BASE | options->control_port,
        .cma_source_port = cma ? cma->local_port : source_port, .udp_source = source_port, .pkey = ROCE_PKEY,
        .mtu = mtu_code(local->pmtu), .private_data = private_data,
        .private_length = private_length, .retry_ns = UINT64_C(250000000), .max_retries = 7,
    };
    if (!roce_cm_session_init(session, &config))
        return false;

    uint64_t deadline = roce_now_ns() + ROCE_LAB_TIMEOUT * UINT64_C(1000000000);
    bool valid_peer = false;
    uint8_t cma_private_length = 0;
    const uint8_t *cma_private = session->peer_private_data + CM_LAB_OFFER_SIZE + 1;
    for (uint64_t now = roce_now_ns(); now < deadline; now = roce_now_ns()) {
        receive_cm(port, session, now);
        if (session->peer_private_length && !valid_peer) {
            if (!peer_offer(session, options, destination, peer))
                return false;
            if (cma && !load_cma_private(session, &cma_private_length))
                return false;
            if (cma)
                cma->peer_qpn = session->peer_qpn;
            if (cma && options->sender) {
                if (!roce_cma_service_response(cma, cma_private, cma_private_length) ||
                    !roce_cm_session_establish(session))
                    return false;
            } else if (cma) {
                /* A passive CM ID accepts the request before the first REP. */
                if (!roce_cma_service_request(cma, session->config.cma_source_port, session->peer_qpn,
                                               cma_private, cma_private_length))
                    return false;
                session->config.private_length = store_cma_private(session->local_private_data,
                                                                   &cma->app_parameters);
            }
            valid_peer = true;
        }
        transmit_cm(port, pool, session, now);
        if (session->phase == ROCE_CM_FAILED)
            return false;
        if (session->phase == ROCE_CM_ESTABLISHED) {
            /* Keep polling for duplicate REP until the lab peer confirms receipt of RTU. */
            if (!options->sender)
                return (!cma || roce_cma_service_event(cma, ROCE_CMA_ESTABLISHED, 0, NULL, 0)) &&
                       roce_lab_send(control, 'C');
            int ready = roce_lab_receive(control, 'C', true);
            if (ready)
                return ready == 1;
        }
        rte_pause();
    }
    return false;
}

bool roce_cm_lab_close(uint16_t port, struct rte_mempool *pool, int control,
                      struct roce_cm_session *session, struct roce_cma_service *cma)
{
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
    uint64_t deadline = roce_now_ns() + ROCE_LAB_TIMEOUT * UINT64_C(1000000000);
    for (uint64_t now = roce_now_ns(); now < deadline; now = roce_now_ns()) {
        receive_cm(port, session, now);
        transmit_cm(port, pool, session, now);
        if (session->phase == ROCE_CM_DISCONNECTED) {
            if (session->disconnect_error)
                return false;
            if (initiator)
                return roce_lab_send(control, 'Q') &&
                       (!cma || roce_cma_service_event(cma, ROCE_CMA_DISCONNECTED, 0, NULL, 0));
            /* Keep replaying DREP until the lab requester confirms receipt. */
            int closed = roce_lab_receive(control, 'Q', true);
            if (closed)
                return closed == 1 && (!cma || roce_cma_service_event(cma, ROCE_CMA_DISCONNECTED, 0, NULL, 0));
            if (!roce_cm_session_in_timewait(session, now))
                return false;
        }
        rte_pause();
    }
    return false;
}
