#include "mrc_abi.h"
#include "mrc_verbs_async.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

struct mrc_abi_async_notice {
    struct mrc_abi_async_notice *next;
    struct mrc_abi_async_source *source;
    struct mrc_async_event event;
};

static int attach_native_channel(struct mrc_abi_async_channel *channel, const struct ibv_context *verbs)
{
    if (verbs->cmd_fd < 0 || verbs->async_fd < 0)
        return 0;
    /* With the default verbs event file installed, this opens an independent stream. */
    int fd = mrc_verbs_async_open(verbs);
    switch (fd) {
    case -EINVAL:
    case -ENOSYS:
    case -ENOTTY:
    case -EOPNOTSUPP:
    case -EPROTONOSUPPORT:
    case -EACCES:
    case -EPERM:
        return 0;
    default:
        if (fd < 0)
            return -fd;
        break;
    }
    channel->native_fd = fd;
    channel->poll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (channel->poll_fd < 0)
        return errno;
    struct epoll_event readiness = { .events = EPOLLIN };
    if (epoll_ctl(channel->poll_fd, EPOLL_CTL_ADD, channel->fd, &readiness) < 0 ||
        epoll_ctl(channel->poll_fd, EPOLL_CTL_ADD, channel->native_fd, &readiness) < 0)
        return errno;
    channel->native_active = true;
    return 0;
}

int mrc_abi_async_open(struct mrc_abi_async_channel *channel, const struct ibv_context *verbs,
                       uint8_t port_count)
{
    channel->fd = channel->poll_fd = channel->native_fd = -1;
    channel->port_count = port_count;
    int error = pthread_mutex_init(&channel->mutex, NULL);
    if (error)
        return error;
    error = pthread_cond_init(&channel->acked, NULL);
    if (error)
        goto destroy_mutex;
    channel->fd = eventfd(0, EFD_CLOEXEC);
    error = channel->fd < 0 ? errno : attach_native_channel(channel, verbs);
    if (error)
        mrc_abi_async_close(channel);
    return error;

destroy_mutex:
    pthread_mutex_destroy(&channel->mutex);
    return error;
}

void mrc_abi_async_close(struct mrc_abi_async_channel *channel)
{
    if (channel->poll_fd >= 0)
        close(channel->poll_fd);
    if (channel->native_fd >= 0)
        close(channel->native_fd);
    if (channel->fd >= 0)
        close(channel->fd);
    pthread_cond_destroy(&channel->acked);
    pthread_mutex_destroy(&channel->mutex);
}

int mrc_abi_async_prepare(struct mrc_abi_async_source *source)
{
    struct mrc_abi_async_channel *channel = &source->context->async;
    pthread_mutex_lock(&channel->mutex);
    if (!source->reserved)
        source->reserved = malloc(sizeof(*source->reserved));
    int result = source->reserved ? 0 : ENOMEM;
    pthread_mutex_unlock(&channel->mutex);
    return result;
}

/* The eventfd is level-triggered. Only the empty/nonempty transitions change
 * the counter, and the queue lock serializes those transitions and retrieval. */
static int set_ready(int fd, bool ready)
{
    /* Only these non-waiting operations run under the queue lock. Keep
     * pthread cancellation at the getter's poll, before it owns an event. */
    long status;
    eventfd_t value = 1;
    do {
        status = syscall(ready ? SYS_write : SYS_read, fd, &value, sizeof(value));
    } while (status < 0 && errno == EINTR);
    return status < 0 ? errno : 0;
}

void mrc_abi_async_publish(struct mrc_abi_async_source *source, const struct mrc_async_event *event)
{
    struct mrc_abi_async_channel *channel = &source->context->async;
    pthread_mutex_lock(&channel->mutex);
    struct mrc_abi_async_notice *notice = source->reserved;
    if (notice && (channel->head || set_ready(channel->fd, true) == 0)) {
        source->reserved = NULL;
        *notice = (struct mrc_abi_async_notice){ .source = source, .event = *event };
        if (channel->tail)
            channel->tail->next = notice;
        else
            channel->head = notice;
        channel->tail = notice;
    }
    pthread_mutex_unlock(&channel->mutex);
}

void mrc_abi_async_finish(struct mrc_abi_async_source *source)
{
    struct mrc_abi_async_channel *channel = &source->context->async;
    pthread_mutex_lock(&channel->mutex);
    struct mrc_abi_async_notice *previous = NULL;
    struct mrc_abi_async_notice **link = &channel->head;
    bool was_ready = channel->head != NULL;
    while (*link) {
        struct mrc_abi_async_notice *notice = *link;
        if (notice->source == source) {
            *link = notice->next;
            if (channel->tail == notice)
                channel->tail = previous;
            free(notice);
        } else {
            previous = notice;
            link = &notice->next;
        }
    }
    if (was_ready && !channel->head)
        set_ready(channel->fd, false);
    free(source->reserved);
    source->reserved = NULL;
    while (source->unacked)
        pthread_cond_wait(&channel->acked, &channel->mutex);
    pthread_mutex_unlock(&channel->mutex);
}

static int take_notice(struct mrc_abi_async_channel *channel, struct mrc_async_event *event)
{
    pthread_mutex_lock(&channel->mutex);
    struct mrc_abi_async_notice *notice = channel->head;
    int error = EAGAIN;
    if (notice) {
        error = notice->source->unacked == SIZE_MAX ? EOVERFLOW : 0;
        if (!error && !notice->next)
            error = set_ready(channel->fd, false);
        if (!error) {
            channel->head = notice->next;
            if (!channel->head)
                channel->tail = NULL;
            notice->source->unacked++;
            *event = notice->event;
            free(notice);
        }
    }
    pthread_mutex_unlock(&channel->mutex);
    return error;
}

static bool native_port_event(enum ibv_event_type type)
{
    switch (type) {
    case IBV_EVENT_PORT_ACTIVE:
    case IBV_EVENT_PORT_ERR:
    case IBV_EVENT_LID_CHANGE:
    case IBV_EVENT_PKEY_CHANGE:
    case IBV_EVENT_SM_CHANGE:
    case IBV_EVENT_CLIENT_REREGISTER:
    case IBV_EVENT_GID_CHANGE:
        return true;
    default:
        return false;
    }
}

static int take_native_notice(struct mrc_abi_async_channel *channel, struct mrc_async_event *event)
{
    pthread_mutex_lock(&channel->mutex);
    int error = EAGAIN;
    while (channel->native_active) {
        enum ibv_event_type type;
        uint64_t element = 0;
        int result = mrc_verbs_async_read(channel->native_fd, &type, &element);
        if (result == -ENODEV || result == -ENXIO || result == -EIO) {
            epoll_ctl(channel->poll_fd, EPOLL_CTL_DEL, channel->native_fd, NULL);
            channel->native_active = false;
            type = IBV_EVENT_DEVICE_FATAL;
        } else if (result) {
            error = -result;
            break;
        }

        if (type == IBV_EVENT_DEVICE_FATAL) {
            if (channel->native_fatal_seen)
                continue;
            channel->native_fatal_seen = true;
            *event = (struct mrc_async_event){ .event_type = type };
        } else if (native_port_event(type) && element && element <= channel->port_count) {
            *event = (struct mrc_async_event){ .event_type = type, .element.port_num = (int)element };
        } else {
            continue;
        }
        error = 0;
        break;
    }
    pthread_mutex_unlock(&channel->mutex);
    return error;
}

static int async_wait_fd(const struct mrc_abi_async_channel *channel)
{
    return channel->poll_fd >= 0 ? channel->poll_fd : channel->fd;
}

static int wait_notice(struct mrc_abi_async_channel *channel, struct mrc_async_event *event)
{
    int fd = async_wait_fd(channel);
    for (;;) {
        int error = take_notice(channel, event);
        if (error == EAGAIN)
            error = take_native_notice(channel, event);
        if (error != EAGAIN)
            return error;

        int flags = fcntl(fd, F_GETFL);
        if (flags < 0)
            return errno;
        if (flags & O_NONBLOCK)
            return EAGAIN;

        struct pollfd wait = { .fd = fd, .events = POLLIN };
        if (poll(&wait, 1, -1) < 0)
            return errno;
        if (!(wait.revents & POLLIN))
            return EIO;
    }
}

static void release_waiter(void *context)
{
    mrc_abi_release_context(context);
}

MRC_PUBLIC int mrc_get_async_event(struct mrc_context *context, struct mrc_async_event *event)
{
    int error = context && event ? mrc_abi_retain_context(context) : EINVAL;
    if (!error) {
        pthread_cleanup_push(release_waiter, context);
        error = wait_notice(&context->async, event);
        pthread_cleanup_pop(1);
    }
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

MRC_PUBLIC void mrc_ack_async_event(struct mrc_async_event *event)
{
    if (!event)
        return;
    struct mrc_abi_async_source *source;
    switch (event->event_type) {
    case IBV_EVENT_CQ_ERR:
        source = event->element.cq ? &event->element.cq->async : NULL;
        break;
    case IBV_EVENT_QP_FATAL:
        source = event->element.qp ? &event->element.qp->async : NULL;
        break;
    default:
        /* Native port and device records carry no MRC object reference. */
        return;
    }
    if (source) {
        struct mrc_abi_async_channel *channel = &source->context->async;
        pthread_mutex_lock(&channel->mutex);
        if (source->unacked && --source->unacked == 0)
            pthread_cond_broadcast(&channel->acked);
        pthread_mutex_unlock(&channel->mutex);
    }
}

MRC_PUBLIC int mrc_dpdk_get_async_fd(struct mrc_context *context, int *fd)
{
    if (!context || !fd)
        return EINVAL;
    *fd = async_wait_fd(&context->async);
    return 0;
}
