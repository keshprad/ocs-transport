#define _GNU_SOURCE
#include "mrc_verbs_async.h"
#include "mrc_port_netlink.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <rdma/ib_user_ioctl_cmds.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/rdma_user_ioctl_cmds.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int mrc_verbs_async_open(const struct ibv_context *verbs)
{
    if (!verbs || !verbs->device)
        return -EINVAL;
    int index = ibv_get_device_index(verbs->device);
    if (index < 0)
        return -EOPNOTSUPP;
    uint32_t driver_id;
    int result = mrc_port_netlink_uverbs_driver_id((uint32_t)index, &driver_id);
    if (result)
        return result;

    struct ib_uverbs_attr attr = {
        .attr_id = UVERBS_ATTR_ASYNC_EVENT_ALLOC_FD_HANDLE,
        .flags = UVERBS_ATTR_F_MANDATORY,
        .data_s64 = -1,
    };
    _Alignas(struct ib_uverbs_ioctl_hdr) unsigned char command[
        sizeof(struct ib_uverbs_ioctl_hdr) + sizeof(attr)];
    const struct ib_uverbs_ioctl_hdr header = {
        .length = sizeof(command),
        .object_id = UVERBS_OBJECT_ASYNC_EVENT,
        .method_id = UVERBS_METHOD_ASYNC_EVENT_ALLOC,
        .num_attrs = 1,
        .driver_id = driver_id,
    };
    const size_t attr_offset = offsetof(struct ib_uverbs_ioctl_hdr, attrs);
    memcpy(command, &header, sizeof(header));
    memcpy(command + attr_offset, &attr, sizeof(attr));
    if (ioctl(verbs->cmd_fd, RDMA_VERBS_IOCTL, command) < 0)
        return -errno;
    memcpy(&attr, command + attr_offset, sizeof(attr));
    if (attr.data_s64 < 0 || attr.data_s64 > INT_MAX)
        return -EBADMSG;

    /* uverbs installs the new descriptor with close-on-exec already set. */
    int fd = (int)attr.data_s64;
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0)
        return fd;
    result = -errno;
    close(fd);
    return result;
}

int mrc_verbs_async_read(int fd, enum ibv_event_type *type, uint64_t *element)
{
    if (!type || !element)
        return -EINVAL;
    struct ib_uverbs_async_event_desc event;
    ssize_t received;
    do {
        /* Avoid a libc cancellation point while the caller holds its mutex. */
        received = syscall(SYS_read, fd, &event, sizeof(event));
    } while (received < 0 && errno == EINTR);
    if (received < 0)
        return -errno;
    if (!received)
        return -ENODEV;
    if (received != sizeof(event) || event.reserved)
        return -EBADMSG;

    *type = (enum ibv_event_type)event.event_type;
    *element = event.element;
    return 0;
}
