#include "mrc_abi.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

struct mrc_abi_async_notice {
    struct mrc_abi_async_notice *next;
    struct mrc_abi_async_source *source;
    struct mrc_async_event event;
};

int mrc_abi_async_open(struct mrc_abi_async_channel *channel)
{
    int error = pthread_mutex_init(&channel->mutex, NULL);
    if (error)
        return error;
    error = pthread_cond_init(&channel->acked, NULL);
    if (error)
        goto destroy_mutex;
    channel->fd = eventfd(0, EFD_CLOEXEC);
    if (channel->fd >= 0)
        return 0;
    error = errno;
    pthread_cond_destroy(&channel->acked);

destroy_mutex:
    pthread_mutex_destroy(&channel->mutex);
    return error;
}

void mrc_abi_async_close(struct mrc_abi_async_channel *channel)
{
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

static int wait_notice(struct mrc_abi_async_channel *channel, struct mrc_async_event *event)
{
    for (;;) {
        int error = take_notice(channel, event);
        if (error != EAGAIN)
            return error;

        int flags = fcntl(channel->fd, F_GETFL);
        if (flags < 0)
            return errno;
        if (flags & O_NONBLOCK)
            return EAGAIN;

        struct pollfd wait = { .fd = channel->fd, .events = POLLIN };
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
    *fd = context->async.fd;
    return 0;
}
