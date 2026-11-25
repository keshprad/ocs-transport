/* Test-only memif lifecycle barriers. Fault injection starts only when all
 * requested rails are ready; both datapaths stop before clients disconnect,
 * and clients disconnect before servers release
 * shared rings. The production CLI keeps its asynchronous behavior. */
#include "mrc_endpoint.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>

static bool fixture_initialized, fixture_requester;
static unsigned int fixture_ports, closed_ports;
static uint64_t fixture_wait_cycles;
static char closed_marker[PATH_MAX], stopped_marker[PATH_MAX];

static void configure_teardown(const struct mrc_endpoint_config *transfer)
{
    const char *directory = getenv("XDG_RUNTIME_DIR");
    if (!directory || snprintf(closed_marker, sizeof(closed_marker),
                               "%s/mrc-write-client-ports-closed", directory) >= (int)sizeof(closed_marker) ||
        snprintf(stopped_marker, sizeof(stopped_marker), "%s/mrc-write-server-stopped", directory) >=
            (int)sizeof(stopped_marker)) {
        fprintf(stderr, "TEST memif teardown requires a private XDG_RUNTIME_DIR\n");
        exit(EXIT_FAILURE);
    }
    fixture_initialized = false;
    fixture_requester = transfer->sender;
    fixture_ports = mrc_endpoint_path_count(transfer);
    /* The benchmark responder intentionally polls for 8 RTOs + 250 ms after
     * delivery. Keep shared rings alive through that grace period, or through
     * its configured idle timeout on an error path, plus one second slack. */
    uint64_t drain = 8 * transfer->initial_rto + transfer->hz / 4;
    fixture_wait_cycles = (drain > transfer->idle_timeout ? drain : transfer->idle_timeout) + transfer->hz;
}

int __real_mrc_responder_create(const struct mrc_endpoint_config *transfer,
                                struct mrc_provider_qp *qp,
                                struct mrc_endpoint_counters *counts,
                                struct mrc_responder **responder);

int __wrap_mrc_responder_create(const struct mrc_endpoint_config *transfer,
                                struct mrc_provider_qp *qp,
                                struct mrc_endpoint_counters *counts,
                                struct mrc_responder **responder)
{
    configure_teardown(transfer);
    int result = __real_mrc_responder_create(transfer, qp, counts, responder);
    fixture_initialized = result == 0;
    return result;
}

static void publish_marker(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 || close(fd)) {
        fprintf(stderr, "TEST memif teardown could not publish marker: %s (errno %d)\n", path, errno);
        exit(EXIT_FAILURE);
    }
}

static void wait_marker(const char *path, uint16_t port)
{
    const uint64_t deadline = rte_get_timer_cycles() + fixture_wait_cycles;
    const struct timespec interval = { .tv_nsec = 1000000 };
    while (access(path, F_OK)) {
        if (errno != ENOENT || rte_get_timer_cycles() >= deadline) {
            fprintf(stderr, "TEST memif teardown timeout/error before port %u: "
                            "marker %s (errno %d)\n", port, path, errno);
            exit(EXIT_FAILURE);
        }
        nanosleep(&interval, NULL);
    }
}

void __real_mrc_dpdk_close_port(uint16_t port);
void __wrap_mrc_dpdk_close_port(uint16_t port)
{
    if (fixture_initialized && !closed_ports) {
        if (fixture_requester) {
            wait_marker(stopped_marker, port);
            fprintf(stderr, "TEST memif teardown: server datapath stopped before client closure\n");
        } else {
            /* Reached only after run_receiver and responder destruction.
             * Its memif RX rings can no longer be polled by the datapath. */
            publish_marker(stopped_marker);
            wait_marker(closed_marker, port);
            fprintf(stderr, "TEST memif teardown: client ports closed before server closure\n");
        }
    }
    __real_mrc_dpdk_close_port(port);
    if (fixture_initialized && ++closed_ports == fixture_ports && fixture_requester) {
        publish_marker(closed_marker);
        fprintf(stderr, "TEST memif teardown: client closed all %u ports\n", fixture_ports);
    }
}

int __real_mrc_requester_create(const struct mrc_endpoint_config *transfer,
                                struct mrc_provider_qp *qp,
                                struct mrc_endpoint_counters *counts,
                                struct mrc_requester **requester);

int __wrap_mrc_requester_create(const struct mrc_endpoint_config *transfer,
                                struct mrc_provider_qp *qp,
                                struct mrc_endpoint_counters *counts,
                                struct mrc_requester **requester)
{
    configure_teardown(transfer);
    const unsigned int count = mrc_endpoint_path_count(transfer);
    const uint64_t deadline = rte_get_timer_cycles() + 3 * rte_get_timer_hz();
    const struct timespec interval = { .tv_nsec = 1000000 };
    for (;;) {
        unsigned int missing = count;
        for (unsigned int path = 0; path < count; path++) {
            struct rte_eth_link link = {0};
            uint16_t port = transfer->paths[path].port;
            int result = rte_eth_link_get_nowait(port, &link);
            if (result) {
                fprintf(stderr, "TEST requester readiness query failed: path %u port %u error %d\n",
                        path, port, result);
                return result;
            }
            if (link.link_status != RTE_ETH_LINK_UP && missing == count)
                missing = path;
        }
        if (missing == count) {
            int result = __real_mrc_requester_create(transfer, qp, counts, requester);
            fixture_initialized = result == 0;
            return result;
        }
        if (rte_get_timer_cycles() >= deadline) {
            fprintf(stderr, "TEST requester readiness timeout after 3000 ms: path %u port %u DOWN\n",
                    missing, transfer->paths[missing].port);
            return -ETIMEDOUT;
        }
        nanosleep(&interval, NULL);
    }
}
