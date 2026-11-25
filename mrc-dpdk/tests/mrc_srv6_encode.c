/* Exercise the production envelope encoder with independently built inner
 * packets and prepacked NEXT-CSID containers. Binary frames use stdin/stdout. */
#include "mrc_wire.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    uint8_t frame[4096], segments[8][MRC_IPV6_ADDR_LEN];
    size_t length = fread(frame, 1, sizeof(frame), stdin);
    if (ferror(stdin) || !feof(stdin))
        return 2;
    if (argc == 2 && !strcmp(argv[1], "decode")) {
        struct mrc_wire_packet packet;
        struct mrc_wire_srv6 routing;
        int result = mrc_wire_decode_srv6(frame, length, &packet, &routing);
        if (result)
            fprintf(stderr, "Terminal SRv6 decode failed: %d\n", result);
        return result ? 1 : 0;
    }
    if (argc < 3 || argc > 11)
        return 2;
    struct mrc_wire_srv6_tx route = {
        .hop_limit = 64, .flow_label = 0x12345, .dscp = 10, .ecn = 2,
        .segment_count = (size_t)argc - 3,
        .segment_list = segments,
    };
    char *end;
    unsigned long left = strtoul(argv[2], &end, 10);
    if (*end || left > 255 || inet_pton(AF_INET6, argv[1], route.outer_dst_ip) != 1 ||
        inet_pton(AF_INET6, "2001:db8::feed", route.outer_src_ip) != 1)
        return 2;
    route.segments_left = (uint8_t)left;
    for (int i = 3; i < argc; i++) {
        if (inet_pton(AF_INET6, argv[i], segments[i - 3]) != 1)
            return 2;
    }
    int result = mrc_wire_wrap_srv6(frame, sizeof(frame), length, &route, &length);
    if (result) {
        fprintf(stderr, "SRv6 encode failed: %d\n", result);
        return 1;
    }
    return fwrite(frame, 1, length, stdout) == length ? 0 : 1;
}
