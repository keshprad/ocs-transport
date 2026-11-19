#include "mrc_wire.h"

#include <errno.h>
#include <string.h>

enum {
    ETHERNET_LEN = 14,
    IPV4_LEN = 20,
    IPV6_LEN = 40,
    UDP_LEN = 8,
    ETHERTYPE_IPV4 = 0x0800,
    ETHERTYPE_IPV6 = 0x86dd,
    IPPROTO_IPV4_ENCAP = 4,
    IPPROTO_UDP = 17,
    IPPROTO_IPV6_ENCAP = 41,
    IPPROTO_ROUTING = 43,
    SRH_ROUTING_TYPE = 4,
    SRH_FIXED_LEN = 8,
    SRH_PAD1 = 0,
    IPV4_DONT_FRAGMENT = 0x4000,
    IP_DEFAULT_HOP_LIMIT = 64,
    BTH_SOLICITED = 0x80,
    BTH_ACK_REQUEST = 0x80,
    BTH_RETRANSMIT = 0x20,
    BTH_TIMESTAMP = 0x10,
    TIMESTAMP_RESOLUTION = 0x80,
    TIMESTAMP_TYPE = 0x01,
    NETH_CC_TYPE = 0x20,
};

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_be24(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 16) | read_be16(bytes + 1);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)read_be16(bytes) << 16) | read_be16(bytes + 2);
}

static uint64_t read_be64(const uint8_t *bytes)
{
    return ((uint64_t)read_be32(bytes) << 32) | read_be32(bytes + 4);
}

static int16_t read_signed_be16(const uint8_t *bytes)
{
    uint16_t value = read_be16(bytes);
    return value <= INT16_MAX ? (int16_t)value : (int16_t)((int32_t)value - 65536);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_be24(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 16);
    write_be16(bytes + 1, (uint16_t)value);
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    write_be16(bytes, (uint16_t)(value >> 16));
    write_be16(bytes + 2, (uint16_t)value);
}

static void write_be64(uint8_t *bytes, uint64_t value)
{
    write_be32(bytes, (uint32_t)(value >> 32));
    write_be32(bytes + 4, (uint32_t)value);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint16_t ipv4_checksum(const uint8_t *ip)
{
    uint32_t sum = 0;
    for (size_t offset = 0; offset < IPV4_LEN; offset += 2)
        sum += read_be16(ip + offset);
    sum = (sum & UINT32_C(0xffff)) + (sum >> 16);
    sum = (sum & UINT32_C(0xffff)) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t len)
{
    /* Four-bit table for the reflected IEEE polynomial 0xedb88320. */
    static const uint32_t table[] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };

    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        crc = (crc >> 4) ^ table[crc & 0xf];
        crc = (crc >> 4) ^ table[crc & 0xf];
    }
    return crc;
}

static uint32_t roce_icrc(const uint8_t *ip, size_t ip_header_len, size_t protected_len)
{
    uint8_t masked[IPV6_LEN + UDP_LEN + MRC_BTH_LEN];
    size_t prefix_len = ip_header_len + UDP_LEN + MRC_BTH_LEN;
    memcpy(masked, ip, prefix_len);
    if (ip_header_len == IPV4_LEN) {
        masked[1] = 0xff;  /* IPv4 DSCP and ECN */
        masked[8] = 0xff;  /* IPv4 TTL */
        masked[10] = 0xff; /* IPv4 header checksum */
        masked[11] = 0xff;
    } else {
        masked[0] |= 0x0f; /* IPv6 traffic class and flow label */
        memset(masked + 1, 0xff, 3);
        masked[7] = 0xff;  /* IPv6 hop limit */
    }
    masked[ip_header_len + 6] = 0xff; /* UDP checksum */
    masked[ip_header_len + 7] = 0xff;
    masked[ip_header_len + UDP_LEN + 4] = 0xff; /* BTH variant reserved field */
    /* This seed includes the eight all-ones bytes of the masked IB LRH. */
    uint32_t crc = crc32_update(UINT32_C(0xdebb20e3), masked, prefix_len);
    return ~crc32_update(crc, ip + prefix_len, protected_len - prefix_len);
}

bool mrc_wire_is_write(uint8_t opcode)
{
    return opcode >= MRC_WRITE_FIRST && opcode <= MRC_WRITE_ONLY_IMM;
}

static bool has_imm_data(uint8_t opcode)
{
    return opcode == MRC_WRITE_LAST_IMM || opcode == MRC_WRITE_ONLY_IMM;
}

static bool is_trim_reason(uint8_t reason)
{
    return reason == MRC_NACK_TRIMMED || reason == MRC_NACK_TRIMMED_LASTHOP;
}

static size_t write_header_len(const struct mrc_wire_bth *bth)
{
    return MRC_METH_LEN + MRC_RETH_LEN + (bth->timestamp ? MRC_TSETH_LEN : 0) +
           (has_imm_data(bth->opcode) ? MRC_IMMDT_LEN : 0);
}

static void encode_bth(uint8_t *bytes, const struct mrc_wire_bth *bth, uint8_t pad)
{
    bytes[0] = bth->opcode;
    bytes[1] = (uint8_t)(pad << 4) | (bth->solicited ? BTH_SOLICITED : 0);
    write_be16(bytes + 2, bth->p_key);
    write_be24(bytes + 5, bth->dqp);
    bytes[8] = (bth->ack_request ? BTH_ACK_REQUEST : 0) |
               (bth->retransmit ? BTH_RETRANSMIT : 0) |
               (bth->timestamp ? BTH_TIMESTAMP : 0);
    write_be24(bytes + 9, bth->psn);
}

static void decode_bth(const uint8_t *bytes, struct mrc_wire_bth *bth)
{
    bth->opcode = bytes[0];
    bth->solicited = (bytes[1] & BTH_SOLICITED) != 0;
    bth->p_key = read_be16(bytes + 2);
    bth->dqp = read_be24(bytes + 5);
    bth->ack_request = (bytes[8] & BTH_ACK_REQUEST) != 0;
    bth->retransmit = (bytes[8] & BTH_RETRANSMIT) != 0;
    bth->timestamp = (bytes[8] & BTH_TIMESTAMP) != 0;
    bth->psn = read_be24(bytes + 9);
}

static void encode_timestamp(uint8_t *bytes, uint16_t timestamp, bool tsr)
{
    write_be16(bytes, timestamp);
    bytes[2] = tsr ? TIMESTAMP_RESOLUTION : 0;
    bytes[3] = TIMESTAMP_TYPE;
}

static void encode_write(uint8_t *bytes, const struct mrc_wire_bth *bth,
                         const struct mrc_wire_write *write)
{
    write_be16(bytes, write->rqmsn);
    write_be16(bytes + 2, write->msn);
    bytes += MRC_METH_LEN;
    if (bth->timestamp) {
        encode_timestamp(bytes, write->tx_timestamp, write->tsr);
        bytes += MRC_TSETH_LEN;
    }
    write_be64(bytes, write->va);
    write_be32(bytes + 8, write->r_key);
    write_be32(bytes + 12, write->dma_len);
    bytes += MRC_RETH_LEN;
    if (has_imm_data(bth->opcode)) {
        write_be32(bytes, write->imm_data);
        bytes += MRC_IMMDT_LEN;
    }
    if (write->data_len)
        memcpy(bytes, write->data, write->data_len);
}

static int decode_write(const uint8_t *bytes, size_t len, uint8_t pad,
                        const struct mrc_wire_bth *bth, struct mrc_wire_write *write)
{
    size_t header_len = write_header_len(bth);
    if (len < header_len + pad || (len & 3))
        return -EBADMSG;

    write->rqmsn = read_be16(bytes);
    write->msn = read_be16(bytes + 2);
    bytes += MRC_METH_LEN;
    if (bth->timestamp) {
        if ((bytes[3] & 0xf) != TIMESTAMP_TYPE)
            return -EBADMSG;
        write->tx_timestamp = read_be16(bytes);
        write->tsr = (bytes[2] & TIMESTAMP_RESOLUTION) != 0;
        bytes += MRC_TSETH_LEN;
    }
    write->va = read_be64(bytes);
    write->r_key = read_be32(bytes + 8);
    write->dma_len = read_be32(bytes + 12);
    bytes += MRC_RETH_LEN;
    if (has_imm_data(bth->opcode)) {
        write->imm_data = read_be32(bytes);
        bytes += MRC_IMMDT_LEN;
    }
    write->data = bytes;
    write->data_len = len - header_len - pad;
    return 0;
}

static void encode_sack(uint8_t *bytes, const struct mrc_wire_sack *sack)
{
    bytes[1] = (uint8_t)((sack->mark << 5) | (sack->probe_response ? 2 : 0));
    write_be16(bytes + 2, (uint16_t)sack->ack_psn_offset);
    write_be32(bytes + 4, sack->entropy);
    write_be16(bytes + 8, sack->spdcid);
    write_be16(bytes + 10, sack->dpdcid);
    write_be24(bytes + 13, sack->cack_psn);
    bytes[17] = sack->mpr;
    write_be16(bytes + 18, (uint16_t)sack->sack_offset);
    write_be64(bytes + 20, sack->bitmap);

    uint8_t *cc = bytes + MRC_SETH_LEN;
    write_be16(cc, sack->tx_timestamp);
    write_be16(cc + 2, sack->ooo_count);
    cc[4] = (sack->restore_cwnd ? 0x80 : 0) | sack->rcv_cwnd_pen;
    write_be24(cc + 5, sack->rcvd_bytes);
}

static int decode_sack(const uint8_t *bytes, struct mrc_wire_sack *sack)
{
    if (bytes[16] >> 4)
        return -EPROTONOSUPPORT;

    sack->mark = (bytes[1] >> 5) & 3;
    sack->probe_response = (bytes[1] & 2) != 0;
    sack->ack_psn_offset = read_signed_be16(bytes + 2);
    sack->entropy = read_be32(bytes + 4);
    sack->spdcid = read_be16(bytes + 8);
    sack->dpdcid = read_be16(bytes + 10);
    sack->cack_psn = read_be24(bytes + 13);
    sack->mpr = bytes[17];
    sack->sack_offset = read_signed_be16(bytes + 18);
    sack->bitmap = read_be64(bytes + 20);

    const uint8_t *cc = bytes + MRC_SETH_LEN;
    sack->tx_timestamp = read_be16(cc);
    sack->ooo_count = read_be16(cc + 2) & 0x7fff;
    sack->restore_cwnd = (cc[4] & 0x80) != 0;
    sack->rcv_cwnd_pen = cc[4] & 0x7f;
    sack->rcvd_bytes = read_be24(cc + 5);
    return 0;
}

static void encode_nack(uint8_t *bytes, const struct mrc_wire_nack *nack)
{
    bytes[2] = nack->reason;
    bytes[3] = nack->vendor_info;
    write_be32(bytes + 4, nack->entropy);
    write_be16(bytes + 8, nack->spdcid);
    write_be16(bytes + 10, nack->dpdcid);
    write_be24(bytes + 13, nack->nack_psn);
    bytes[16] = NETH_CC_TYPE;
    write_be16(bytes + 18, nack->tx_timestamp);
}

static int decode_nack(const uint8_t *bytes, struct mrc_wire_nack *nack,
                       uint16_t nominal_udp_len)
{
    if ((bytes[16] & 0xf0) != NETH_CC_TYPE)
        return -EPROTONOSUPPORT;

    nack->reason = bytes[2];
    nack->vendor_info = bytes[3];
    nack->entropy = read_be32(bytes + 4);
    nack->spdcid = read_be16(bytes + 8);
    nack->dpdcid = read_be16(bytes + 10);
    nack->nack_psn = read_be24(bytes + 13);
    nack->tx_timestamp = read_be16(bytes + 18);
    if (is_trim_reason(nack->reason))
        nack->original_udp_len = nominal_udp_len;
    return 0;
}

static void encode_probe(uint8_t *bytes, const struct mrc_wire_probe *probe)
{
    bytes[3] = probe->vendor_info;
    write_be16(bytes + 4, probe->probe_id);
    write_be16(bytes + 8, probe->spdcid);
    write_be16(bytes + 10, probe->dpdcid);
    if (probe->timestamp)
        encode_timestamp(bytes + 12, probe->tx_timestamp, probe->tsr);
}

static int decode_probe(const uint8_t *bytes, struct mrc_wire_probe *probe)
{
    uint8_t type = bytes[15] & 0xf;
    if (type != 0 && type != TIMESTAMP_TYPE)
        return -EBADMSG;

    probe->vendor_info = bytes[3];
    probe->probe_id = read_be16(bytes + 4);
    probe->spdcid = read_be16(bytes + 8);
    probe->dpdcid = read_be16(bytes + 10);
    probe->timestamp = type == TIMESTAMP_TYPE;
    if (probe->timestamp) {
        probe->tx_timestamp = read_be16(bytes + 12);
        probe->tsr = (bytes[14] & TIMESTAMP_RESOLUTION) != 0;
    }
    return 0;
}

static void encode_endpoint_request(uint8_t *bytes,
                                    const struct mrc_wire_endpoint_request *request)
{
    bytes[1] = request->op;
    bytes[3] = request->vendor_info;
    if (request->op == MRC_ENDPOINT_PORT_STATUS)
        write_be32(bytes + 4, request->port_status_mask);
    if (request->timestamp)
        encode_timestamp(bytes + 12, request->tx_timestamp, request->tsr);
}

static int decode_endpoint_request(const uint8_t *bytes,
                                   struct mrc_wire_endpoint_request *request)
{
    uint8_t type = bytes[15] & 0xf;
    request->op = bytes[1] & 3;
    if (request->op > MRC_ENDPOINT_EV_PROBE || (type && type != TIMESTAMP_TYPE))
        return -EBADMSG;

    request->vendor_info = bytes[3];
    if (request->op == MRC_ENDPOINT_PORT_STATUS)
        request->port_status_mask = read_be32(bytes + 4);
    request->timestamp = type == TIMESTAMP_TYPE;
    if (request->timestamp) {
        request->tx_timestamp = read_be16(bytes + 12);
        request->tsr = (bytes[14] & TIMESTAMP_RESOLUTION) != 0;
    }
    return 0;
}

static void encode_endpoint_response(uint8_t *bytes,
                                     const struct mrc_wire_endpoint_response *response)
{
    bytes[1] = (uint8_t)(response->op << 2);
    write_be16(bytes + 28, response->tx_timestamp);
}

static int decode_endpoint_response(const uint8_t *bytes,
                                    struct mrc_wire_endpoint_response *response)
{
    response->op = (bytes[1] >> 2) & 3;
    if (response->op > MRC_ENDPOINT_EV_PROBE)
        return -EBADMSG;
    response->tx_timestamp = read_be16(bytes + 28);
    return 0;
}

static bool is_endpoint(uint8_t opcode)
{
    return opcode == MRC_ENDPOINT_REQUEST || opcode == MRC_ENDPOINT_RESPONSE;
}

static size_t control_body_len(uint8_t opcode)
{
    switch (opcode) {
    case MRC_TRANSPORT_ACK:
        return MRC_AETH_LEN;
    case MRC_RELIABILITY_SACK:
        return MRC_SETH_LEN + MRC_CC_STATE_LEN;
    case MRC_RELIABILITY_NACK:
        return MRC_NETH_LEN;
    case MRC_RELIABILITY_PROBE:
        return MRC_PETH_LEN;
    case MRC_ENDPOINT_REQUEST:
        return MRC_ERTH_LEN;
    case MRC_ENDPOINT_RESPONSE:
        return MRC_EETH_LEN;
    default:
        return 0;
    }
}

static int validate_control(const struct mrc_wire_packet *packet)
{
    if (!control_body_len(packet->bth.opcode))
        return -EPROTONOSUPPORT;
    if (packet->bth.timestamp ||
        (is_endpoint(packet->bth.opcode) &&
         (packet->bth.dqp != MRC_ENDPOINT_QPN || packet->bth.psn > UINT16_MAX)))
        return -EINVAL;

    switch (packet->bth.opcode) {
    case MRC_TRANSPORT_ACK:
        return packet->body.aeth.msn <= MRC_WIRE_PSN_MASK ? 0 : -EINVAL;
    case MRC_RELIABILITY_SACK: {
        const struct mrc_wire_sack *sack = &packet->body.sack;
        if (sack->mark > MRC_SACK_ALWAYS_SKIP || sack->cack_psn > MRC_WIRE_PSN_MASK ||
            sack->ooo_count > 0x7fff || sack->rcv_cwnd_pen > 0x7f ||
            sack->rcvd_bytes > MRC_WIRE_PSN_MASK)
            return -EINVAL;
        return 0;
    }
    case MRC_RELIABILITY_NACK: {
        const struct mrc_wire_nack *nack = &packet->body.nack;
        size_t min_udp_len = UDP_LEN + MRC_BTH_LEN + MRC_NETH_LEN + MRC_ICRC_LEN;
        if (nack->nack_psn > MRC_WIRE_PSN_MASK ||
            (is_trim_reason(nack->reason) && nack->original_udp_len < min_udp_len))
            return -EINVAL;
        return 0;
    }
    case MRC_ENDPOINT_REQUEST:
        return packet->body.endpoint_request.op <= MRC_ENDPOINT_EV_PROBE ? 0 : -EINVAL;
    case MRC_ENDPOINT_RESPONSE:
        return packet->body.endpoint_response.op <= MRC_ENDPOINT_EV_PROBE ? 0 : -EINVAL;
    default:
        return 0;
    }
}

static void encode_control(uint8_t *bytes, const struct mrc_wire_packet *packet)
{
    switch (packet->bth.opcode) {
    case MRC_TRANSPORT_ACK:
        bytes[0] = packet->body.aeth.syndrome;
        write_be24(bytes + 1, packet->body.aeth.msn);
        break;
    case MRC_RELIABILITY_SACK:
        encode_sack(bytes, &packet->body.sack);
        break;
    case MRC_RELIABILITY_NACK:
        encode_nack(bytes, &packet->body.nack);
        break;
    case MRC_RELIABILITY_PROBE:
        encode_probe(bytes, &packet->body.probe);
        break;
    case MRC_ENDPOINT_REQUEST:
        encode_endpoint_request(bytes, &packet->body.endpoint_request);
        break;
    case MRC_ENDPOINT_RESPONSE:
        encode_endpoint_response(bytes, &packet->body.endpoint_response);
        break;
    }
}

static int decode_control(const uint8_t *bytes, size_t len, struct mrc_wire_packet *packet,
                          uint16_t physical_udp_len, uint16_t nominal_udp_len)
{
    size_t expected_len = control_body_len(packet->bth.opcode);
    if (!expected_len)
        return -EPROTONOSUPPORT;
    if (len != expected_len || packet->bth.timestamp ||
        (is_endpoint(packet->bth.opcode) && packet->bth.dqp != MRC_ENDPOINT_QPN))
        return -EBADMSG;

    bool trim_nack = packet->bth.opcode == MRC_RELIABILITY_NACK && is_trim_reason(bytes[2]);
    /* Only a trim NACK copies the original, potentially longer UDP length. */
    if ((!trim_nack && physical_udp_len != nominal_udp_len) ||
        (trim_nack && nominal_udp_len < physical_udp_len))
        return -EBADMSG;

    switch (packet->bth.opcode) {
    case MRC_TRANSPORT_ACK:
        packet->body.aeth.syndrome = bytes[0];
        packet->body.aeth.msn = read_be24(bytes + 1);
        return 0;
    case MRC_RELIABILITY_SACK:
        return decode_sack(bytes, &packet->body.sack);
    case MRC_RELIABILITY_NACK:
        return decode_nack(bytes, &packet->body.nack, nominal_udp_len);
    case MRC_RELIABILITY_PROBE:
        return decode_probe(bytes, &packet->body.probe);
    case MRC_ENDPOINT_REQUEST:
        return decode_endpoint_request(bytes, &packet->body.endpoint_request);
    case MRC_ENDPOINT_RESPONSE:
        return decode_endpoint_response(bytes, &packet->body.endpoint_response);
    default:
        return -EPROTONOSUPPORT;
    }
}

static void encode_ethernet(uint8_t *frame, uint16_t ethertype,
                             const uint8_t *dst, const uint8_t *src)
{
    memcpy(frame, dst, MRC_MAC_LEN);
    memcpy(frame + MRC_MAC_LEN, src, MRC_MAC_LEN);
    write_be16(frame + 2 * MRC_MAC_LEN, ethertype);
}

static void encode_ipv4(uint8_t *frame, const struct mrc_wire_ipv4 *ipv4, uint16_t udp_len)
{
    encode_ethernet(frame, ETHERTYPE_IPV4, ipv4->dst_mac, ipv4->src_mac);
    uint8_t *ip = frame + ETHERNET_LEN;
    ip[0] = 0x45;
    ip[1] = (uint8_t)((ipv4->dscp << 2) | ipv4->ecn);
    write_be16(ip + 2, (uint16_t)(IPV4_LEN + udp_len));
    write_be16(ip + 6, IPV4_DONT_FRAGMENT);
    ip[8] = ipv4->ttl ? ipv4->ttl : IP_DEFAULT_HOP_LIMIT;
    ip[9] = IPPROTO_UDP;
    write_be32(ip + 12, ipv4->src_ip);
    write_be32(ip + 16, ipv4->dst_ip);
    write_be16(ip + 10, ipv4_checksum(ip));
}

static void encode_ipv6_base(uint8_t *ip, uint32_t flow_label, uint8_t traffic_class,
                              uint8_t hop_limit, uint8_t next_header, uint16_t payload_len)
{
    uint32_t version_traffic_flow = UINT32_C(0x60000000) |
                                    ((uint32_t)traffic_class << 20) | flow_label;
    write_be32(ip, version_traffic_flow);
    write_be16(ip + 4, payload_len);
    ip[6] = next_header;
    ip[7] = hop_limit ? hop_limit : IP_DEFAULT_HOP_LIMIT;
}

static void encode_ipv6(uint8_t *frame, const struct mrc_wire_ipv6 *ipv6, uint16_t udp_len)
{
    encode_ethernet(frame, ETHERTYPE_IPV6, ipv6->dst_mac, ipv6->src_mac);
    uint8_t *ip = frame + ETHERNET_LEN;
    uint8_t traffic_class = (uint8_t)((ipv6->dscp << 2) | ipv6->ecn);
    encode_ipv6_base(ip, ipv6->flow_label, traffic_class, ipv6->hop_limit, IPPROTO_UDP, udp_len);
    memcpy(ip + 8, ipv6->src_ip, MRC_IPV6_ADDR_LEN);
    memcpy(ip + 24, ipv6->dst_ip, MRC_IPV6_ADDR_LEN);
}

int mrc_wire_encode(uint8_t *frame, size_t capacity, const struct mrc_wire_packet *packet,
                    size_t *length)
{
    if (!frame || !packet || !length || packet->bth.psn > MRC_WIRE_PSN_MASK ||
        packet->bth.dqp > MRC_WIRE_PSN_MASK ||
        (packet->bth.solicited && !has_imm_data(packet->bth.opcode)))
        return -EINVAL;

    size_t ip_header_len;
    uint16_t src_port, dst_port;
    switch (packet->ip_version) {
    case MRC_WIRE_IP_UNSPECIFIED:
    case MRC_WIRE_IP4:
        if (packet->ipv4.dscp > 63 || packet->ipv4.ecn > 3)
            return -EINVAL;
        ip_header_len = IPV4_LEN;
        src_port = packet->ipv4.src_port;
        dst_port = packet->ipv4.dst_port;
        break;
    case MRC_WIRE_IP6:
        if (packet->ipv6.dscp > 63 || packet->ipv6.ecn > 3 ||
            packet->ipv6.flow_label > MRC_IPV6_FLOW_LABEL_MASK)
            return -EINVAL;
        ip_header_len = IPV6_LEN;
        src_port = packet->ipv6.src_port;
        dst_port = packet->ipv6.dst_port;
        break;
    default:
        return -EINVAL;
    }

    const struct mrc_wire_bth *bth = &packet->bth;
    size_t body_len;
    uint8_t pad = 0;
    bool is_write = mrc_wire_is_write(bth->opcode);
    if (is_write) {
        size_t data_len = packet->body.write.data_len;
        if (data_len > UINT16_MAX || (data_len && !packet->body.write.data))
            return -EINVAL;
        pad = (uint8_t)((4 - (data_len & 3)) & 3);
        body_len = write_header_len(bth) + data_len + pad;
    } else {
        int error = validate_control(packet);
        if (error)
            return error;
        body_len = control_body_len(bth->opcode);
    }

    size_t udp_len = UDP_LEN + MRC_BTH_LEN + body_len + MRC_ICRC_LEN;
    size_t ip_len = ip_header_len + udp_len;
    size_t frame_len = ETHERNET_LEN + ip_len;
    if (udp_len > UINT16_MAX || (ip_header_len == IPV4_LEN && ip_len > UINT16_MAX))
        return -EINVAL;
    if (frame_len > capacity)
        return -EMSGSIZE;

    uint8_t *ip = frame + ETHERNET_LEN;
    memset(ip, 0, ip_header_len + UDP_LEN + MRC_BTH_LEN +
                  (is_write ? write_header_len(bth) : body_len));
    if (ip_header_len == IPV4_LEN)
        encode_ipv4(frame, &packet->ipv4, (uint16_t)udp_len);
    else
        encode_ipv6(frame, &packet->ipv6, (uint16_t)udp_len);

    uint8_t *udp = ip + ip_header_len;
    uint16_t nominal_udp_len = (uint16_t)udp_len;
    if (bth->opcode == MRC_RELIABILITY_NACK && is_trim_reason(packet->body.nack.reason))
        nominal_udp_len = packet->body.nack.original_udp_len;
    write_be16(udp, src_port);
    write_be16(udp + 2, dst_port ? dst_port : MRC_UDP_DEFAULT_PORT);
    write_be16(udp + 4, nominal_udp_len);
    encode_bth(udp + UDP_LEN, bth, pad);
    uint8_t *body = udp + UDP_LEN + MRC_BTH_LEN;
    if (is_write)
        encode_write(body, bth, &packet->body.write);
    else
        encode_control(body, packet);
    memset(body + body_len - pad, 0, pad);

    write_le32(frame + frame_len - MRC_ICRC_LEN, roce_icrc(ip, ip_header_len, ip_len - MRC_ICRC_LEN));
    *length = frame_len;
    return 0;
}

struct wire_prefix {
    const uint8_t *ip;
    const uint8_t *udp;
    enum mrc_wire_ip_version ip_version;
    uint16_t physical_udp_len;
};

static enum mrc_wire_ip_version frame_ip_version(const uint8_t *frame)
{
    uint16_t ethertype = read_be16(frame + 2 * MRC_MAC_LEN);
    if (ethertype == ETHERTYPE_IPV4)
        return MRC_WIRE_IP4;
    if (ethertype == ETHERTYPE_IPV6)
        return MRC_WIRE_IP6;
    return MRC_WIRE_IP_UNSPECIFIED;
}

static bool family_matches(enum mrc_wire_ip_version actual,
                           enum mrc_wire_ip_version expected)
{
    return actual != MRC_WIRE_IP_UNSPECIFIED &&
           (expected == MRC_WIRE_IP_UNSPECIFIED || actual == expected);
}

static uint8_t ip_traffic_class(const uint8_t *ip, enum mrc_wire_ip_version version)
{
    return version == MRC_WIRE_IP4 ? ip[1] : (uint8_t)(read_be16(ip) >> 4);
}

/* Parse the physically present prefix. Ordinary traffic also needs validated
 * lengths and iCRC. For trim handling the caller checks the configured DSCP.
 */
static int decode_ip_prefix(const uint8_t *ip, size_t available,
                             enum mrc_wire_ip_version version, struct wire_prefix *prefix)
{
    size_t ip_header_len = version == MRC_WIRE_IP4 ? IPV4_LEN : IPV6_LEN;
    if (available < ip_header_len + UDP_LEN + MRC_BTH_LEN)
        return -EBADMSG;

    if (version == MRC_WIRE_IP4) {
        if (ip[0] != 0x45 || ip[9] != IPPROTO_UDP ||
            (read_be16(ip + 6) & (uint16_t)~IPV4_DONT_FRAGMENT))
            return -EPROTONOSUPPORT;
        if (ipv4_checksum(ip))
            return -EBADMSG;
    } else if ((ip[0] >> 4) != 6 || ip[6] != IPPROTO_UDP) {
        return -EPROTONOSUPPORT;
    }

    const uint8_t *udp = ip + ip_header_len;
    const uint8_t *bth = udp + UDP_LEN;
    if (read_be16(udp + 6) || !read_be16(udp + 2) || (bth[1] & 0xf))
        return -EBADMSG;
    *prefix = (struct wire_prefix){ .ip = ip, .udp = udp, .ip_version = version };
    return 0;
}

static int decode_prefix(const uint8_t *frame, size_t length, enum mrc_wire_ip_version expected,
                         struct wire_prefix *prefix)
{
    if (length < MRC_WIRE_IPV4_HEADER_LEN + MRC_BTH_LEN)
        return -EBADMSG;

    enum mrc_wire_ip_version version = frame_ip_version(frame);
    if (!family_matches(version, expected))
        return -EPROTONOSUPPORT;
    return decode_ip_prefix(frame + ETHERNET_LEN, length - ETHERNET_LEN, version, prefix);
}

static int validate_ip_envelope(struct wire_prefix *prefix, size_t available, bool encapsulated)
{
    const uint8_t *ip = prefix->ip;
    size_t ip_header_len = (size_t)(prefix->udp - ip);
    size_t ip_len = prefix->ip_version == MRC_WIRE_IP4 ? read_be16(ip + 2) :
                    IPV6_LEN + read_be16(ip + 4);
    /* An inner IP datagram has no Ethernet padding to explain trailing octets. */
    if (ip_len < ip_header_len + UDP_LEN + MRC_BTH_LEN + MRC_ICRC_LEN ||
        ip_len > available || (encapsulated && ip_len != available))
        return -EBADMSG;

    size_t crc_offset = ip_len - MRC_ICRC_LEN;
    if (read_le32(ip + crc_offset) != roce_icrc(ip, ip_header_len, crc_offset))
        return -EBADMSG;

    prefix->physical_udp_len = (uint16_t)(ip_len - ip_header_len);
    return 0;
}

static int decode_envelope(const uint8_t *frame, size_t length, enum mrc_wire_ip_version expected,
                           struct wire_prefix *prefix)
{
    int error = decode_prefix(frame, length, expected, prefix);
    if (error)
        return error;
    return validate_ip_envelope(prefix, length - ETHERNET_LEN, false);
}

static void decode_ipv4(const uint8_t *frame, const struct wire_prefix *prefix,
                         struct mrc_wire_ipv4 *ipv4)
{
    const uint8_t *ip = prefix->ip;
    memcpy(ipv4->dst_mac, frame, MRC_MAC_LEN);
    memcpy(ipv4->src_mac, frame + MRC_MAC_LEN, MRC_MAC_LEN);
    ipv4->src_ip = read_be32(ip + 12);
    ipv4->dst_ip = read_be32(ip + 16);
    ipv4->src_port = read_be16(prefix->udp);
    ipv4->dst_port = read_be16(prefix->udp + 2);
    ipv4->dscp = ip[1] >> 2;
    ipv4->ecn = ip[1] & 3;
    ipv4->ttl = ip[8];
}

static void decode_ipv6(const uint8_t *frame, const struct wire_prefix *prefix,
                         struct mrc_wire_ipv6 *ipv6)
{
    const uint8_t *ip = prefix->ip;
    memcpy(ipv6->dst_mac, frame, MRC_MAC_LEN);
    memcpy(ipv6->src_mac, frame + MRC_MAC_LEN, MRC_MAC_LEN);
    memcpy(ipv6->src_ip, ip + 8, MRC_IPV6_ADDR_LEN);
    memcpy(ipv6->dst_ip, ip + 24, MRC_IPV6_ADDR_LEN);
    ipv6->flow_label = read_be32(ip) & MRC_IPV6_FLOW_LABEL_MASK;
    ipv6->src_port = read_be16(prefix->udp);
    ipv6->dst_port = read_be16(prefix->udp + 2);
    uint8_t traffic_class = ip_traffic_class(ip, MRC_WIRE_IP6);
    ipv6->dscp = traffic_class >> 2;
    ipv6->ecn = traffic_class & 3;
    ipv6->hop_limit = ip[7];
}

static void unpack_header(const uint8_t *frame, const struct wire_prefix *prefix,
                           struct mrc_wire_packet *header)
{
    header->ip_version = prefix->ip_version;
    if (prefix->ip_version == MRC_WIRE_IP4)
        decode_ipv4(frame, prefix, &header->ipv4);
    else
        decode_ipv6(frame, prefix, &header->ipv6);
    decode_bth(prefix->udp + UDP_LEN, &header->bth);
}

int mrc_wire_dispatch_header(const uint8_t *frame, size_t length, struct mrc_wire_packet *header)
{
    if (!frame || !header)
        return -EINVAL;
    struct wire_prefix prefix;
    int error = decode_prefix(frame, length, MRC_WIRE_IP_UNSPECIFIED, &prefix);
    if (!error)
        unpack_header(frame, &prefix, header);
    return error;
}

static int decode_header(const uint8_t *frame, size_t length, enum mrc_wire_ip_version version,
                         struct wire_prefix *prefix)
{
    int error = decode_envelope(frame, length, version, prefix);
    if (error)
        return error;
    return read_be16(prefix->udp + 4) == prefix->physical_udp_len ? 0 : -EBADMSG;
}

int mrc_wire_decode_header(const uint8_t *frame, size_t length, struct mrc_wire_ipv4 *ipv4,
                           struct mrc_wire_bth *bth)
{
    if (!frame || !ipv4 || !bth)
        return -EINVAL;
    struct wire_prefix prefix;
    int error = decode_header(frame, length, MRC_WIRE_IP4, &prefix);
    if (error)
        return error;
    decode_ipv4(frame, &prefix, ipv4);
    decode_bth(prefix.udp + UDP_LEN, bth);
    return 0;
}

int mrc_wire_decode_ipv6_header(const uint8_t *frame, size_t length, struct mrc_wire_ipv6 *ipv6,
                                struct mrc_wire_bth *bth)
{
    if (!frame || !ipv6 || !bth)
        return -EINVAL;
    struct wire_prefix prefix;
    int error = decode_header(frame, length, MRC_WIRE_IP6, &prefix);
    if (error)
        return error;
    decode_ipv6(frame, &prefix, ipv6);
    decode_bth(prefix.udp + UDP_LEN, bth);
    return 0;
}

static int decode_roce_packet(const uint8_t *frame, const struct wire_prefix *prefix,
                              struct mrc_wire_packet *packet)
{
    struct mrc_wire_packet decoded = {0};
    unpack_header(frame, prefix, &decoded);
    const uint8_t *bth = prefix->udp + UDP_LEN;

    const uint8_t *body = bth + MRC_BTH_LEN;
    uint16_t nominal_udp_len = read_be16(prefix->udp + 4);
    uint16_t physical_udp_len = prefix->physical_udp_len;
    size_t body_len = physical_udp_len - UDP_LEN - MRC_BTH_LEN - MRC_ICRC_LEN;
    int error;
    if (mrc_wire_is_write(decoded.bth.opcode)) {
        if (physical_udp_len != nominal_udp_len)
            return -EBADMSG;
        uint8_t pad = (bth[1] >> 4) & 3;
        error = decode_write(body, body_len, pad, &decoded.bth, &decoded.body.write);
    } else {
        error = decode_control(body, body_len, &decoded, physical_udp_len, nominal_udp_len);
    }
    if (error)
        return error;
    *packet = decoded;
    return 0;
}

static int decode_packet(const uint8_t *frame, size_t length, enum mrc_wire_ip_version expected,
                         struct mrc_wire_packet *packet)
{
    if (!frame || !packet)
        return -EINVAL;

    struct wire_prefix prefix;
    int error = decode_envelope(frame, length, expected, &prefix);
    if (error)
        return error;
    return decode_roce_packet(frame, &prefix, packet);
}

int mrc_wire_decode(const uint8_t *frame, size_t length, struct mrc_wire_packet *packet)
{
    return decode_packet(frame, length, MRC_WIRE_IP4, packet);
}

int mrc_wire_decode_ip(const uint8_t *frame, size_t length, struct mrc_wire_packet *packet)
{
    return decode_packet(frame, length, MRC_WIRE_IP_UNSPECIFIED, packet);
}

/* RFC 9800 uses the RFC 8754 SRH layout. Each 128-bit list entry can
 * contain several compressed SIDs. Their forwarding semantics are not used
 * when the packet has reached its terminal transport endpoint.
 */
static int decode_srh(const uint8_t *srh, size_t available, size_t *header_len)
{
    if (available < SRH_FIXED_LEN)
        return -EBADMSG;
    if (srh[2] != SRH_ROUTING_TYPE)
        return -EPROTONOSUPPORT;

    size_t len = ((size_t)srh[1] + 1) * 8;
    size_t list_end = SRH_FIXED_LEN + ((size_t)srh[4] + 1) * MRC_IPV6_ADDR_LEN;
    if (len > available || list_end > len || (unsigned int)srh[3] > (unsigned int)srh[4] + 1)
        return -EBADMSG;
    if (srh[3] != 0)
        return -EPROTONOSUPPORT; /* Active transit segment. */

    /* Skip optional TLVs after ensuring that each is contained in the SRH. */
    for (size_t offset = list_end; offset < len;) {
        if (srh[offset] == SRH_PAD1) {
            ++offset;
            continue;
        }
        if (len - offset < 2)
            return -EBADMSG;
        size_t tlv_len = 2 + (size_t)srh[offset + 1];
        if (tlv_len > len - offset)
            return -EBADMSG;
        offset += tlv_len;
    }
    *header_len = len;
    return 0;
}

int mrc_wire_wrap_srv6(uint8_t *frame, size_t capacity, size_t inner_frame_len,
                        const struct mrc_wire_srv6_tx *route, size_t *length)
{
    /* Hdr Ext Len counts 8-octet units after the fixed SRH header. */
    if (!frame || !route || !length || route->flow_label > MRC_IPV6_FLOW_LABEL_MASK ||
        route->dscp > 63 || route->ecn > 3 || route->segment_count > UINT8_MAX / 2 ||
        (route->segment_count && !route->segment_list) ||
        route->segments_left > route->segment_count)
        return -EINVAL;
    if (inner_frame_len > capacity)
        return -EMSGSIZE;
    if (inner_frame_len < MRC_WIRE_IPV6_HEADER_LEN + MRC_BTH_LEN + MRC_ICRC_LEN)
        return -EBADMSG;

    struct wire_prefix prefix;
    int error = decode_prefix(frame, inner_frame_len, MRC_WIRE_IP6, &prefix);
    if (error)
        return error;
    size_t inner_ip_len = inner_frame_len - ETHERNET_LEN;
    if (inner_ip_len != IPV6_LEN + (size_t)read_be16(prefix.ip + 4))
        return -EBADMSG;

    size_t list_len = route->segment_count * MRC_IPV6_ADDR_LEN;
    size_t srh_len = list_len ? SRH_FIXED_LEN + list_len : 0;
    size_t added = IPV6_LEN + srh_len;
    if (inner_ip_len + srh_len > UINT16_MAX || added > capacity - inner_frame_len)
        return -EMSGSIZE;

    uint8_t *outer = frame + ETHERNET_LEN;
    memmove(outer + added, outer, inner_ip_len);
    uint8_t traffic_class = (uint8_t)((route->dscp << 2) | route->ecn);
    encode_ipv6_base(outer, route->flow_label, traffic_class, route->hop_limit,
                     srh_len ? IPPROTO_ROUTING : IPPROTO_IPV6_ENCAP,
                     (uint16_t)(inner_ip_len + srh_len));
    memcpy(outer + 8, route->outer_src_ip, MRC_IPV6_ADDR_LEN);
    memcpy(outer + 24, route->outer_dst_ip, MRC_IPV6_ADDR_LEN);
    if (srh_len) {
        uint8_t *srh = outer + IPV6_LEN;
        memset(srh, 0, SRH_FIXED_LEN);
        srh[0] = IPPROTO_IPV6_ENCAP;
        srh[1] = (uint8_t)(list_len / 8);
        srh[2] = SRH_ROUTING_TYPE;
        srh[3] = route->segments_left;
        srh[4] = (uint8_t)(route->segment_count - 1);
        memcpy(srh + SRH_FIXED_LEN, route->segment_list, list_len);
    }
    *length = inner_frame_len + added;
    return 0;
}

static bool has_srv6_envelope(const uint8_t *frame, size_t length)
{
    return length >= ETHERNET_LEN + 7 && frame_ip_version(frame) == MRC_WIRE_IP6 &&
           frame[ETHERNET_LEN + 6] != IPPROTO_UDP;
}

static int decode_srv6_envelope(const uint8_t *ip, size_t available, bool trimmed,
                                 struct wire_prefix *prefix, struct mrc_wire_srv6 *routing)
{
    if (available < IPV6_LEN)
        return -EBADMSG;
    if (ip[0] >> 4 != 6)
        return -EPROTONOSUPPORT;

    /* A trim switch may preserve or update the outer payload length. */
    size_t remaining = trimmed ? available - IPV6_LEN : read_be16(ip + 4);
    if (!remaining || remaining > available - IPV6_LEN)
        return -EBADMSG;
    const uint8_t *inner = ip + IPV6_LEN;
    uint8_t next = ip[6];
    size_t srh_len = 0;
    if (next == IPPROTO_ROUTING) {
        int error = decode_srh(inner, remaining, &srh_len);
        if (error)
            return error;
        next = inner[0];
        inner += srh_len;
        remaining -= srh_len;
    }

    enum mrc_wire_ip_version inner_version;
    if (next == IPPROTO_IPV4_ENCAP)
        inner_version = MRC_WIRE_IP4;
    else if (next == IPPROTO_IPV6_ENCAP)
        inner_version = MRC_WIRE_IP6;
    else
        return -EPROTONOSUPPORT;

    int error = decode_ip_prefix(inner, remaining, inner_version, prefix);
    if (error)
        return error;
    if (!trimmed) {
        error = validate_ip_envelope(prefix, remaining, true);
        if (error)
            return error;
    }

    uint8_t traffic_class = ip_traffic_class(ip, MRC_WIRE_IP6);
    routing->encapsulated = true;
    memcpy(routing->outer_src_ip, ip + 8, MRC_IPV6_ADDR_LEN);
    memcpy(routing->outer_dst_ip, ip + 24, MRC_IPV6_ADDR_LEN);
    routing->outer_dscp = traffic_class >> 2;
    routing->outer_ecn = traffic_class & 3;
    routing->outer_hop_limit = ip[7];
    routing->srh_len = (uint16_t)srh_len;
    return 0;
}

static int decode_routed_envelope(const uint8_t *frame, size_t length, struct wire_prefix *prefix,
                                  struct mrc_wire_srv6 *routing)
{
    if (has_srv6_envelope(frame, length))
        return decode_srv6_envelope(frame + ETHERNET_LEN, length - ETHERNET_LEN, false, prefix, routing);
    return decode_envelope(frame, length, MRC_WIRE_IP_UNSPECIFIED, prefix);
}

int mrc_wire_decode_srv6(const uint8_t *frame, size_t length, struct mrc_wire_packet *packet,
                         struct mrc_wire_srv6 *routing)
{
    if (!frame || !packet)
        return -EINVAL;

    struct mrc_wire_srv6 decoded_routing = {0};
    struct wire_prefix prefix;
    int error = decode_routed_envelope(frame, length, &prefix, &decoded_routing);
    if (!error)
        error = decode_roce_packet(frame, &prefix, packet);
    if (!error && routing)
        *routing = decoded_routing;
    return error;
}

int mrc_wire_decode_unsupported_opcode(const uint8_t *frame, size_t length,
                                        struct mrc_wire_packet *header, struct mrc_wire_srv6 *routing)
{
    if (!frame || !header || !routing)
        return -EINVAL;
    struct mrc_wire_srv6 decoded_routing = {0};
    struct wire_prefix prefix;
    int error = decode_routed_envelope(frame, length, &prefix, &decoded_routing);
    if (error)
        return error;
    const uint8_t *bth = prefix.udp + UDP_LEN;
    if (mrc_wire_is_write(bth[0]) || control_body_len(bth[0]))
        return -EINVAL;
    uint8_t pad = (bth[1] >> 4) & 3;
    if (read_be16(prefix.udp + 4) != prefix.physical_udp_len ||
        prefix.physical_udp_len < UDP_LEN + MRC_BTH_LEN + MRC_ICRC_LEN + pad)
        return -EBADMSG;
    unpack_header(frame, &prefix, header);
    *routing = decoded_routing;
    return 0;
}

static int decode_trimmed_write(const uint8_t *frame, size_t length, const struct wire_prefix *prefix,
                                struct mrc_wire_trimmed *packet)
{
    struct mrc_wire_trimmed decoded = { .ip_version = prefix->ip_version };
    const uint8_t *bth = prefix->udp + UDP_LEN;
    decode_bth(bth, &decoded.bth);
    if (!mrc_wire_is_write(decoded.bth.opcode))
        return -EBADMSG;
    if (prefix->ip_version == MRC_WIRE_IP4)
        decode_ipv4(frame, prefix, &decoded.ipv4);
    else
        decode_ipv6(frame, prefix, &decoded.ipv6);

    /* UET 1.0.1 sections 3.5.15.1 and 4.1. A switch may shorten the packet
     * without changing the original UDP length, and the iCRC may be absent.
     */
    decoded.original_udp_len = read_be16(prefix->udp + 4);
    size_t timestamp_offset = (size_t)(bth - frame) + MRC_BTH_LEN + MRC_METH_LEN;
    if (decoded.bth.timestamp && length >= timestamp_offset + MRC_TSETH_LEN) {
        const uint8_t *timestamp = frame + timestamp_offset;
        if ((timestamp[3] & 0xf) != TIMESTAMP_TYPE)
            return -EBADMSG;
        decoded.timestamp = true;
        decoded.tx_timestamp = read_be16(timestamp);
        decoded.tsr = (timestamp[2] & TIMESTAMP_RESOLUTION) != 0;
    }
    *packet = decoded;
    return 0;
}

static int decode_trimmed(const uint8_t *frame, size_t length, uint8_t trim_dscp,
                          enum mrc_wire_ip_version expected, struct mrc_wire_trimmed *packet)
{
    if (!frame || !packet || trim_dscp > 63)
        return -EINVAL;
    if (length < ETHERNET_LEN + 2)
        return -EBADMSG;

    enum mrc_wire_ip_version version = frame_ip_version(frame);
    if (!family_matches(version, expected) ||
        ip_traffic_class(frame + ETHERNET_LEN, version) >> 2 != trim_dscp)
        return -EPROTONOSUPPORT;

    struct wire_prefix prefix;
    int error = decode_prefix(frame, length, version, &prefix);
    if (error)
        return error;

    return decode_trimmed_write(frame, length, &prefix, packet);
}

int mrc_wire_decode_trimmed(const uint8_t *frame, size_t length, uint8_t trim_dscp,
                            struct mrc_wire_trimmed *packet)
{
    return decode_trimmed(frame, length, trim_dscp, MRC_WIRE_IP4, packet);
}

int mrc_wire_decode_trimmed_ip(const uint8_t *frame, size_t length, uint8_t trim_dscp,
                               struct mrc_wire_trimmed *packet)
{
    return decode_trimmed(frame, length, trim_dscp, MRC_WIRE_IP_UNSPECIFIED, packet);
}

int mrc_wire_decode_trimmed_srv6(const uint8_t *frame, size_t length, uint8_t trim_dscp,
                                 struct mrc_wire_trimmed *packet, struct mrc_wire_srv6 *routing)
{
    if (!frame || !packet || trim_dscp > 63)
        return -EINVAL;

    struct mrc_wire_srv6 decoded_routing = {0};
    int error;
    if (has_srv6_envelope(frame, length)) {
        const uint8_t *outer = frame + ETHERNET_LEN;
        if ((ip_traffic_class(outer, MRC_WIRE_IP6) >> 2) != trim_dscp)
            return -EPROTONOSUPPORT;

        struct wire_prefix prefix;
        error = decode_srv6_envelope(outer, length - ETHERNET_LEN, true, &prefix, &decoded_routing);
        if (error)
            return error;
        error = decode_trimmed_write(frame, length, &prefix, packet);
    } else {
        error = mrc_wire_decode_trimmed_ip(frame, length, trim_dscp, packet);
    }
    if (!error && routing)
        *routing = decoded_routing;
    return error;
}
