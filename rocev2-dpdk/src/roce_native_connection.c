#define _GNU_SOURCE
#include "roce_native_connection.h"
#include "roce_native_queue.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

struct ibv_context *roce_native_open_device(void)
{
    struct ibv_device **devices = ibv_get_device_list(NULL);
    if (!devices)
        return NULL;
    struct ibv_context *context = NULL;
    int error = ENODEV;
    for (size_t i = 0; devices[i]; ++i) {
        if (strcmp(ibv_get_device_name(devices[i]), "roce_dpdk0") == 0) {
            context = ibv_open_device(devices[i]);
            error = errno;
            break;
        }
    }
    ibv_free_device_list(devices);
    if (!context)
        errno = error;
    return context;
}

int roce_native_init_qp(struct ibv_qp *qp, bool receiver)
{
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT, .port_num = 1,
        .qp_access_flags = receiver ? IBV_ACCESS_REMOTE_WRITE : 0,
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static enum ibv_mtu verbs_mtu(uint32_t pmtu)
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

static bool valid_config(const struct roce_native_queue_config *config, bool receiver)
{
    if (config->version != ROCE_NATIVE_QUEUE_VERSION || !config->qps ||
        config->qps > ROCE_LAB_MAX_QPS || !config->workers || config->workers > config->qps ||
        !config->writes || config->writes > ROCE_PSN_MASK || !config->write_size ||
        config->flags != (receiver ? ROCE_NATIVE_QUEUE_RECEIVE_ONLY : 0) || !verbs_mtu(config->pmtu) ||
        config->local_qpn < 2 || config->local_qpn > ROCE_PSN_MASK - config->qps ||
        config->peer_qpn < 2 || config->peer_qpn > ROCE_PSN_MASK - config->qps ||
        config->local_psn > ROCE_PSN_MASK || config->peer_psn > ROCE_PSN_MASK ||
        !config->local_lkey || !config->local_ipv4 || !config->peer_ipv4)
        return false;
    if (receiver ? config->qps != 1 : !config->remote_rkey)
        return false;

    uint64_t bytes = (uint64_t)config->qps * config->writes * config->write_size;
    return bytes - 1 <= UINT64_MAX - config->local_address &&
           bytes - 1 <= UINT64_MAX - config->remote_address;
}

const struct roce_native_work *roce_native_map_work(int fd, bool receiver,
                                                     struct roce_native_queue_config *config)
{
    const int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    int seals = fcntl(fd, F_GET_SEALS);
    struct stat status;
    if (seals < 0 || fstat(fd, &status))
        return MAP_FAILED;
    if ((seals & required_seals) != required_seals || !S_ISREG(status.st_mode) ||
        status.st_size != (off_t)sizeof(struct roce_native_work)) {
        errno = EPROTO;
        return MAP_FAILED;
    }

    const struct roce_native_work *work = mmap(NULL, sizeof(*work), PROT_READ, MAP_SHARED, fd, 0);
    if (work == MAP_FAILED)
        return MAP_FAILED;
    *config = work->config;
    if (valid_config(config, receiver)) {
        uint32_t i = 0;
        for (; i < config->qps; ++i) {
            const struct roce_native_sq *sq = &work->qps[i];
            if (!atomic_is_lock_free(&sq->admitted) ||
                atomic_load_explicit(&sq->posted, memory_order_acquire) ||
                atomic_load_explicit(&sq->admitted, memory_order_acquire) ||
                atomic_load_explicit(&sq->retired, memory_order_acquire))
                break;
        }
        if (i == config->qps)
            return work;
    }
    munmap((void *)work, sizeof(*work));
    errno = EPROTO;
    return MAP_FAILED;
}

static union ibv_gid peer_gid(uint32_t ipv4)
{
    union ibv_gid gid = { .raw = { [10] = 0xff, [11] = 0xff } };
    uint32_t network = htonl(ipv4);
    memcpy(gid.raw + 12, &network, sizeof(network));
    return gid;
}

int roce_native_connect_qp(struct ibv_qp *qp, const struct roce_native_queue_config *config,
                             uint32_t index)
{
    if (qp->qp_num != config->local_qpn + index)
        return EPROTO;
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR, .path_mtu = verbs_mtu(config->pmtu),
        .dest_qp_num = config->peer_qpn + index,
        .rq_psn = roce_native_psn(config->peer_psn, index), .min_rnr_timer = 12,
        .ah_attr = {
            .port_num = 1, .is_global = 1,
            .grh = { .dgid = peer_gid(config->peer_ipv4), .hop_limit = 64 },
        },
    };
    int error = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                              IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                              IBV_QP_MIN_RNR_TIMER);
    if (error)
        return error;
    attr = (struct ibv_qp_attr) {
        .qp_state = IBV_QPS_RTS, .sq_psn = roce_native_psn(config->local_psn, index),
        .timeout = 14, .retry_cnt = ROCE_LAB_RETRIES,
    };
    return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                         IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

int roce_native_bind_work(struct ibv_context *context, int queue_fd)
{
    void *provider = dlopen("libroce_dpdk-rdmav34.so", RTLD_NOLOAD | RTLD_NOW);
    if (!provider)
        return ENOTSUP;
    typedef int (*bind_queues_fn)(struct ibv_context *, int);
    bind_queues_fn bind_queues = (bind_queues_fn)dlsym(provider, "roce_dpdk_bind_queues");
    int error = bind_queues ? bind_queues(context, queue_fd) : ENOTSUP;
    if (dlclose(provider) && !error)
        error = EIO;
    return error;
}

int roce_native_configure_qp(struct ibv_qp *qp, int queue_fd, bool receiver)
{
    if (!qp || qp->state != IBV_QPS_INIT)
        return EINVAL;
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init;
    int error = ibv_query_qp(qp, &attr, IBV_QP_ACCESS_FLAGS, &init);
    if (error)
        return error;
    if (!!(attr.qp_access_flags & IBV_ACCESS_REMOTE_WRITE) != receiver)
        return EINVAL;
    struct roce_native_queue_config config;
    const struct roce_native_work *work = roce_native_map_work(queue_fd, receiver, &config);
    if (work == MAP_FAILED)
        return errno;
    if (munmap((void *)work, sizeof(*work)))
        return errno;
    if (config.qps != 1 || qp->qp_num != config.local_qpn)
        return EPROTO;
    error = roce_native_bind_work(qp->context, queue_fd);
    return error ? error : roce_native_connect_qp(qp, &config, 0);
}
