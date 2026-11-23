#ifndef OCS_ROCE_ICRC_H
#define OCS_ROCE_ICRC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__AARCH64EL__) && defined(__ARM_FEATURE_CRC32) && !defined(OCS_ROCE_PORTABLE_CRC)
#include <arm_acle.h>
#endif

static inline uint32_t roce_crc32_update(uint32_t crc, const uint8_t *bytes, size_t len)
{
#if defined(__AARCH64EL__) && defined(__ARM_FEATURE_CRC32) && !defined(OCS_ROCE_PORTABLE_CRC)
    while (len >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, bytes, sizeof(word));
        crc = __crc32d(crc, word);
        bytes += sizeof(word);
        len -= sizeof(word);
    }
    for (size_t i = 0; i < len; ++i)
        crc = __crc32b(crc, bytes[i]);
#else
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
#endif
    return crc;
}

enum { ROCE_ICRC_UDP_LEN = 8, ROCE_ICRC_BTH_LEN = 12 };

static inline uint32_t roce_icrc_seed(const uint8_t *ip, size_t ip_header_len)
{
    enum { IPV4_LEN = 20, IPV6_LEN = 40 };
    uint8_t masked[IPV6_LEN + ROCE_ICRC_UDP_LEN + ROCE_ICRC_BTH_LEN];
    size_t prefix_len = ip_header_len + ROCE_ICRC_UDP_LEN + ROCE_ICRC_BTH_LEN;
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
    masked[ip_header_len + ROCE_ICRC_UDP_LEN + 4] = 0xff; /* BTH variant reserved field */
    /* This seed includes the eight all-ones bytes of the masked IB LRH. */
    return roce_crc32_update(UINT32_C(0xdebb20e3), masked, prefix_len);
}

static inline uint32_t roce_icrc(const uint8_t *ip, size_t ip_header_len, size_t protected_len)
{
    size_t prefix_len = ip_header_len + ROCE_ICRC_UDP_LEN + ROCE_ICRC_BTH_LEN;
    uint32_t crc = roce_icrc_seed(ip, ip_header_len);
    return ~roce_crc32_update(crc, ip + prefix_len, protected_len - prefix_len);
}

#endif
