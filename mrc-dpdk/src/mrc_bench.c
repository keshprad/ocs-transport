#include "dpdk_io.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

enum {
    PATHS = 2,
    PAYLOAD = 1400,
    WINDOW = 1024,
    ACK_BITS = 64,
    BURST = 32,
    ETHER_TYPE = 0x88b5,
    VERSION = 1,
    DATA = 1,
    ACK = 2,
};

struct wire_header {
    uint8_t dst[RTE_ETHER_ADDR_LEN];
    uint8_t src[RTE_ETHER_ADDR_LEN];
    rte_be16_t ether_type;
    uint8_t version;
    uint8_t kind;
    rte_be16_t length;
    rte_be32_t id;
    rte_be32_t seq;
    rte_be64_t ack;
} __attribute__((packed));

_Static_assert(sizeof(struct wire_header) == 34, "unexpected wire header size");

struct path {
    uint16_t port;
    struct rte_ether_addr local;
    struct rte_ether_addr peer;
};

struct bench {
    struct path paths[PATHS];
    struct rte_mempool *pool;
    FILE *trace;
    uint32_t id;
    uint32_t bytes;
    uint32_t packets;
    uint32_t drop_after;
    uint64_t hz;
    uint64_t rto;
    uint64_t timeout;
};

struct counter {
    uint32_t first_tx[PATHS];
    uint32_t retries[PATHS];
    uint32_t received[PATHS];
    uint32_t dropped;
    uint32_t switched;
    uint32_t duplicates;
    uint32_t reordered;
    uint32_t reorder_max;
};

struct slot {
    uint64_t sent_at;
    unsigned int path;
    bool acked;
};

static void trace_packet(const struct bench *bench, const char *event,
                         uint32_t seq, unsigned int path)
{
    if (bench->trace)
        fprintf(bench->trace, "%s,%" PRIu32 ",%u\n", event, seq, path);
}

static uint16_t data_length(const struct bench *bench, uint32_t seq)
{
    uint32_t remaining = bench->bytes - seq * PAYLOAD;
    return remaining < PAYLOAD ? (uint16_t)remaining : PAYLOAD;
}

static bool send_frame(const struct bench *bench, unsigned int index, uint8_t kind,
                       uint32_t seq, uint64_t ack, const uint8_t *payload)
{
    const struct path *path = &bench->paths[index];
    uint16_t length = kind == DATA ? data_length(bench, seq) : 0;
    struct rte_mbuf *packet = rte_pktmbuf_alloc(bench->pool);
    if (!packet)
        return false;

    struct wire_header *header =
        (void *)rte_pktmbuf_append(packet, sizeof(*header) + length);
    if (!header) {
        rte_pktmbuf_free(packet);
        return false;
    }

    *header = (struct wire_header){
        .ether_type = rte_cpu_to_be_16(ETHER_TYPE),
        .version = VERSION, .kind = kind, .length = rte_cpu_to_be_16(length),
        .id = rte_cpu_to_be_32(bench->id), .seq = rte_cpu_to_be_32(seq),
        .ack = rte_cpu_to_be_64(ack),
    };
    memcpy(header->dst, path->peer.addr_bytes, sizeof(header->dst));
    memcpy(header->src, path->local.addr_bytes, sizeof(header->src));
    if (length)
        memcpy(header + 1, payload + (size_t)seq * PAYLOAD, length);

    return mrc_dpdk_transmit(path->port, packet);
}

static const struct wire_header *read_header(const struct bench *bench,
        unsigned int index, const struct rte_mbuf *packet, uint8_t kind,
        struct wire_header *scratch)
{
    const struct path *path = &bench->paths[index];
    const struct wire_header *header = rte_pktmbuf_read(packet, 0, sizeof(*header),
                                                       scratch);
    if (!header || header->kind != kind || header->version != VERSION ||
        rte_be_to_cpu_16(header->ether_type) != ETHER_TYPE ||
        rte_be_to_cpu_32(header->id) != bench->id ||
        memcmp(header->dst, path->local.addr_bytes, sizeof(header->dst)) ||
        memcmp(header->src, path->peer.addr_bytes, sizeof(header->src)))
        return NULL;

    return header;
}

static uint8_t payload_byte(uint32_t offset)
{
    return (uint8_t)(offset ^ (offset >> 8) ^ (offset >> 16) ^ (offset >> 24));
}

static bool send_data(const struct bench *bench, struct counter *counter,
                      struct slot *slot, const uint8_t *data, unsigned int path,
                      uint32_t seq, uint32_t next, bool retry, uint64_t now)
{
    bool drop = path == 1 && next >= bench->drop_after;
    if (!drop && !send_frame(bench, path, DATA, seq, 0, data))
        return false;

    trace_packet(bench, retry ? "retry" : "send", seq, path);
    if (drop) {
        trace_packet(bench, "drop", seq, path);
        counter->dropped++;
    }
    if (retry) {
        counter->retries[path]++;
        counter->switched += path != slot->path;
    } else {
        counter->first_tx[path]++;
    }
    *slot = (struct slot){ .sent_at = now, .path = path };
    return true;
}

static unsigned int healthy_path(const uint64_t unavailable[PATHS],
                                 unsigned int preferred, uint64_t now)
{
    unsigned int other = 1 - preferred;
    if (now < unavailable[preferred] && now >= unavailable[other])
        return other;
    return preferred;
}

static void apply_ack(const struct bench *bench, unsigned int path,
                      const struct rte_mbuf *packet, struct slot slots[WINDOW],
                      uint32_t base, uint32_t next, uint32_t *acked)
{
    struct wire_header scratch;
    const struct wire_header *header = read_header(bench, path, packet, ACK, &scratch);
    if (!header || header->length)
        return;

    uint32_t start = rte_be_to_cpu_32(header->seq);
    if (start % ACK_BITS != 0 || start >= next)
        return;

    uint64_t bits = rte_be_to_cpu_64(header->ack);
    while (bits) {
        uint32_t seq = start + (uint32_t)__builtin_ctzll(bits);
        bits &= bits - 1;
        if (seq < base || seq >= next || slots[seq % WINDOW].acked)
            continue;
        slots[seq % WINDOW].acked = true;
        (*acked)++;
    }
}

static int send_transfer(const struct bench *bench, const uint8_t *data,
                         struct counter *counter, double *seconds)
{
    struct slot slots[WINDOW] = {0};
    uint64_t unavailable[PATHS] = {0}, blocked_since[PATHS] = {0};
    uint32_t base = 0, next = 0, acked = 0;
    uint64_t start = rte_get_timer_cycles(), last_progress = start, next_retry = start;

    while (acked < bench->packets) {
        uint32_t previous_acked = acked;
        for (unsigned int path = 0; path < PATHS; path++) {
            struct rte_mbuf *packets[BURST];
            uint16_t count = rte_eth_rx_burst(bench->paths[path].port, 0, packets, BURST);
            for (uint16_t i = 0; i < count; i++) {
                apply_ack(bench, path, packets[i], slots, base, next, &acked);
                rte_pktmbuf_free(packets[i]);
            }
        }
        while (base < next && slots[base % WINDOW].acked)
            base++;

        uint64_t now = rte_get_timer_cycles();
        if (acked != previous_acked)
            last_progress = now;
        if (acked == bench->packets) {
            *seconds = (double)(now - start) / (double)bench->hz;
            return 0;
        }
        if (now - last_progress >= bench->timeout) {
            fprintf(stderr, "Sender timed out with %" PRIu32 "/%" PRIu32 " packets acknowledged\n",
                    acked, bench->packets);
            return -1;
        }

        if (now >= next_retry) {
            /* Mark all expired paths before scheduling retries. */
            for (uint32_t seq = base; seq < next; seq++) {
                const struct slot *slot = &slots[seq % WINDOW];
                if (!slot->acked && now - slot->sent_at >= bench->rto)
                    unavailable[slot->path] = now + 10 * bench->rto;
            }
            for (uint32_t seq = base; seq < next; seq++) {
                struct slot *slot = &slots[seq % WINDOW];
                if (slot->acked || now - slot->sent_at < bench->rto)
                    continue;
                unsigned int path = healthy_path(unavailable, 1 - slot->path, now);
                send_data(bench, counter, slot, data, path, seq, next, true, now);
            }
            next_retry = now + bench->rto / 8;
        }

        uint32_t burst_end = next + BURST;
        while (next < bench->packets && next - base < WINDOW && next < burst_end) {
            unsigned int path = healthy_path(unavailable, next % PATHS, now);
            if (!send_data(bench, counter, &slots[next % WINDOW], data,
                           path, next, next, false, now)) {
                if (!blocked_since[path])
                    blocked_since[path] = now;
                if (now - blocked_since[path] >= bench->rto)
                    unavailable[path] = now + 10 * bench->rto;
                break;
            }
            blocked_since[path] = 0;
            next++;
        }
        rte_pause();
    }
    return -1;
}

static bool receive_data(const struct bench *bench, struct counter *counter,
                         uint64_t *received, uint8_t *data, uint32_t *highest,
                         uint32_t *unique, unsigned int path, const struct rte_mbuf *packet,
                         uint32_t *ack_block)
{
    struct wire_header scratch;
    const struct wire_header *header = read_header(bench, path, packet, DATA, &scratch);
    if (!header)
        return false;

    uint32_t seq = rte_be_to_cpu_32(header->seq);
    if (seq >= bench->packets || rte_be_to_cpu_16(header->length) != data_length(bench, seq))
        return false;

    uint16_t length = data_length(bench, seq);
    uint8_t *destination = data + (size_t)seq * PAYLOAD;
    uint8_t payload[PAYLOAD];
    const uint8_t *source = rte_pktmbuf_read(packet, sizeof(*header), length, payload);
    if (!source)
        return false;

    trace_packet(bench, "receive", seq, path);
    uint32_t block = seq / ACK_BITS;
    uint64_t bit = UINT64_C(1) << (seq % ACK_BITS);
    if (received[block] & bit) {
        counter->duplicates++;
    } else {
        memcpy(destination, source, length);
        received[block] |= bit;
        counter->received[path]++;
        if (*unique && seq < *highest) {
            uint32_t distance = *highest - seq;
            if (distance > counter->reorder_max)
                counter->reorder_max = distance;
            counter->reordered++;
        } else {
            *highest = seq;
        }
        (*unique)++;
    }

    *ack_block = block;
    return true;
}

static int receive_transfer(const struct bench *bench, uint8_t *data,
                            struct counter *counter)
{
    uint64_t start = rte_get_timer_cycles(), last_progress = start, last_data = start;
    uint32_t unique = 0, highest = 0;
    uint64_t *received = calloc((bench->packets + ACK_BITS - 1) / ACK_BITS, sizeof(*received));
    if (!received) {
        fprintf(stderr, "Could not allocate receive bitmap\n");
        return -1;
    }

    int result = 0;
    for (;;) {
        uint32_t previous_unique = unique;
        bool saw_data = false;
        for (unsigned int path = 0; path < PATHS; path++) {
            struct rte_mbuf *packets[BURST];
            uint32_t ack_blocks[BURST];
            uint16_t ack_count = 0;
            uint16_t count = rte_eth_rx_burst(bench->paths[path].port, 0, packets, BURST);
            for (uint16_t i = 0; i < count; i++) {
                uint32_t block;
                if (receive_data(bench, counter, received, data, &highest, &unique,
                                 path, packets[i], &block)) {
                    saw_data = true;
                    bool queued = false;
                    for (uint16_t j = 0; j < ack_count; j++)
                        queued |= ack_blocks[j] == block;
                    if (!queued)
                        ack_blocks[ack_count++] = block;
                }
                rte_pktmbuf_free(packets[i]);
            }
            for (uint16_t i = 0; i < ack_count; i++) {
                uint32_t block = ack_blocks[i];
                send_frame(bench, path, ACK, block * ACK_BITS, received[block], NULL);
            }
        }

        uint64_t now = rte_get_timer_cycles();
        if (saw_data)
            last_data = now;
        if (unique != previous_unique)
            last_progress = now;
        if (unique == bench->packets) {
            /* Keep acknowledging duplicates while the sender can still retry. */
            uint64_t linger = 8 * bench->rto + bench->hz / 4;
            if (now - last_data >= linger)
                break;
        } else if (now - last_progress >= bench->timeout) {
            fprintf(stderr, "Receiver timed out with %" PRIu32 "/%" PRIu32 " packets received\n",
                    unique, bench->packets);
            result = -1;
            break;
        }
        rte_pause();
    }
    free(received);
    return result;
}

static bool number(const char *text, uint32_t lower, uint32_t upper, uint32_t *value)
{
    if (*text < '0' || *text > '9')
        return false;
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed < lower || parsed > upper)
        return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_args(int argc, char **argv, struct bench *bench, bool *sender,
                       const char **trace_path)
{
    if (argc < 8 || (strcmp(argv[1], "tx") && strcmp(argv[1], "rx")) ||
        !number(argv[2], 1, 1U << 30, &bench->bytes) ||
        !number(argv[3], 1, UINT32_MAX, &bench->id))
        return false;

    *sender = strcmp(argv[1], "tx") == 0;
    bench->packets = (bench->bytes + PAYLOAD - 1) / PAYLOAD;
    for (unsigned int i = 0; i < PATHS; i++) {
        uint32_t port;
        if (!number(argv[4 + 2 * i], 0, UINT16_MAX, &port) ||
            rte_ether_unformat_addr(argv[5 + 2 * i], &bench->paths[i].peer))
            return false;
        bench->paths[i].port = (uint16_t)port;
    }
    if (bench->paths[0].port == bench->paths[1].port)
        return false;

    uint32_t rto_us = 10000, timeout_ms = 30000;
    bench->drop_after = UINT32_MAX;
    for (int i = 8; i + 1 < argc; i += 2) {
        const char *option = argv[i], *value = argv[i + 1];
        if (!strcmp(option, "--trace"))
            *trace_path = value;
        else if (!strcmp(option, "--rto-us") && number(value, 100, 1000000, &rto_us))
            continue;
        else if (!strcmp(option, "--timeout-ms") && number(value, 1, 300000, &timeout_ms))
            continue;
        else if (*sender && !strcmp(option, "--drop-path-after") &&
                 number(value, 0, bench->packets - 1, &bench->drop_after))
            continue;
        else
            return false;
    }
    bench->hz = rte_get_timer_hz();
    bench->rto = bench->hz * rto_us / 1000000;
    bench->timeout = bench->hz * timeout_ms / 1000;
    return argc % 2 == 0 && bench->timeout > bench->rto;
}

int main(int argc, char **argv)
{
    int consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return EXIT_FAILURE;

    struct bench bench = {0};
    struct counter counter = {0};
    bool sender;
    const char *trace_path = NULL;
    int result = EXIT_FAILURE;
    unsigned int started = 0;
    uint8_t *data = NULL;
    if (!parse_args(argc - consumed, argv + consumed, &bench, &sender, &trace_path)) {
        fprintf(stderr, "Usage: %s [EAL options] -- tx|rx BYTES ID PORT0 PEER_MAC0 PORT1 PEER_MAC1\n"
                        "       [--trace CSV] [--rto-us N] [--timeout-ms N] [--drop-path-after SEQ (tx)]\n",
                argv[0]);
        goto done;
    }
    if (trace_path) {
        bench.trace = fopen(trace_path, "w");
        if (!bench.trace) {
            perror(trace_path);
            goto done;
        }
        fputs("event,seq,path\n", bench.trace);
    }
    data = malloc(bench.bytes);
    if (!data) {
        perror("transfer buffer");
        goto done;
    }
    if (sender) {
        for (uint32_t i = 0; i < bench.bytes; i++)
            data[i] = payload_byte(i);
    }

    bench.pool = mrc_dpdk_packet_pool();
    if (!bench.pool) {
        fprintf(stderr, "Could not allocate DPDK packet pool\n");
        goto done;
    }
    for (; started < PATHS; started++) {
        struct path *path = &bench.paths[started];
        int error = mrc_dpdk_open_port(path->port, bench.pool, &path->local);
        if (error) {
            fprintf(stderr, "Could not start port %u: %s\n", bench.paths[started].port,
                    strerror(-error));
            goto done;
        }
    }

    double seconds = 0;
    if (sender) {
        if (send_transfer(&bench, data, &counter, &seconds))
            goto done;
    } else {
        if (receive_transfer(&bench, data, &counter))
            goto done;
        for (uint32_t i = 0; i < bench.bytes; i++) {
            if (data[i] != payload_byte(i)) {
                fprintf(stderr, "Invalid payload at offset %" PRIu32 "\n", i);
                goto done;
            }
        }
    }
    if (bench.trace && fflush(bench.trace)) {
        perror("packet trace");
        goto done;
    }

    printf("{\"role\":\"%s\",\"bytes\":%" PRIu32 ",\"packets\":%" PRIu32
           ",\"seconds\":%.6f,\"goodput_gbps\":%.6f"
           ",\"first_tx\":[%u,%u],\"retries\":[%u,%u],\"dropped\":%u,\"switched\":%u"
           ",\"received\":[%u,%u],\"duplicates\":%u,\"reordered\":%u,\"reorder_max\":%u}\n",
           sender ? "tx" : "rx", bench.bytes, bench.packets, seconds,
           seconds > 0 ? 8.0 * bench.bytes / seconds / 1e9 : 0.0,
           counter.first_tx[0], counter.first_tx[1], counter.retries[0], counter.retries[1],
           counter.dropped, counter.switched, counter.received[0], counter.received[1],
           counter.duplicates, counter.reordered, counter.reorder_max);
    result = EXIT_SUCCESS;

done:
    for (unsigned int i = 0; i < started; i++)
        mrc_dpdk_close_port(bench.paths[i].port);
    if (bench.pool)
        rte_mempool_free(bench.pool);
    free(data);
    if (bench.trace)
        fclose(bench.trace);
    rte_eal_cleanup();
    return result;
}
