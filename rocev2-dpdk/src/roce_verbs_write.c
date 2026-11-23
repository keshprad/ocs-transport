#include "roce_lab.h"

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    struct roce_lab_options options;
    uint32_t port, gid_index;
    if (argc != 10 || !roce_lab_parse(argv, &options) ||
        !roce_parse_uint(argv[8], UINT8_MAX, &port) || !port ||
        !roce_parse_uint(argv[9], UINT8_MAX, &gid_index)) {
        fprintf(stderr, "usage: roce_verbs_write tx|rx BYTES CONTROL_IP TCP_PORT DATA_IP PMTU RDMA_DEVICE PORT GID_INDEX\n");
        return 1;
    }
    struct ibv_context *context = open_device(argv[7]);
    if (!context) {
        fprintf(stderr, "could not open verbs device %s\n", argv[7]);
        return 1;
    }

    int result = 1;
    struct roce_lab_offer local = { .data.ipv4 = options.data_ip, .pmtu = options.pmtu }, peer;
    struct ibv_port_attr port_info;
    uint16_t pkey;
    if (ibv_query_port(context, (uint8_t)port, &port_info) ||
        port_info.state != IBV_PORT_ACTIVE || port_info.link_layer != IBV_LINK_LAYER_ETHERNET ||
        port_info.active_mtu < verbs_mtu(options.pmtu) ||
        ibv_query_pkey(context, (uint8_t)port, 0, &pkey) || ntohs(pkey) != ROCE_PKEY ||
        !read_gid_mac(context, (uint8_t)port, gid_index, &local.data)) {
        fprintf(stderr, "port, active MTU, P_Key or IPv4 RoCEv2 GID does not match\n");
        goto cleanup_device;
    }

    uint8_t *buffer = malloc(options.size);
    if (!buffer)
        goto cleanup_device;
    roce_lab_fill(buffer, options.size, options.sender);
    struct ibv_pd *pd = ibv_alloc_pd(context);
    struct ibv_cq *cq = ibv_create_cq(context, 1, NULL, NULL, 0);
    struct ibv_mr *mr = pd ? ibv_reg_mr(pd, buffer, options.size,
                                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE) : NULL;
    struct ibv_qp_init_attr init = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
    };
    struct ibv_qp *qp = mr && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!qp) {
        perror("allocate verbs resources");
        goto cleanup_resources;
    }
    local.qpn = qp->qp_num;
    local.region = (struct roce_region) {
        .address = (uintptr_t)buffer, .rkey = mr->rkey, .size = options.size,
    };
    if (!roce_lab_random(&local.psn))
        goto cleanup_resources;
    local.psn &= ROCE_PSN_MASK;

    int fd = roce_lab_open(&options);
    if (fd < 0) {
        perror("control socket");
        goto cleanup_resources;
    }
    if (!roce_lab_exchange(fd, &options, &local, &peer) ||
        !connect_qp(qp, (uint8_t)port, (uint8_t)gid_index, &local, &peer) || !roce_lab_ready(fd)) {
        fprintf(stderr, "control setup failed\n");
    } else if (options.sender) {
        if (write_and_wait(qp, cq, mr, &peer.region) && roce_lab_send(fd, 'D') &&
            roce_lab_receive(fd, 'V', false) == 1) {
            printf("WRITE complete: %u bytes, native verbs completion, peer verified\n", options.size);
            result = 0;
        }
    } else if (roce_lab_receive(fd, 'D', false) == 1 && roce_lab_verify(buffer, options.size) &&
               roce_lab_send(fd, 'V')) {
        printf("WRITE verified: %u bytes in native registered memory\n", options.size);
        result = 0;
    }
    close(fd);
cleanup_resources:
    if (qp)
        ibv_destroy_qp(qp);
    if (mr)
        ibv_dereg_mr(mr);
    if (cq)
        ibv_destroy_cq(cq);
    if (pd)
        ibv_dealloc_pd(pd);
    free(buffer);
cleanup_device:
    ibv_close_device(context);
    return result;
}
