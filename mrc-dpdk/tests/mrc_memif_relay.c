#include "dpdk_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

enum { PORTS = 2, BURST = 32, FRAME_SIZE = 1514, TERMINAL = 0x80 };
static bool quiesced, quiesce_ack_pending;
static uint64_t quiesce_deadline;
static void discard_pending(void);

static bool socket_address(struct sockaddr_un *address, const char *path)
{
    size_t size = strlen(path) + 1;
    if (size > sizeof(address->sun_path))
        return false;
    *address = (struct sockaddr_un){ .sun_family = AF_UNIX };
    memcpy(address->sun_path, path, size);
    return true;
}

/* A datagram contains a one-byte port index followed by an Ethernet frame.
 * Bit 7 marks terminal feedback: enqueue it, cease RX polling, and retain
 * ports until the fixture observes client exit and sends an empty stop.
 * A one-byte 0x80 response confirms quiescence. No further frames are allowed. */
static int poll_fixture(int fd, struct rte_mempool *pool)
{
    uint8_t bytes[1 + FRAME_SIZE];
    for (unsigned int n = 0; n < BURST; n++) {
        ssize_t length = recv(fd, bytes, sizeof(bytes), MSG_TRUNC);
        if (!length)
            return 1;
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return 0;
            return -errno;
        }
        if (quiesced)
            return -EPROTO;
        uint8_t port = bytes[0] & ~TERMINAL;
        bool terminal = (bytes[0] & TERMINAL) != 0;
        if (length <= 1 || length > (ssize_t)sizeof(bytes) || port >= PORTS)
            return -EBADMSG;

        struct rte_mbuf *packet = rte_pktmbuf_alloc(pool);
        if (!packet)
            return -ENOMEM;
        char *frame = rte_pktmbuf_append(packet, (uint16_t)(length - 1));
        if (!frame) {
            rte_pktmbuf_free(packet);
            return -EMSGSIZE;
        }
        memcpy(frame, bytes + 1, (size_t)length - 1);
        if (terminal) {
            /* Release already-polled packets before the final feedback can
             * cause the client to disconnect and unmap shared rings. */
            discard_pending();
            quiesced = true;
            quiesce_deadline = rte_get_timer_cycles() + 5 * rte_get_timer_hz();
        }
        if (!mrc_dpdk_transmit(port, packet))
            return -EIO;
        if (terminal)
            quiesce_ack_pending = true;
    }
    return 0;
}

static int deliver_frame(int fd, uint8_t port, const struct rte_mbuf *packet)
{
    size_t length = rte_pktmbuf_pkt_len(packet);
    uint8_t bytes[1 + FRAME_SIZE];
    bytes[0] = port;
    if (!length || length > FRAME_SIZE)
        return -EMSGSIZE;
    const void *frame = rte_pktmbuf_read(packet, 0, (uint32_t)length, bytes + 1);
    if (!frame)
        return -EBADMSG;
    if (frame != bytes + 1)
        memcpy(bytes + 1, frame, length);
    ssize_t sent;
    do {
        sent = send(fd, bytes, length + 1, 0);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0)
        return -errno;
    return sent == (ssize_t)(length + 1) ? 0 : -EIO;
}

/* Keep one burst while the Python fixture drains its datagram queue.
 * Continue polling fixture input between attempts to avoid a bidirectional
 * socket-buffer deadlock. A stalled fixture fails within three seconds. */
static struct rte_mbuf *pending[BURST];
static uint16_t pending_count, pending_next;
static uint8_t pending_port;
static uint64_t pending_deadline;

static void discard_pending(void)
{
    while (pending_next < pending_count)
        rte_pktmbuf_free(pending[pending_next++]);
    pending_count = pending_next = 0;
}

static int poll_quiesced(int fd)
{
    if (rte_get_timer_cycles() >= quiesce_deadline) {
        fprintf(stderr, "memif relay quiesce timed out waiting for stop\n");
        return -ETIMEDOUT;
    }
    if (quiesce_ack_pending) {
        const uint8_t acknowledgement = TERMINAL;
        ssize_t sent = send(fd, &acknowledgement, 1, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return 0;
            return -errno;
        }
        if (sent != 1)
            return -EIO;
        quiesce_ack_pending = false;
    }
    return 0;
}

static int flush_pending(int fd)
{
    while (pending_next < pending_count) {
        int result = deliver_frame(fd, pending_port, pending[pending_next]);
        if (result == -EAGAIN || result == -EWOULDBLOCK)
            return rte_get_timer_cycles() >= pending_deadline ? -ETIMEDOUT : 1;
        if (result)
            return result;
        rte_pktmbuf_free(pending[pending_next++]);
    }
    pending_count = pending_next = 0;
    return 0;
}

static int poll_ports(int fd)
{
    int result = flush_pending(fd);
    if (result)
        return result > 0 ? 0 : result;
    for (uint8_t port = 0; port < PORTS; port++) {
        pending_count = rte_eth_rx_burst(port, 0, pending, BURST);
        pending_port = port;
        pending_deadline = rte_get_timer_cycles() + 3 * rte_get_timer_hz();
        result = flush_pending(fd);
        if (result)
            return result > 0 ? 0 : result;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return EXIT_FAILURE;

    int result = -EINVAL;
    unsigned int opened = 0;
    bool bound = false;
    int fd = -1;
    struct rte_mempool *pool = NULL;
    struct sockaddr_un local, peer;
    if (argc - consumed != 3 || rte_eth_dev_count_avail() != PORTS ||
        !socket_address(&local, argv[consumed + 1]) ||
        !socket_address(&peer, argv[consumed + 2])) {
        fprintf(stderr, "Usage: %s [two EAL ports] -- RELAY_SOCKET FIXTURE_SOCKET\n", argv[0]);
        goto done;
    }
    pool = mrc_dpdk_packet_pool();
    if (!pool) {
        result = -ENOMEM;
        goto done;
    }
    for (; opened < PORTS; opened++) {
        struct rte_ether_addr mac;
        result = mrc_dpdk_open_port((uint16_t)opened, pool, &mac);
        if (result)
            goto done;
    }
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || connect(fd, (const struct sockaddr *)&peer, sizeof(peer)) < 0 ||
        bind(fd, (const struct sockaddr *)&local, sizeof(local)) < 0) {
        result = -errno;
        goto done;
    }
    bound = true;
    do {
        result = poll_fixture(fd, pool);
        if (!result)
            result = quiesced ? poll_quiesced(fd) : poll_ports(fd);
        rte_pause();
    } while (!result);
    if (result == 1)
        result = 0;

done:
    discard_pending();
    if (result)
        fprintf(stderr, "memif relay failed: %s\n", strerror(-result));
    if (fd >= 0)
        close(fd);
    if (bound)
        unlink(local.sun_path);
    for (unsigned int i = 0; i < opened; i++)
        mrc_dpdk_close_port((uint16_t)i);
    if (pool)
        rte_mempool_free(pool);
    rte_eal_cleanup();
    return result ? EXIT_FAILURE : EXIT_SUCCESS;
}
