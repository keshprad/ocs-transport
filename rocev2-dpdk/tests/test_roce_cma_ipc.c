#include "roce_cma_ipc.h"
#include "roce_fd.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>

int main(void)
{
    int peer[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, peer) == 0);

    struct roce_cma_message message = {
        .version = ROCE_CMA_IPC_VERSION, .type = ROCE_CMA_CONNECT,
        .qp_num = 42, .private_length = 1, .private_data = {0xa5},
    };
    uint8_t record[sizeof(message)];
    assert(roce_cma_send(peer[0], &message) == 0);
    assert(recv(peer[1], record, sizeof(record), 0) == 56);
    assert(send(peer[1], record, 56, 0) == 56);

    struct roce_cma_message received;
    assert(roce_cma_receive(peer[0], &received, false) == 1);
    assert(received.type == ROCE_CMA_CONNECT && received.qp_num == 42);
    assert(received.private_length == 1 && received.private_data[0] == 0xa5);
    record[55] = 1;
    assert(send(peer[1], record, 56, 0) == 56);
    assert(roce_cma_receive(peer[0], &received, false) == -EPROTO);

    message.type = ROCE_CMA_REJECTED;
    message.status = ROCE_CM_REJ_CONSUMER_DEFINED;
    message.private_length = ROCE_CM_REJ_PRIVATE_SIZE;
    for (size_t i = 0; i < message.private_length; ++i)
        message.private_data[i] = (uint8_t)i;
    assert(roce_cma_send(peer[0], &message) == 0);
    assert(roce_cma_receive(peer[1], &received, false) == 1);
    assert(received.type == message.type && received.status == 28);
    assert(received.private_length == message.private_length);
    assert(!memcmp(received.private_data, message.private_data, message.private_length));

    assert(roce_cma_send_fd(peer[0], &message, peer[1]) == 0);
    int fd;
    assert(roce_cma_receive_fd(peer[1], &received, false, &fd) == -EPROTO && fd == -1);
    assert(send(peer[0], &message, 56, 0) == 56);
    assert(roce_cma_receive(peer[1], &received, false) == -EPROTO);

    message.type = ROCE_CMA_CONFIGURE_QP;
    message.status = 0;
    memset(message.private_data, 0, sizeof(message.private_data));
    message.private_length = 0;
    assert(roce_cma_send_fd(peer[0], &message, peer[1]) == 0);
    assert(roce_cma_receive_fd(peer[1], &received, false, &fd) == 1 && fd >= 0);
    assert(received.type == message.type && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    close(fd);
    close(peer[0]);
    close(peer[1]);
    puts("RoCEv2 CM Unix records and descriptors verified");
}
