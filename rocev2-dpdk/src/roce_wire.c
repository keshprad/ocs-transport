#include "roce_wire.h"

#include "../../common/roce_icrc.h"

enum { ETH = 14, IP = 20, UDP = 8, BTH = 12, RETH = 16, AETH = 4, DETH = 8, ICRC = 4 };
enum { CM_QPN = 1 };
static const uint32_t cm_qkey = UINT32_C(0x80010000);

static uint32_t read_be(const uint8_t *bytes, size_t width)
{
    uint32_t value = 0;
    for (size_t i = 0; i < width; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

static void write_be(uint8_t *bytes, size_t width, uint32_t value)
{
    while (width) {
        bytes[--width] = (uint8_t)value;
        value >>= 8;
    }
}

static uint16_t internet_checksum(const uint8_t *bytes, size_t length,
                                  uint32_t sum)
{
    while (length >= 2) {
        sum += read_be(bytes, 2);
        bytes += 2;
        length -= 2;
    }
    if (length)
        sum += (uint32_t)*bytes << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static size_t extension_size(enum roce_opcode opcode, bool cm)
{
    if (cm)
        return opcode == ROCE_CM_SEND_ONLY ? DETH : SIZE_MAX;

    switch (opcode) {
    case ROCE_WRITE_FIRST:
    case ROCE_WRITE_ONLY:
        return RETH;
    case ROCE_WRITE_MIDDLE:
    case ROCE_WRITE_LAST:
        return 0;
    case ROCE_ACK:
        return AETH;
    case ROCE_CM_SEND_ONLY:
        break;
    }
    return SIZE_MAX;
}

static bool valid_packet(const struct roce_packet *p)
{
    return p->payload_length <= ROCE_MAX_PMTU && (!p->payload_length || p->payload) &&
           p->udp_source && p->qpn <= ROCE_PSN_MASK && p->psn <= ROCE_PSN_MASK &&
           p->msn <= ROCE_PSN_MASK && (p->opcode != ROCE_ACK || !p->payload_length);
}

static void encode_header(uint8_t *frame, const struct roce_packet *p, size_t extension,
                          size_t pad, size_t length)
{
    memcpy(frame, p->dst.mac, ROCE_MAC_LEN);
    memcpy(frame + ROCE_MAC_LEN, p->src.mac, ROCE_MAC_LEN);
    write_be(frame + 12, 2, 0x0800);
    uint8_t *ip = frame + ETH;
    memset(ip, 0, IP + UDP + BTH);
    ip[0] = 0x45;
    write_be(ip + 2, 2, (uint32_t)(length - ETH));
    ip[6] = 0x40;
    ip[8] = 64;
    ip[9] = 17;
    write_be(ip + 12, 4, p->src.ipv4);
    write_be(ip + 16, 4, p->dst.ipv4);
    write_be(ip + 10, 2, internet_checksum(ip, IP, 0));

    uint8_t *udp = ip + IP;
    write_be(udp, 2, p->udp_source);
    write_be(udp + 2, 2, ROCE_UDP_PORT);
    write_be(udp + 4, 2, (uint32_t)(length - ETH - IP));

    uint8_t *bth = udp + UDP;
    bth[0] = (uint8_t)p->opcode;
    bth[1] = (uint8_t)(pad << 4);
    write_be(bth + 2, 2, p->pkey);
    write_be(bth + 5, 3, p->qpn);
    bth[8] = p->ack_requested ? 0x80 : 0;
    write_be(bth + 9, 3, p->psn);

    uint8_t *ext = bth + BTH;
    if (extension == RETH) {
        write_be(ext, 4, (uint32_t)(p->remote_address >> 32));
        write_be(ext + 4, 4, (uint32_t)p->remote_address);
        write_be(ext + 8, 4, p->rkey);
        write_be(ext + 12, 4, p->write_length);
    } else if (extension == AETH) {
        ext[0] = p->syndrome;
        write_be(ext + 1, 3, p->msn);
    } else if (extension == DETH) {
        write_be(ext, 4, cm_qkey);
        write_be(ext + 4, 4, CM_QPN);
    }
}

static void encode_icrc(uint8_t *output, uint32_t crc)
{
    for (size_t i = 0; i < ICRC; ++i)
        output[i] = (uint8_t)(crc >> (8 * i));
}

static size_t encode_packet(uint8_t *frame, size_t capacity, const struct roce_packet *p,
                            size_t extension)
{
    if (extension == SIZE_MAX || !valid_packet(p))
        return 0;
    size_t pad = (4 - p->payload_length % 4) % 4;
    size_t header = ETH + IP + UDP + BTH + extension;
    size_t length = header + p->payload_length + pad + ICRC;
    if (capacity < length)
        return 0;

    encode_header(frame, p, extension, pad, length);
    if (p->payload_length)
        memcpy(frame + header, p->payload, p->payload_length);
    memset(frame + length - ICRC - pad, 0, pad);
    encode_icrc(frame + length - ICRC, roce_icrc(frame + ETH, IP, length - ETH - ICRC));
    return length;
}

size_t roce_encode(uint8_t *frame, size_t capacity, const struct roce_packet *p)
{
    return encode_packet(frame, capacity, p, extension_size(p->opcode, false));
}

bool roce_encode_split(uint8_t header[ROCE_MAX_HEADER], uint8_t trailer[ROCE_MAX_TRAILER],
                       const struct roce_packet *p, uint16_t *header_length, uint16_t *trailer_length)
{
    size_t extension = extension_size(p->opcode, false);
    if (extension == SIZE_MAX || !valid_packet(p))
        return false;
    size_t pad = (4 - p->payload_length % 4) % 4;
    *header_length = ETH + IP + UDP + BTH + extension;
    *trailer_length = (uint16_t)(pad + ICRC);
    encode_header(header, p, extension, pad, *header_length + p->payload_length + *trailer_length);
    memset(trailer, 0, pad);
    uint32_t crc = roce_icrc_seed(header + ETH, IP);
    crc = roce_crc32_update(crc, header + ETH + IP + UDP + BTH, extension);
    if (p->payload_length)
        crc = roce_crc32_update(crc, p->payload, p->payload_length);
    encode_icrc(trailer + pad, ~roce_crc32_update(crc, trailer, pad));
    return true;
}

static bool decode_packet(struct roce_packet *packet, const uint8_t *frame, size_t frame_length,
                          bool cm)
{
    if (frame_length < ETH + IP + UDP + BTH + ICRC || read_be(frame + 12, 2) != 0x0800)
        return false;
    const uint8_t *ip = frame + ETH;
    size_t ip_length = read_be(ip + 2, 2);
    if (ip[0] != 0x45 || ip[9] != 17 || (read_be(ip + 6, 2) & 0xbfff) ||
        ip_length < IP + UDP + BTH + ICRC || ip_length > frame_length - ETH ||
        ip_length > ROCE_MAX_FRAME - ETH || internet_checksum(ip, IP, 0))
        return false;

    const uint8_t *udp = ip + IP;
    if (read_be(udp + 2, 2) != ROCE_UDP_PORT || read_be(udp + 4, 2) != ip_length - IP)
        return false;
    if (read_be(udp + 6, 2)) {
        uint32_t pseudo = read_be(ip + 12, 2) + read_be(ip + 14, 2) +
                          read_be(ip + 16, 2) + read_be(ip + 18, 2) +
                          17 + (uint32_t)(ip_length - IP);
        if (internet_checksum(udp, ip_length - IP, pseudo))
            return false;
    }

    const uint8_t *bth = udp + UDP;
    enum roce_opcode opcode = (enum roce_opcode)bth[0];
    size_t extension = extension_size(opcode, cm);
    size_t pad = (bth[1] >> 4) & 3;
    size_t body = ip_length - (IP + UDP + BTH + ICRC);
    if ((body & 3) || (bth[1] & 0x0f) || extension == SIZE_MAX || extension + pad > body)
        return false;
    size_t payload_length = body - extension - pad;
    if (payload_length > ROCE_MAX_PMTU || (opcode == ROCE_ACK && (payload_length || pad)))
        return false;
    const uint8_t *ext = bth + BTH;
    /* QP1 uses UD, so the AckReq bit has no effect. Linux RXE can set it on GSI. */
    if (cm && (payload_length != ROCE_CM_MAD_SIZE || pad ||
               read_be(bth + 5, 3) != CM_QPN || read_be(ext, 4) != cm_qkey ||
               read_be(ext + 5, 3) != CM_QPN))
        return false;

    const uint8_t *crc_bytes = ip + ip_length - ICRC;
    uint32_t crc = 0;
    for (size_t i = 0; i < ICRC; ++i)
        crc |= (uint32_t)crc_bytes[i] << (8 * i);
    if (crc != roce_icrc(ip, IP, ip_length - ICRC))
        return false;

    struct roce_packet p = {
        .src.ipv4 = read_be(ip + 12, 4), .dst.ipv4 = read_be(ip + 16, 4),
        .udp_source = (uint16_t)read_be(udp, 2), .pkey = (uint16_t)read_be(bth + 2, 2),
        .qpn = read_be(bth + 5, 3), .psn = read_be(bth + 9, 3), .opcode = opcode,
        .ack_requested = (bth[8] & 0x80) != 0,
        .payload = bth + BTH + extension, .payload_length = payload_length,
    };
    memcpy(p.dst.mac, frame, ROCE_MAC_LEN);
    memcpy(p.src.mac, frame + ROCE_MAC_LEN, ROCE_MAC_LEN);
    if (extension == RETH) {
        p.remote_address = (uint64_t)read_be(ext, 4) << 32 | read_be(ext + 4, 4);
        p.rkey = read_be(ext + 8, 4);
        p.write_length = read_be(ext + 12, 4);
    } else if (extension == AETH) {
        p.syndrome = ext[0];
        p.msn = read_be(ext + 1, 3);
    }
    *packet = p;
    return true;
}

bool roce_decode(struct roce_packet *packet, const uint8_t *frame, size_t frame_length)
{
    return decode_packet(packet, frame, frame_length, false);
}

size_t roce_cm_frame_encode(uint8_t *frame, size_t capacity, const struct roce_cm_frame *cm)
{
    if (!frame || !cm)
        return 0;

    const struct roce_packet packet = {
        .src = cm->src, .dst = cm->dst, .udp_source = cm->udp_source, .pkey = cm->pkey,
        .qpn = CM_QPN, .psn = cm->psn, .opcode = ROCE_CM_SEND_ONLY,
        .payload = cm->mad, .payload_length = ROCE_CM_MAD_SIZE,
    };
    return encode_packet(frame, capacity, &packet, DETH);
}

bool roce_cm_frame_decode(struct roce_cm_frame *cm, const uint8_t *frame, size_t frame_length)
{
    struct roce_packet packet;

    if (!cm || !frame || !decode_packet(&packet, frame, frame_length, true))
        return false;

    *cm = (struct roce_cm_frame) {
        .src = packet.src, .dst = packet.dst, .udp_source = packet.udp_source,
        .pkey = packet.pkey, .psn = packet.psn, .mad = packet.payload,
    };
    return true;
}
