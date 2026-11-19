#ifndef MRC_WIRE_H
#define MRC_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Default specified by OCP MRC 1.0 section 7.5.5.2. */
#define MRC_UDP_DEFAULT_PORT 4971
#define MRC_WIRE_PSN_MASK UINT32_C(0x00ffffff)
#define MRC_MAC_LEN 6
#define MRC_BTH_LEN 12
#define MRC_METH_LEN 4
#define MRC_TSETH_LEN 4
#define MRC_RETH_LEN 16
#define MRC_IMMDT_LEN 4
#define MRC_AETH_LEN 4
#define MRC_SETH_LEN 28
#define MRC_CC_STATE_LEN 8
#define MRC_NETH_LEN 20
#define MRC_PETH_LEN 16
#define MRC_ERTH_LEN 16
#define MRC_EETH_LEN 36
#define MRC_ENDPOINT_QPN UINT32_C(2)
#define MRC_WIRE_IPV4_HEADER_LEN 42
#define MRC_WIRE_IPV6_HEADER_LEN 62
#define MRC_IPV6_ADDR_LEN 16
#define MRC_IPV6_FLOW_LABEL_MASK UINT32_C(0x000fffff)
#define MRC_ICRC_LEN 4

enum mrc_opcode {
    MRC_WRITE_FIRST = 0xc6,
    MRC_WRITE_MIDDLE = 0xc7,
    MRC_WRITE_LAST = 0xc8,
    MRC_WRITE_LAST_IMM = 0xc9,
    MRC_WRITE_ONLY = 0xca,
    MRC_WRITE_ONLY_IMM = 0xcb,
    MRC_TRANSPORT_ACK = 0xd1,
    MRC_ENDPOINT_REQUEST = 0xd8,
    MRC_ENDPOINT_RESPONSE = 0xd9,
    MRC_RELIABILITY_SACK = 0xdc,
    MRC_RELIABILITY_NACK = 0xdd,
    MRC_RELIABILITY_PROBE = 0xde,
};

enum mrc_nack_reason {
    MRC_NACK_TRIMMED = 0x01,
    MRC_NACK_TRIMMED_LASTHOP = 0x02,
    MRC_NACK_NO_BITMAP = 0x06,
    MRC_NACK_NO_PKT_BUFFER = 0x07,
    MRC_NACK_NO_RESOURCE = 0x0a,
    MRC_NACK_PSN_OOR_WINDOW = 0x0b,
    MRC_NACK_UNEXP_EVENT = 0x19,
};

enum mrc_endpoint_op {
    MRC_ENDPOINT_PORT_STATUS = 0,
    MRC_ENDPOINT_EV_PROBE = 1,
};

enum mrc_sack_mark {
    MRC_SACK_NONE = 0,
    MRC_SACK_SKIP = 1,
    MRC_SACK_ALWAYS_SKIP = 2,
};

enum mrc_wire_ip_version {
    MRC_WIRE_IP_UNSPECIFIED = 0,
    MRC_WIRE_IP4 = 4,
    MRC_WIRE_IP6 = 6,
};

/* Scalars are in host byte order, including IPv4 addresses and immediate data. */
struct mrc_wire_ipv4 {
    uint8_t src_mac[MRC_MAC_LEN];
    uint8_t dst_mac[MRC_MAC_LEN];
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port; /* 0 requests the default when encoding. */
    uint8_t dscp;
    uint8_t ecn;
    uint8_t ttl; /* 0 requests 64 when encoding. */
};

struct mrc_wire_ipv6 {
    uint8_t src_mac[MRC_MAC_LEN];
    uint8_t dst_mac[MRC_MAC_LEN];
    uint8_t src_ip[MRC_IPV6_ADDR_LEN]; /* Wire-order octets. */
    uint8_t dst_ip[MRC_IPV6_ADDR_LEN]; /* Wire-order octets. */
    uint32_t flow_label;
    uint16_t src_port;
    uint16_t dst_port; /* 0 requests the default when encoding. */
    uint8_t dscp;
    uint8_t ecn;
    uint8_t hop_limit; /* 0 requests 64 when encoding. */
};

/* Diagnostic routing metadata. Addresses are wire-order octets. */
struct mrc_wire_srv6 {
    bool encapsulated;
    uint8_t outer_src_ip[MRC_IPV6_ADDR_LEN];
    uint8_t outer_dst_ip[MRC_IPV6_ADDR_LEN];
    uint8_t outer_dscp;
    uint8_t outer_ecn;
    uint8_t outer_hop_limit;
    uint16_t srh_len; /* Zero when absent. */
};

/*
 * Outer IPv6 fields for an already routed SRv6 packet. The segment list is
 * made of prepacked RFC 9800 containers in RFC 8754 wire order. Set the
 * destination address and Segments Left for the chosen full or reduced list.
 */
struct mrc_wire_srv6_tx {
    uint8_t outer_src_ip[MRC_IPV6_ADDR_LEN];
    uint8_t outer_dst_ip[MRC_IPV6_ADDR_LEN];
    uint32_t flow_label;
    uint8_t dscp;
    uint8_t ecn;
    uint8_t hop_limit; /* 0 requests 64 when encoding. */
    const uint8_t (*segment_list)[MRC_IPV6_ADDR_LEN];
    size_t segment_count; /* 0 omits the SRH. */
    uint8_t segments_left;
};

struct mrc_wire_bth {
    uint8_t opcode;
    uint16_t p_key;
    uint32_t dqp;
    uint32_t psn;
    bool ack_request;
    bool solicited;
    bool retransmit;
    bool timestamp;
};

struct mrc_wire_write {
    uint16_t rqmsn;
    uint16_t msn;
    uint16_t tx_timestamp;
    bool tsr;
    uint64_t va;
    uint32_t r_key;
    uint32_t dma_len; /* Length of the entire work request. */
    uint32_t imm_data;
    const uint8_t *data;
    size_t data_len;
};

struct mrc_wire_aeth {
    uint8_t syndrome; /* Full syndrome/credit octet. Positive ACK is 0x1f. */
    uint32_t msn;
};

struct mrc_wire_sack {
    uint8_t mark;
    bool probe_response;
    int16_t ack_psn_offset; /* Probe responses use the same bits for the probe ID. */
    uint32_t entropy;
    uint16_t spdcid;
    uint16_t dpdcid;
    uint32_t cack_psn;
    uint8_t mpr;
    int16_t sack_offset;
    uint64_t bitmap;
    /* Congestion-control format 0 per Tables 7-12 through 7-15. */
    uint16_t tx_timestamp;
    uint16_t ooo_count;
    bool restore_cwnd;
    uint8_t rcv_cwnd_pen;
    uint32_t rcvd_bytes; /* Counter in units of 256 bytes. */
};

struct mrc_wire_nack {
    uint8_t reason;
    uint8_t vendor_info;
    uint32_t entropy;
    uint16_t spdcid;
    uint16_t dpdcid;
    uint32_t nack_psn;
    uint16_t tx_timestamp;
    /* TRIMMED/TRIMMED_LASTHOP carry the triggering datagram's UDP length. */
    uint16_t original_udp_len;
};

struct mrc_wire_probe {
    uint8_t vendor_info;
    uint16_t probe_id;
    uint16_t spdcid;
    uint16_t dpdcid;
    uint16_t tx_timestamp;
    bool tsr;
    bool timestamp;
};

struct mrc_wire_endpoint_request {
    uint8_t op;
    uint8_t vendor_info;
    uint32_t port_status_mask;
    uint16_t tx_timestamp;
    bool tsr;
    bool timestamp;
};

struct mrc_wire_endpoint_response {
    uint8_t op;
    uint16_t tx_timestamp;
};

/* Reliability metadata from an explicitly classified, possibly truncated WRITE. */
struct mrc_wire_trimmed {
    enum mrc_wire_ip_version ip_version;
    union {
        struct mrc_wire_ipv4 ipv4;
        struct mrc_wire_ipv6 ipv6;
    };
    struct mrc_wire_bth bth;
    uint16_t original_udp_len;
    uint16_t tx_timestamp;
    bool timestamp;
    bool tsr;
};

struct mrc_wire_packet {
    enum mrc_wire_ip_version ip_version;
    union {
        struct mrc_wire_ipv4 ipv4;
        struct mrc_wire_ipv6 ipv6;
    };
    struct mrc_wire_bth bth;
    union {
        struct mrc_wire_write write;
        struct mrc_wire_aeth aeth;
        struct mrc_wire_sack sack;
        struct mrc_wire_nack nack;
        struct mrc_wire_probe probe;
        struct mrc_wire_endpoint_request endpoint_request;
        struct mrc_wire_endpoint_response endpoint_response;
    } body;
};

bool mrc_wire_is_write(uint8_t opcode);

/*
 * Encode an untagged Ethernet frame carrying IPv4/UDP or IPv6/UDP. A zero
 * ip_version encodes IPv4 for compatibility. Decode accepts IPv4 only.
 * Decode_ip accepts either IP version and sets ip_version on success. Frames
 * exclude the Ethernet FCS. IPv6 frames carry UDP directly after the base IP
 * header. The decoder verifies the IPv4 checksum and RoCE iCRC and returns a zero-copy
 * payload view valid for as long as the input buffer. Callers own QP state,
 * connection identity checks, data placement bounds, and PMTU validation.
 *
 * Return 0 on success or a negative errno. Supported error codes:
 * EINVAL for invalid encoder arguments/fields, EMSGSIZE for insufficient
 * capacity, EBADMSG for malformed/truncated/integrity-invalid input, and
 * EPROTONOSUPPORT for unsupported envelopes/opcodes/congestion formats.
 */
int mrc_wire_encode(uint8_t *frame, size_t capacity, const struct mrc_wire_packet *packet,
                    size_t *length);
int mrc_wire_decode(const uint8_t *frame, size_t length, struct mrc_wire_packet *packet);
int mrc_wire_decode_ip(const uint8_t *frame, size_t length, struct mrc_wire_packet *packet);

/* Returns IP, UDP and BTH fields for dispatch of unencapsulated packets.
 * This accepts a physically present prefix, including trimmed packets.
 * The receiving endpoint still validates lengths, iCRC and payload. */
int mrc_wire_dispatch_header(const uint8_t *frame, size_t length, struct mrc_wire_packet *header);

/*
 * Expand an already encoded bare IPv6/UDP MRC frame in place. The frame's
 * Ethernet header and complete inner datagram, including iCRC, are retained.
 * This validates inner bounds and framing. Use the ordinary receive decoder
 * for end-to-end integrity validation.
 */
int mrc_wire_wrap_srv6(uint8_t *frame, size_t capacity, size_t inner_frame_len,
                        const struct mrc_wire_srv6_tx *route, size_t *length);

/*
 * Receive a bare IP frame or preserved SRv6 encapsulation as in OCP 9.3.5.1.
 * With an SRH, the packet must be at the end of its route (Segments Left 0).
 * The optional routing output is zero for a bare frame. Packet MAC addresses
 * describe the received Ethernet header. IP fields, RoCE iCRC, and zero-copy
 * payload refer to the inner datagram. The caller remains responsible for
 * local SID and inner address checks and any outer ECN handling. This
 * function validates SRH lengths but does not process transit SIDs or HMAC.
 */
int mrc_wire_decode_srv6(const uint8_t *frame, size_t length, struct mrc_wire_packet *packet,
                         struct mrc_wire_srv6 *routing);

/* A complete, checksum-verified frame with an unsupported BTH opcode.
 * Only the IP and BTH fields of header are populated. */
int mrc_wire_decode_unsupported_opcode(const uint8_t *frame, size_t length,
                                        struct mrc_wire_packet *header, struct mrc_wire_srv6 *routing);

/*
 * Validate the envelope and iCRC of a complete datagram without interpreting
 * the opcode. Accepts matching IP/UDP lengths only. A responder can use this
 * after an unsupported opcode to match a QP and report an invalid request.
 */
int mrc_wire_decode_header(const uint8_t *frame, size_t length, struct mrc_wire_ipv4 *ipv4,
                           struct mrc_wire_bth *bth);
int mrc_wire_decode_ipv6_header(const uint8_t *frame, size_t length, struct mrc_wire_ipv6 *ipv6,
                                struct mrc_wire_bth *bth);

/*
 * Call for configured trim DSCPs before attempting ordinary receive processing.
 * A nonmatching DSCP or unsupported IP envelope returns EPROTONOSUPPORT. A
 * matching WRITE may lack an iCRC, so returned metadata is for reliability
 * responses only. It cannot be used for placement or semantic completion.
 * Decode_trimmed accepts IPv4. Decode_trimmed_ip accepts IPv4 or IPv6.
 */
int mrc_wire_decode_trimmed(const uint8_t *frame, size_t length, uint8_t trim_dscp,
                            struct mrc_wire_trimmed *packet);
int mrc_wire_decode_trimmed_ip(const uint8_t *frame, size_t length, uint8_t trim_dscp,
                               struct mrc_wire_trimmed *packet);

/*
 * Preserved encapsulation is classified by the outer IPv6 DSCP. Bare frames
 * use their own DSCP. Output contains only reliability metadata and never a
 * payload view. If requested, routing metadata is zero for a bare frame.
 */
int mrc_wire_decode_trimmed_srv6(const uint8_t *frame, size_t length, uint8_t trim_dscp,
                                 struct mrc_wire_trimmed *packet, struct mrc_wire_srv6 *routing);

#endif
