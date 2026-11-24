#ifndef ROCE_WIRE_H
#define ROCE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ROCE_UDP_PORT = 4791,
    ROCE_PKEY = 0xffff,
    ROCE_PSN_MASK = 0xffffff,
    ROCE_MAX_PMTU = 4096,
    ROCE_MAX_HEADER = 14 + 20 + 8 + 12 + 16,
    ROCE_MAX_TRAILER = 3 + 4,
    ROCE_MAX_FRAME = ROCE_MAX_HEADER + ROCE_MAX_PMTU + 4,
    ROCE_MAC_LEN = 6,
    ROCE_CM_MAD_SIZE = 256,
    ROCE_CM_FRAME_SIZE = 14 + 20 + 8 + 12 + 8 + ROCE_CM_MAD_SIZE + 4,
};

enum roce_opcode {
    ROCE_WRITE_FIRST = 0x06,
    ROCE_WRITE_MIDDLE = 0x07,
    ROCE_WRITE_LAST = 0x08,
    ROCE_WRITE_ONLY = 0x0a,
    ROCE_ACK = 0x11,
    ROCE_CM_SEND_ONLY = 0x64,
};

enum roce_syndrome {
    ROCE_ACK_NO_CREDIT = 0x1f,
    ROCE_NAK_SEQUENCE = 0x60,
    ROCE_NAK_INVALID = 0x61,
    ROCE_NAK_ACCESS = 0x62,
    ROCE_NAK_OPERATION = 0x63,
};

struct roce_address {
    uint8_t mac[ROCE_MAC_LEN];
    uint32_t ipv4; /* Host byte order. */
};

struct roce_packet {
    struct roce_address src;
    struct roce_address dst;
    uint16_t udp_source;
    uint16_t pkey;
    uint32_t qpn;
    uint32_t psn;
    enum roce_opcode opcode;
    bool ack_requested;
    uint64_t remote_address;
    uint32_t rkey;
    uint32_t write_length;
    uint8_t syndrome;
    uint16_t payload_tail_length; /* Included in payload_length. Zero for a contiguous payload. */
    uint32_t msn;
    const uint8_t *payload;
    const uint8_t *payload_tail;
    size_t payload_length;
};

struct roce_cm_frame {
    struct roce_address src;
    struct roce_address dst;
    uint16_t udp_source;
    uint16_t pkey;
    uint32_t psn;
    const uint8_t *mad; /* ROCE_CM_MAD_SIZE bytes. */
};

size_t roce_encode(uint8_t *frame, size_t capacity, const struct roce_packet *packet);
/* The wire order is header, payload spans in order, trailer. */
bool roce_encode_split(uint8_t header[ROCE_MAX_HEADER], uint8_t trailer[ROCE_MAX_TRAILER],
                       const struct roce_packet *packet, uint16_t *header_length,
                       uint16_t *trailer_length);
/* The decoded payload always borrows a contiguous span from the input frame. */
bool roce_decode(struct roce_packet *packet, const uint8_t *frame, size_t frame_length);

/* QP1 UD SEND_ONLY with one MAD. Encode uses separate input and output buffers.
 * Decode borrows the MAD bytes from the input frame. */
size_t roce_cm_frame_encode(uint8_t *frame, size_t capacity, const struct roce_cm_frame *cm);
bool roce_cm_frame_decode(struct roce_cm_frame *cm, const uint8_t *frame, size_t frame_length);

#endif
