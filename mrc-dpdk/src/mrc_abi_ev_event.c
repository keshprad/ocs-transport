#include "mrc_abi.h"
#include "mrc_ev_event_io.h"

#include <errno.h>
#include <mrc_ctl.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct mrc_abi_ev_cq {
    pthread_mutex_t mutex;
    pthread_t worker;
    struct mrc_ev_event_listener listener;
    struct mrc_provider_cq *notification_cq;
    size_t capacity;
    size_t head;
    size_t count;
    int stop_fd;
    int failure;
    bool running;
    bool include_state;
    bool dropped;
    struct mrc_ev_event_record entries[];
};

struct legacy_ev_event {
    uint64_t profile_id;
    struct mrc_ctl_ev ev;
    uint32_t drop_count;
};

_Static_assert(sizeof(((struct mrc_ctl_ev *)0)->val) ==
               sizeof(((struct mrc_ev_event_record *)0)->value), "EV record widths must agree");
_Static_assert(offsetof(struct legacy_ev_event, drop_count) ==
               offsetof(struct mrc_ctl_ev_event, drop_count), "EV event prefixes must agree");
#if MRC_CTL_API_VER_USED >= MRC_CTL_API_VER(1, 0, 1)
_Static_assert(sizeof(struct legacy_ev_event) ==
               offsetof(struct mrc_ctl_ev_event, state), "Legacy EV event stride");
#endif

static void enqueue_event(struct mrc_abi_ev_cq *cq, const struct mrc_ev_event_record *record)
{
    pthread_mutex_lock(&cq->mutex);
    if (cq->count == cq->capacity) {
        cq->head = (cq->head + 1) % cq->capacity;
        cq->count--;
        cq->dropped = true;
    }
    size_t tail = (cq->head + cq->count) % cq->capacity;
    cq->entries[tail] = *record;
    cq->count++;
    mrc_provider_cq_signal(cq->notification_cq, true);
    pthread_mutex_unlock(&cq->mutex);
}

static int receive_events(struct mrc_abi_ev_cq *cq)
{
    /* Return to poll regularly so destruction can stop even a busy receiver. */
    for (unsigned int i = 0; i < 32; i++) {
        struct mrc_ev_event_record event;
        int result = mrc_ev_event_receive(cq->listener.fd, &event);
        if (!result)
            enqueue_event(cq, &event);
        else if (result == -EAGAIN)
            break;
        else if (result != -EBADMSG && result != -EINTR)
            return result;
    }
    return 0;
}

static void *event_worker(void *argument)
{
    struct mrc_abi_ev_cq *cq = argument;
    struct pollfd wait[] = {
        { .fd = cq->stop_fd, .events = POLLIN },
        { .fd = cq->listener.fd, .events = POLLIN },
    };
    int error;
    for (;;) {
        if (poll(wait, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            error = -errno;
            break;
        }
        if (wait[0].revents)
            return NULL;
        if (wait[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            error = -EIO;
            break;
        }
        if ((wait[1].revents & POLLIN) && (error = receive_events(cq)))
            break;
    }
    pthread_mutex_lock(&cq->mutex);
    cq->failure = error;
    pthread_mutex_unlock(&cq->mutex);
    return NULL;
}

void mrc_abi_ev_cq_stop(struct mrc_abi_ev_cq *cq)
{
    if (!cq || !cq->running)
        return;
    while (eventfd_write(cq->stop_fd, 1) < 0 && errno == EINTR) {}
    pthread_join(cq->worker, NULL);
    cq->running = false;
}

void mrc_abi_ev_cq_free(struct mrc_abi_ev_cq *cq)
{
    if (!cq)
        return;
    mrc_abi_ev_cq_stop(cq);
    if (cq->stop_fd >= 0)
        close(cq->stop_fd);
    mrc_ev_event_close(&cq->listener);
    pthread_mutex_destroy(&cq->mutex);
    free(cq);
}

MRC_PUBLIC struct mrc_cq *mrc_ctl_create_ev_event_cq(struct mrc_context *context, int cqe,
                                                     void *cq_context, struct mrc_comp_channel *channel,
                                                     int comp_vector)
{
    if (cqe <= 0) {
        errno = EINVAL;
        return NULL;
    }
    /* Use the ordinary provider CQ only for completion notifications. */
    struct mrc_cq *cq = mrc_create_cq(context, 1, cq_context, channel, comp_vector);
    if (!cq)
        return NULL;
    int error = EPERM;
    if (!mrc_abi_controller_capable())
        goto destroy_cq;
    struct mrc_ctl_store store = mrc_abi_profile_store(context);
    error = ENODEV;
    if (store.fd < 0)
        goto destroy_cq;
    error = EOVERFLOW;
    if ((size_t)cqe > (SIZE_MAX - sizeof(struct mrc_abi_ev_cq)) / sizeof(struct mrc_ev_event_record))
        goto destroy_cq;

    struct mrc_abi_ev_cq *events = calloc(1, sizeof(*events) + (size_t)cqe * sizeof(events->entries[0]));
    error = ENOMEM;
    if (!events)
        goto destroy_cq;
    error = pthread_mutex_init(&events->mutex, NULL);
    if (error) {
        free(events);
        goto destroy_cq;
    }
    events->listener = (struct mrc_ev_event_listener)MRC_EV_EVENT_LISTENER_INITIALIZER;
    events->stop_fd = -1;
    events->notification_cq = cq->provider;
    events->capacity = (size_t)cqe;
    events->include_state = mrc_abi_ev_event_state_supported(context);
    error = -mrc_ev_event_listen(store.fd, &events->listener);
    if (error)
        goto free_events;
    events->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (events->stop_fd < 0) {
        error = errno;
        goto free_events;
    }
    error = pthread_create(&events->worker, NULL, event_worker, events);
    if (error)
        goto free_events;
    events->running = true;
    cq->ev_events = events;
    return cq;

free_events:
    mrc_abi_ev_cq_free(events);
destroy_cq:
    mrc_destroy_cq(cq);
    errno = error;
    return NULL;
}

MRC_PUBLIC int mrc_ctl_poll_ev_event(struct mrc_cq *ev_cq, int num_entries,
                                     struct mrc_ctl_ev_event *ev_event)
{
    if (!ev_cq || !ev_cq->ev_events || num_entries < 0 || (num_entries && !ev_event))
        return -EINVAL;
    if (!num_entries)
        return 0;

    struct mrc_abi_ev_cq *cq = ev_cq->ev_events;
    uint8_t *output = (uint8_t *)ev_event;
    size_t stride = cq->include_state ? sizeof(*ev_event) : sizeof(struct legacy_ev_event);
    pthread_mutex_lock(&cq->mutex);
    size_t count = (size_t)num_entries < cq->count ? (size_t)num_entries : cq->count;
    for (size_t i = 0; i < count; i++) {
        const struct mrc_ev_event_record *record = &cq->entries[(cq->head + i) % cq->capacity];
        struct mrc_ctl_ev_event event;
        memset(&event, 0, sizeof(event));
        event.profile_id = record->profile_id;
        memcpy(event.ev.val, record->value, sizeof(event.ev.val));
        event.ev.port = record->port;
        event.drop_count = cq->dropped || record->dropped;
#if MRC_CTL_API_VER_USED >= MRC_CTL_API_VER(1, 0, 1)
        if (cq->include_state)
            event.state = record->state == MRC_EV_EVENT_GOOD ?
                          MRC_CTL_EV_GOOD : MRC_CTL_EV_ASSUMED_BAD;
#endif
        memcpy(output + i * stride, &event, stride);
        cq->dropped = false;
    }
    cq->head = (cq->head + count) % cq->capacity;
    cq->count -= count;
    int result = count ? (int)count : cq->failure;
    pthread_mutex_unlock(&cq->mutex);
    return result;
}
