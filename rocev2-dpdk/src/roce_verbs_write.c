#include "roce_cm_lab_offer.h"
#include "roce_lab.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct write_resources {
    uint8_t *buffer;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *mr;
    struct ibv_qp *qp;
};

static enum ibv_mtu verbs_mtu(uint16_t pmtu)
{
    switch (pmtu) {
    case 256: return IBV_MTU_256;
    case 512: return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    }
    return 0;
}

static union ibv_gid ipv4_gid(uint32_t ip)
{
    union ibv_gid gid = { .raw = { [10] = 0xff, [11] = 0xff } };
    uint32_t network = htonl(ip);
    memcpy(gid.raw + 12, &network, sizeof(network));
    return gid;
}

static bool read_gid_mac(struct ibv_context *context, uint8_t port, unsigned int gid_index,
                         struct roce_address *address)
{
    struct ibv_gid_entry entry;
    union ibv_gid expected = ipv4_gid(address->ipv4);
    if (ibv_query_gid_ex(context, port, gid_index, &entry, 0) ||
        entry.gid_type != IBV_GID_TYPE_ROCE_V2 ||
        memcmp(entry.gid.raw, expected.raw, sizeof(expected.raw)))
        return false;

    struct ifreq request = {0};
    if (!if_indextoname(entry.ndev_ifindex, request.ifr_name))
        return false;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    int result = ioctl(fd, SIOCGIFHWADDR, &request);
    close(fd);
    if (result || request.ifr_hwaddr.sa_family != ARPHRD_ETHER)
        return false;
    memcpy(address->mac, request.ifr_hwaddr.sa_data, ROCE_MAC_LEN);
    return true;
}

static struct ibv_context *open_device(const char *name)
{
    struct ibv_device **devices = ibv_get_device_list(NULL);
    if (!devices)
        return NULL;
    struct ibv_context *context = NULL;
    for (size_t i = 0; devices[i]; ++i) {
        if (strcmp(ibv_get_device_name(devices[i]), name) == 0) {
            context = ibv_open_device(devices[i]);
            break;
        }
    }
    ibv_free_device_list(devices);
    return context;
}

static bool match_port(struct ibv_context *context, uint8_t port, uint8_t gid_index,
                       struct roce_lab_offer *local)
{
    struct ibv_port_attr info;
    uint16_t pkey;
    return !ibv_query_port(context, port, &info) && info.state == IBV_PORT_ACTIVE &&
           info.link_layer == IBV_LINK_LAYER_ETHERNET && info.active_mtu >= verbs_mtu(local->pmtu) &&
           !ibv_query_pkey(context, port, 0, &pkey) && ntohs(pkey) == ROCE_PKEY &&
           read_gid_mac(context, port, gid_index, &local->data);
}

static bool allocate_write_resources(struct write_resources *resources, struct ibv_context *context,
                                      struct rdma_cm_id *id, const struct roce_lab_options *options,
                                      struct roce_lab_offer *local)
{
    resources->buffer = malloc(options->size);
    if (!resources->buffer)
        return false;
    roce_lab_fill(resources->buffer, options->size, options->sender);
    resources->pd = ibv_alloc_pd(context);
    resources->cq = ibv_create_cq(context, 1, NULL, NULL, 0);
    if (!resources->pd || !resources->cq)
        return false;
    resources->mr = ibv_reg_mr(resources->pd, resources->buffer, options->size,
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!resources->mr)
        return false;
    struct ibv_qp_init_attr init = {
        .send_cq = resources->cq, .recv_cq = resources->cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
    };
    if (id) {
        if (rdma_create_qp(id, resources->pd, &init))
            return false;
        resources->qp = id->qp;
    } else {
        resources->qp = ibv_create_qp(resources->pd, &init);
        if (!resources->qp)
            return false;
    }
    local->qpn = resources->qp->qp_num;
    local->region = (struct roce_region) {
        .address = (uintptr_t)resources->buffer, .rkey = resources->mr->rkey, .size = options->size,
    };
    return true;
}

static void release_write_resources(struct write_resources *resources, struct rdma_cm_id *id)
{
    if (resources->qp) {
        if (id)
            rdma_destroy_qp(id);
        else
            ibv_destroy_qp(resources->qp);
    }
    if (resources->mr)
        ibv_dereg_mr(resources->mr);
    if (resources->cq)
        ibv_destroy_cq(resources->cq);
    if (resources->pd)
        ibv_dealloc_pd(resources->pd);
    free(resources->buffer);
}

static bool modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int mask)
{
    int result = ibv_modify_qp(qp, attr, mask);
    if (result)
        fprintf(stderr, "QP transition to state %d failed: %s\n", attr->qp_state, strerror(result));
    return result == 0;
}

static bool connect_qp(struct ibv_qp *qp, uint8_t port, uint8_t gid_index,
                       const struct roce_lab_offer *local, const struct roce_lab_offer *peer)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT, .port_num = port, .pkey_index = 0,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE,
    };
    if (!modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
        return false;

    uint16_t pmtu = local->pmtu < peer->pmtu ? local->pmtu : peer->pmtu;
    attr = (struct ibv_qp_attr) {
        .qp_state = IBV_QPS_RTR, .path_mtu = verbs_mtu(pmtu), .dest_qp_num = peer->qpn,
        .rq_psn = peer->psn, .min_rnr_timer = 12,
        .ah_attr = {
            .port_num = port, .is_global = 1,
            .grh = { .dgid = ipv4_gid(peer->data.ipv4), .sgid_index = gid_index, .hop_limit = 64 },
        },
    };
    if (!modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                             IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
        return false;

    attr = (struct ibv_qp_attr) {
        .qp_state = IBV_QPS_RTS, .sq_psn = local->psn,
        .timeout = 14, .retry_cnt = ROCE_LAB_RETRIES, .rnr_retry = 0,
    };
    return modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                               IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
}

static bool write_and_wait(struct ibv_qp *qp, struct ibv_cq *cq, const struct ibv_mr *mr,
                           const struct roce_region *peer)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)mr->addr, .length = peer->size, .lkey = mr->lkey,
    };
    struct ibv_send_wr wr = {
        .sg_list = &sge, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = { .remote_addr = peer->address, .rkey = peer->rkey },
    };
    struct ibv_send_wr *bad;
    int err = ibv_post_send(qp, &wr, &bad);
    if (err) {
        fprintf(stderr, "ibv_post_send: %s\n", strerror(err));
        return false;
    }
    uint64_t deadline = roce_now_ns() + (uint64_t)ROCE_LAB_TIMEOUT * 1000000000;
    const struct timespec interval = { .tv_nsec = 50000 };
    do {
        struct ibv_wc wc;
        int count = ibv_poll_cq(cq, 1, &wc);
        if (count < 0)
            break;
        if (count > 0) {
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RDMA_WRITE)
                return true;
            fprintf(stderr, "WRITE completion: %s (vendor error %#x)\n",
                    ibv_wc_status_str(wc.status), wc.vendor_err);
            return false;
        }
        nanosleep(&interval, NULL);
    } while (roce_now_ns() < deadline);
    fprintf(stderr, "WRITE completion missing\n");
    return false;
}

static bool verify_write(int fd, const struct roce_lab_options *options,
                         const struct write_resources *resources, const struct roce_lab_offer *peer)
{
    if (options->sender) {
        if (!write_and_wait(resources->qp, resources->cq, resources->mr, &peer->region) ||
            !roce_lab_send(fd, 'D') || roce_lab_receive(fd, 'V', false) != 1)
            return false;
        printf("WRITE complete: %u bytes, native verbs completion, peer verified\n", options->size);
    } else {
        if (roce_lab_receive(fd, 'D', false) != 1 || !roce_lab_verify(resources->buffer, options->size) ||
            !roce_lab_send(fd, 'V'))
            return false;
        printf("WRITE verified: %u bytes in native registered memory\n", options->size);
    }
    return true;
}

static struct rdma_cm_event *wait_cm_event(struct rdma_cm_id *id, enum rdma_cm_event_type expected)
{
    uint64_t deadline = roce_now_ns() + (uint64_t)ROCE_LAB_TIMEOUT * 1000000000;
    for (uint64_t now = roce_now_ns(); now < deadline; now = roce_now_ns()) {
        struct pollfd wait = { .fd = id->channel->fd, .events = POLLIN };
        int timeout = (int)((deadline - now + 999999) / 1000000);
        int ready = poll(&wait, 1, timeout);
        if (ready <= 0) {
            if (!ready || errno == EINTR)
                continue;
            return NULL;
        }
        if (!(wait.revents & POLLIN)) {
            errno = wait.revents & POLLNVAL ? EBADF : ECONNRESET;
            return NULL;
        }
        struct rdma_cm_event *event;
        if (rdma_get_cm_event(id->channel, &event)) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return NULL;
        }

        bool matches = expected == RDMA_CM_EVENT_CONNECT_REQUEST ?
                       event->listen_id == id && event->id && event->id != id : event->id == id;
        if (matches && event->event == expected && !event->status)
            return event;
        fprintf(stderr, "RDMA CM expected %s, received %s (status %d)\n",
                rdma_event_str(expected), rdma_event_str(event->event), event->status);
        int error = event->event == RDMA_CM_EVENT_REJECTED ? ECONNREFUSED : EPROTO;
        if (event->status < 0 && event->status >= -4095)
            error = -event->status;
        struct rdma_cm_id *child = event->event == RDMA_CM_EVENT_CONNECT_REQUEST ? event->id : NULL;
        rdma_ack_cm_event(event);
        if (child && child != id)
            rdma_destroy_id(child);
        errno = error;
        return NULL;
    }
    fprintf(stderr, "RDMA CM timed out waiting for %s\n", rdma_event_str(expected));
    errno = ETIMEDOUT;
    return NULL;
}

static bool expect_cm_event(struct rdma_cm_id *id, enum rdma_cm_event_type expected)
{
    struct rdma_cm_event *event = wait_cm_event(id, expected);
    return event && !rdma_ack_cm_event(event);
}

static bool cm_addresses_match(struct rdma_cm_id *id, const struct roce_lab_options *options,
                               uint32_t peer_ip)
{
    const struct sockaddr_in *local = (const struct sockaddr_in *)rdma_get_local_addr(id);
    const struct sockaddr_in *peer = (const struct sockaddr_in *)rdma_get_peer_addr(id);
    return local->sin_family == AF_INET && local->sin_addr.s_addr == htonl(options->data_ip) &&
           peer->sin_family == AF_INET && peer->sin_addr.s_addr == htonl(peer_ip) &&
           ntohs(options->sender ? peer->sin_port : local->sin_port) == options->control_port;
}

static bool decode_cm_offer(const struct rdma_cm_event *event, const struct roce_lab_options *options,
                            struct roce_lab_offer *peer, const char *tag, size_t tag_length)
{
    const uint8_t *app;
    uint8_t length;
    return roce_cm_lab_offer_decode(event->param.conn.private_data, event->param.conn.private_data_len,
                                     options, peer, &app, &length) &&
           (!length || (length == tag_length && !memcmp(app, tag, length)));
}

static bool cm_path_matches(struct rdma_cm_id *id, const struct roce_lab_offer *local,
                            const struct roce_lab_offer *peer, uint8_t gid_index)
{
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_RTR };
    int mask;
    if (rdma_init_qp_attr(id, &attr, &mask))
        return false;

    uint16_t pmtu = local->pmtu < peer->pmtu ? local->pmtu : peer->pmtu;
    union ibv_gid expected = ipv4_gid(peer->data.ipv4);
    const int required = IBV_QP_PATH_MTU | IBV_QP_AV;
    if ((mask & required) == required && attr.path_mtu == verbs_mtu(pmtu) && attr.ah_attr.is_global &&
        attr.ah_attr.port_num == id->port_num && attr.ah_attr.grh.sgid_index == gid_index &&
        !memcmp(attr.ah_attr.grh.dgid.raw, expected.raw, sizeof(expected.raw)))
        return true;
    fprintf(stderr, "RDMA CM path mismatch: MTU code %u (expected %u for %u bytes), "
                    "port %u (expected %u), GID index %u (expected %u), mask %#x\n",
            attr.path_mtu, verbs_mtu(pmtu), pmtu, attr.ah_attr.port_num, id->port_num,
            attr.ah_attr.grh.sgid_index, gid_index, mask);
    errno = EPROTO;
    return false;
}

static bool open_cm_connection(struct rdma_cm_id *id, const struct roce_lab_options *options,
                                const struct roce_lab_offer *local, struct roce_lab_offer *peer)
{
    static const char request[] = ROCE_CM_LAB_REQUEST_TAG;
    static const char reply[] = ROCE_CM_LAB_REPLY_TAG;
    const char *tag = options->sender ? request : reply;
    uint8_t tag_length = options->sender ? sizeof(request) : sizeof(reply);
    uint8_t data[ROCE_CM_LAB_PRIVATE_SIZE];
    size_t length = roce_cm_lab_offer_encode(data, options, local, tag, tag_length);
    if (!length) {
        errno = EINVAL;
        return false;
    }
    struct rdma_conn_param params = {
        .private_data = data, .private_data_len = (uint8_t)length, .retry_count = ROCE_LAB_RETRIES,
    };
    if (options->sender ? rdma_connect(id, &params) : rdma_accept(id, &params))
        return false;
    struct rdma_cm_event *event = wait_cm_event(id, RDMA_CM_EVENT_ESTABLISHED);
    if (!event)
        return false;
    bool valid = !options->sender || decode_cm_offer(event, options, peer, reply, sizeof(reply));
    if (rdma_ack_cm_event(event))
        return false;
    if (!valid)
        errno = EPROTO;
    return valid;
}

static int run_cm(const struct roce_lab_options *options, const char *device, uint8_t port, uint8_t gid_index,
                  uint32_t peer_ip)
{
    static const char request[] = ROCE_CM_LAB_REQUEST_TAG;
    struct roce_lab_offer local = { .data.ipv4 = options->data_ip, .pmtu = options->pmtu };
    struct roce_lab_offer peer = { .data.ipv4 = peer_ip };
    struct sockaddr_in local_addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(options->data_ip),
        .sin_port = options->sender ? 0 : htons(options->control_port),
    };
    struct sockaddr_in peer_addr = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(peer_ip), .sin_port = htons(options->control_port),
    };
    struct write_resources resources = {0};
    struct rdma_cm_id *id = NULL, *listener = NULL;
    struct rdma_event_channel *channel = rdma_create_event_channel();
    if (!channel) {
        perror("rdma_create_event_channel");
        return 1;
    }
    int fd = -1, result = 1;
    const char *stage = "RDMA CM event channel";
    int flags = fcntl(channel->fd, F_GETFL);
    if (flags < 0 || fcntl(channel->fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto failed;

    /* Install the RDMA listener before the TCP verification socket admits its peer. */
    stage = "RDMA CM listener";
    if (!options->sender &&
        (rdma_create_id(channel, &listener, NULL, RDMA_PS_TCP) ||
         rdma_bind_addr(listener, (struct sockaddr *)&local_addr) || rdma_listen(listener, 1)))
        goto failed;
    stage = "TCP WRITE verification socket";
    fd = roce_lab_open(options);
    if (fd < 0)
        goto failed;

    if (options->sender) {
        stage = "RDMA CM address resolution";
        if (rdma_create_id(channel, &id, NULL, RDMA_PS_TCP) ||
            rdma_resolve_addr(id, (struct sockaddr *)&local_addr, (struct sockaddr *)&peer_addr,
                              ROCE_LAB_TIMEOUT * 1000) || !expect_cm_event(id, RDMA_CM_EVENT_ADDR_RESOLVED))
            goto failed;
        stage = "RDMA CM route resolution";
        if (rdma_resolve_route(id, ROCE_LAB_TIMEOUT * 1000) || !expect_cm_event(id, RDMA_CM_EVENT_ROUTE_RESOLVED))
            goto failed;
    } else {
        stage = "RDMA CM request";
        struct rdma_cm_event *event = wait_cm_event(listener, RDMA_CM_EVENT_CONNECT_REQUEST);
        if (!event)
            goto failed;
        id = event->id;
        bool valid = decode_cm_offer(event, options, &peer, request, sizeof(request));
        if (rdma_ack_cm_event(event))
            goto failed;
        if (!valid) {
            errno = EPROTO;
            goto failed;
        }
    }

    stage = "RDMA CM device, port, P_Key or IPv4 RoCEv2 GID mismatch";
    if (!id->verbs || strcmp(ibv_get_device_name(id->verbs->device), device) || id->port_num != port ||
        !cm_addresses_match(id, options, peer_ip) || !match_port(id->verbs, port, gid_index, &local)) {
        errno = EPROTO;
        goto failed;
    }
    stage = "allocate RDMA CM verbs resources";
    if (!allocate_write_resources(&resources, id->verbs, id, options, &local))
        goto failed;
    stage = "RDMA CM receive path";
    if (!options->sender && !cm_path_matches(id, &local, &peer, gid_index))
        goto failed;
    stage = "RDMA CM connection";
    if (!open_cm_connection(id, options, &local, &peer))
        goto failed;
    stage = "RDMA CM negotiated path";
    if (options->sender && !cm_path_matches(id, &local, &peer, gid_index))
        goto failed;
    puts("Native RoCEv2 CM established");
    fflush(stdout);

    if (!verify_write(fd, options, &resources, &peer)) {
        fprintf(stderr, "Native RoCEv2 WRITE verification failed\n");
        goto cleanup;
    }
    stage = "RDMA CM disconnect";
    /* On a passive ID, rdma_disconnect answers the DREQ that caused the event. */
    if ((options->sender && rdma_disconnect(id)) || !expect_cm_event(id, RDMA_CM_EVENT_DISCONNECTED) ||
        (!options->sender && rdma_disconnect(id)))
        goto failed;
    puts("Native RoCEv2 CM disconnected");
    result = 0;
    goto cleanup;

failed:
    perror(stage);
cleanup:
    if (fd >= 0)
        close(fd);
    release_write_resources(&resources, id);
    if (id)
        rdma_destroy_id(id);
    if (listener)
        rdma_destroy_id(listener);
    rdma_destroy_event_channel(channel);
    return result;
}

int main(int argc, char **argv)
{
    struct roce_lab_options options;
    uint32_t port, gid_index;
    bool in_band = argc == 12;
    struct in_addr cm_peer;
    if ((argc != 10 && !in_band) || !roce_lab_parse(argv, &options) ||
        !roce_parse_uint(argv[8], UINT8_MAX, &port) || !port ||
        !roce_parse_uint(argv[9], UINT8_MAX, &gid_index) ||
        (in_band && (strcmp(argv[10], "--cm-peer") || inet_pton(AF_INET, argv[11], &cm_peer) != 1 ||
                     !cm_peer.s_addr || ntohl(cm_peer.s_addr) == options.data_ip))) {
        fprintf(stderr, "usage: roce_verbs_write tx|rx BYTES CONTROL_IP TCP_PORT DATA_IP PMTU "
                        "RDMA_DEVICE PORT GID_INDEX [--cm-peer IPV4]\n");
        return 1;
    }
    if (in_band)
        return run_cm(&options, argv[7], (uint8_t)port, (uint8_t)gid_index, ntohl(cm_peer.s_addr));

    struct ibv_context *context = open_device(argv[7]);
    if (!context) {
        fprintf(stderr, "could not open verbs device %s\n", argv[7]);
        return 1;
    }

    int result = 1;
    struct roce_lab_offer local = { .data.ipv4 = options.data_ip, .pmtu = options.pmtu }, peer;
    if (!match_port(context, (uint8_t)port, (uint8_t)gid_index, &local)) {
        fprintf(stderr, "port, active MTU, P_Key or IPv4 RoCEv2 GID does not match\n");
        goto cleanup_device;
    }

    struct write_resources resources = {0};
    if (!allocate_write_resources(&resources, context, NULL, &options, &local)) {
        perror("allocate verbs resources");
        goto cleanup_resources;
    }
    if (!roce_lab_random(&local.psn))
        goto cleanup_resources;
    local.psn &= ROCE_PSN_MASK;

    int fd = roce_lab_open(&options);
    if (fd < 0) {
        perror("control socket");
        goto cleanup_resources;
    }
    if (!roce_lab_exchange(fd, &options, &local, &peer) ||
        !connect_qp(resources.qp, (uint8_t)port, (uint8_t)gid_index, &local, &peer) || !roce_lab_ready(fd)) {
        fprintf(stderr, "control setup failed\n");
    } else if (verify_write(fd, &options, &resources, &peer)) {
        result = 0;
    }
    close(fd);
cleanup_resources:
    release_write_resources(&resources, NULL);
cleanup_device:
    ibv_close_device(context);
    return result;
}
