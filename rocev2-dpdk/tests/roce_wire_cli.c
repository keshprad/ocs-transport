#include "roce_wire.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const uint8_t payload[] = {0x24, 0x35, 0x46, 0x57, 0x68};
    uint8_t frame[ROCE_MAX_FRAME + 1];
    if (argc == 2 && strcmp(argv[1], "decode") == 0) {
        size_t length = fread(frame, 1, sizeof(frame), stdin);
        struct roce_packet packet;
        if (!roce_decode(&packet, frame, length))
            return 1;
        printf("%u %u %u %u %llu %u %u %u %u %zu\n",
               (unsigned int)packet.opcode, packet.qpn, packet.psn, packet.ack_requested,
               (unsigned long long)packet.remote_address, packet.rkey, packet.write_length,
               packet.syndrome, packet.msn, packet.payload_length);
        return 0;
    }
    if (argc == 2 && argv[1][0] >= '0' && argv[1][0] <= '4' && !argv[1][1]) {
        enum roce_opcode opcodes[] = {
            ROCE_WRITE_ONLY, ROCE_WRITE_FIRST, ROCE_WRITE_MIDDLE, ROCE_WRITE_LAST, ROCE_ACK,
        };
        enum roce_opcode opcode = opcodes[argv[1][0] - '0'];
        struct roce_packet packet = {
            .src = { .mac = {2, 0, 0, 0, 0, 1}, .ipv4 = 0xc0000201 },
            .dst = { .mac = {2, 0, 0, 0, 0, 2}, .ipv4 = 0xc0000202 },
            .udp_source = 0xc123, .pkey = ROCE_PKEY, .qpn = 0x123456, .psn = 0xabcdef,
            .opcode = opcode, .ack_requested = opcode != ROCE_ACK,
            .remote_address = UINT64_C(0xfedcba9876543210), .rkey = 0x10a0b0c0, .write_length = 5,
            .syndrome = ROCE_NAK_SEQUENCE, .msn = 0xc0ffee, .payload = payload,
            .payload_length = opcode == ROCE_ACK ? 0 : sizeof(payload),
        };
        size_t length = roce_encode(frame, sizeof(frame), &packet);
        return length && fwrite(frame, 1, length, stdout) == length ? 0 : 1;
    }
    return 2;
}
