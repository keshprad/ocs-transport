#ifndef OCS_ROCE_ICRC_H
#define OCS_ROCE_ICRC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__AARCH64EL__) && defined(__ARM_FEATURE_CRC32) && !defined(OCS_ROCE_PORTABLE_CRC)
#include <arm_acle.h>

static inline uint64_t roce_crc_load_word(const uint8_t *bytes)
{
    uint64_t word;
    memcpy(&word, bytes, sizeof(word));
    return word;
}

#if defined(OCS_ROCE_PMULL_ICRC) && defined(__ARM_FEATURE_AES)
#include <arm_neon.h>

enum { ROCE_CRC_LANE_BYTES = 256, ROCE_CRC_BLOCK_BYTES = 4 * ROCE_CRC_LANE_BYTES };

static inline uint32_t roce_crc32_block(uint32_t crc, const uint8_t *bytes)
{
    uint32_t a = crc, b = 0, c = 0, d = 0;
    for (size_t offset = 0; offset < ROCE_CRC_LANE_BYTES; offset += sizeof(uint64_t)) {
        a = __crc32d(a, roce_crc_load_word(bytes + offset));
        b = __crc32d(b, roce_crc_load_word(bytes + offset + ROCE_CRC_LANE_BYTES));
        c = __crc32d(c, roce_crc_load_word(bytes + offset + 2 * ROCE_CRC_LANE_BYTES));
        d = __crc32d(d, roce_crc_load_word(bytes + offset + 3 * ROCE_CRC_LANE_BYTES));
    }
    /*
     * Shift the first three raw CRCs by their remaining byte counts.
     * For n remaining bytes the factor is reverse32(x^(8*n - 32) mod P),
     * where P is the IEEE CRC-32 polynomial. The products are reversed
     * over 63 bits, so shifting left once supplies the CRC32X input.
     * CRC32X includes the final x^32 factor and reduces modulo P.
     */
    uint64_t folded = (uint64_t)vmull_p64(a, UINT32_C(0x0eb38450)) ^
                      (uint64_t)vmull_p64(b, UINT32_C(0xeba0f9ae)) ^
                      (uint64_t)vmull_p64(c, UINT32_C(0x99168a18));
    return __crc32d(0, folded << 1) ^ d;
}
#endif
#endif

static inline uint32_t roce_crc32_update(uint32_t crc, const uint8_t *bytes, size_t len)
{
#if defined(__AARCH64EL__) && defined(__ARM_FEATURE_CRC32) && !defined(OCS_ROCE_PORTABLE_CRC)
#if defined(OCS_ROCE_PMULL_ICRC) && defined(__ARM_FEATURE_AES)
    while (len >= ROCE_CRC_BLOCK_BYTES) {
        crc = roce_crc32_block(crc, bytes);
        bytes += ROCE_CRC_BLOCK_BYTES;
        len -= ROCE_CRC_BLOCK_BYTES;
    }
#endif
    while (len >= sizeof(uint64_t)) {
        crc = __crc32d(crc, roce_crc_load_word(bytes));
        bytes += sizeof(uint64_t);
        len -= sizeof(uint64_t);
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
