#include "mrc_provider.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

_Static_assert(SIZE_MAX <= UINT64_MAX, "MR lengths must fit in a 64-bit IOVA");

struct region {
    unsigned char *base;
    size_t length;
    uint64_t iova;
    unsigned int flags;
    struct mrc_provider_mr_keys keys;
    size_t leases;
};

struct mrc_provider_pd {
    pthread_mutex_t mutex;
    size_t capacity;
    size_t references;
    struct region regions[];
};

struct completion_notification {
    struct mrc_provider_cq *cq;
    struct completion_notification *next;
};

struct mrc_provider_comp_channel {
    pthread_mutex_t mutex;
    size_t references;
    int fd;
    struct completion_notification *head;
    struct completion_notification *tail;
};

enum cq_health {
    CQ_READY,
    CQ_PUBLISHING_ERROR,
    CQ_OVERRUN,
};

struct mrc_provider_cq {
    pthread_mutex_t mutex;
    pthread_cond_t events_acked;
    struct mrc_provider_comp_channel *channel;
    struct completion_notification *notification;
    void *context;
    void (*on_overrun)(void *context);
    void *overrun_context;
    size_t capacity;
    size_t references;
    size_t head;
    size_t count;
    size_t ready;
    size_t unacked;
    bool armed;
    bool solicited_only;
    atomic_int health;
    bool overflow_event;
    bool closing;
    struct mrc_provider_wc entries[];
};

static atomic_uint_fast64_t next_key = ATOMIC_VAR_INIT(1);

static int reserve_keys(struct mrc_provider_mr_keys *keys, bool remote_write)
{
    uint_fast64_t first = atomic_load_explicit(&next_key, memory_order_relaxed);
    uint_fast64_t last;
    do {
        last = first + (remote_write ? 1u : 0u);
        if (last > UINT32_MAX)
            return -ENOSPC;
    } while (!atomic_compare_exchange_weak_explicit(&next_key, &first, last + 1,
                                                    memory_order_relaxed,
                                                    memory_order_relaxed));
    keys->lkey = (uint32_t)first;
    keys->rkey = remote_write ? (uint32_t)last : 0;
    return 0;
}

static int change_reference(pthread_mutex_t *mutex, size_t *references, bool retain)
{
    pthread_mutex_lock(mutex);
    int result = 0;
    if (retain) {
        if (*references == SIZE_MAX)
            result = -EOVERFLOW;
        else
            ++*references;
    } else if (*references == 0) {
        result = -EINVAL;
    } else {
        --*references;
    }
    pthread_mutex_unlock(mutex);
    return result;
}

int mrc_provider_pd_create(size_t region_capacity, struct mrc_provider_pd **pd)
{
    if (!pd || !region_capacity)
        return -EINVAL;
    if (region_capacity > (SIZE_MAX - sizeof(struct mrc_provider_pd)) / sizeof(struct region))
        return -EOVERFLOW;
    struct mrc_provider_pd *object =
        calloc(1, sizeof(*object) + region_capacity * sizeof(struct region));
    if (!object)
        return -ENOMEM;
    int error = pthread_mutex_init(&object->mutex, NULL);
    if (error) {
        free(object);
        return -error;
    }
    object->capacity = region_capacity;
    *pd = object;
    return 0;
}

int mrc_provider_pd_retain(struct mrc_provider_pd *pd)
{
    return pd ? change_reference(&pd->mutex, &pd->references, true) : -EINVAL;
}

int mrc_provider_pd_release(struct mrc_provider_pd *pd)
{
    return pd ? change_reference(&pd->mutex, &pd->references, false) : -EINVAL;
}

int mrc_provider_pd_destroy(struct mrc_provider_pd *pd)
{
    if (!pd)
        return -EINVAL;
    pthread_mutex_lock(&pd->mutex);
    bool busy = pd->references != 0;
    for (size_t i = 0; i < pd->capacity && !busy; i++)
        busy = pd->regions[i].keys.lkey != 0;
    pthread_mutex_unlock(&pd->mutex);
    if (busy)
        return -EBUSY;
    pthread_mutex_destroy(&pd->mutex);
    free(pd);
    return 0;
}

static struct region *find_region(struct mrc_provider_pd *pd, uint32_t key, bool remote)
{
    for (size_t i = 0; i < pd->capacity; i++) {
        struct region *slot = &pd->regions[i];
        if (key == (remote ? slot->keys.rkey : slot->keys.lkey))
            return slot;
    }
    return NULL;
}

static int validate_registration(struct mrc_provider_pd *pd, void *base, size_t length,
                                 uint64_t iova, unsigned int flags)
{
    unsigned int supported = MRC_PROVIDER_MR_LOCAL_WRITE | MRC_PROVIDER_MR_REMOTE_WRITE;
    if (!pd || !base || !length || (flags & ~supported) ||
        ((flags & MRC_PROVIDER_MR_REMOTE_WRITE) && !(flags & MRC_PROVIDER_MR_LOCAL_WRITE)))
        return -EINVAL;
    return length > UINTPTR_MAX - (uintptr_t)base || length > UINT64_MAX - iova
           ? -EOVERFLOW : 0;
}

static bool keys_in_use(struct mrc_provider_pd *pd, const struct mrc_provider_mr_keys *keys)
{
    return find_region(pd, keys->lkey, false) ||
           (keys->rkey && find_region(pd, keys->rkey, true));
}

static int insert_registration(struct mrc_provider_pd *pd, struct region *region,
                               const struct mrc_provider_mr_keys *external)
{
    pthread_mutex_lock(&pd->mutex);
    int result = -ENOSPC;
    if (external && keys_in_use(pd, external)) {
        result = -EEXIST;
    } else {
        struct region *slot = find_region(pd, 0, false);
        if (slot) {
            if (external) {
                region->keys = *external;
                result = 0;
            } else {
                bool remote_write = (region->flags & MRC_PROVIDER_MR_REMOTE_WRITE) != 0;
                do {
                    result = reserve_keys(&region->keys, remote_write);
                } while (!result && keys_in_use(pd, &region->keys));
            }
            if (!result)
                *slot = *region;
        }
    }
    pthread_mutex_unlock(&pd->mutex);
    return result;
}

int mrc_provider_mr_register(struct mrc_provider_pd *pd, void *base, size_t length,
                             uint64_t iova, unsigned int flags,
                             struct mrc_provider_mr_keys *keys)
{
    if (!keys)
        return -EINVAL;
    int result = validate_registration(pd, base, length, iova, flags);
    if (result)
        return result;
    struct region region = { .base = base, .length = length, .iova = iova, .flags = flags };
    result = insert_registration(pd, &region, NULL);
    if (!result)
        *keys = region.keys;
    return result;
}

int mrc_provider_mr_bind(struct mrc_provider_pd *pd, void *base, size_t length,
                         uint64_t iova, unsigned int flags,
                         const struct mrc_provider_mr_keys *keys)
{
    if (!keys || !keys->lkey || (!!keys->rkey != !!(flags & MRC_PROVIDER_MR_REMOTE_WRITE)))
        return -EINVAL;
    int result = validate_registration(pd, base, length, iova, flags);
    if (result)
        return result;
    struct region region = { .base = base, .length = length, .iova = iova, .flags = flags };
    return insert_registration(pd, &region, keys);
}

int mrc_provider_mr_deregister(struct mrc_provider_pd *pd, uint32_t lkey)
{
    if (!pd || !lkey)
        return -EINVAL;

    pthread_mutex_lock(&pd->mutex);
    struct region *slot = find_region(pd, lkey, false);
    int result;
    if (!slot) {
        result = -ENOENT;
    } else if (slot->leases) {
        result = -EBUSY;
    } else {
        *slot = (struct region){0};
        result = 0;
    }
    pthread_mutex_unlock(&pd->mutex);
    return result;
}

static int acquire_region(struct mrc_provider_pd *pd, struct region *slot,
                           const struct mrc_provider_mr_access *access,
                           struct mrc_provider_mr_lease *lease)
{
    if (!slot || (access->operation == MRC_PROVIDER_MR_WRITE_LOCAL &&
                  !(slot->flags & MRC_PROVIDER_MR_LOCAL_WRITE)))
        return -EACCES;
    if (access->iova < slot->iova)
        return -EACCES;
    uint64_t offset = access->iova - slot->iova;
    if (offset > slot->length || access->length > slot->length - (size_t)offset)
        return -EACCES;

    unsigned char *address = slot->base + (size_t)offset;
    if (access->alignment) {
        size_t mask = access->alignment - 1;
        if ((access->iova & mask) || ((uintptr_t)address & mask))
            return -EACCES;
    }
    if (slot->leases == SIZE_MAX)
        return -EOVERFLOW;

    slot->leases++;
    *lease = (struct mrc_provider_mr_lease){
        .address = address, .length = access->length,
        .region_iova = slot->iova, .region_length = slot->length,
        .owner = pd, .lkey = slot->keys.lkey,
    };
    return 0;
}

int mrc_provider_mr_acquire(struct mrc_provider_pd *pd,
                            const struct mrc_provider_mr_access *access,
                            struct mrc_provider_mr_lease *lease)
{
    if (!pd || !access || !lease || !access->key ||
        (access->operation != MRC_PROVIDER_MR_READ_LOCAL &&
         access->operation != MRC_PROVIDER_MR_WRITE_LOCAL &&
         access->operation != MRC_PROVIDER_MR_WRITE_REMOTE) ||
        (access->alignment && (access->alignment & (access->alignment - 1))))
        return -EINVAL;
    if (lease->owner)
        return -EBUSY;

    bool remote = access->operation == MRC_PROVIDER_MR_WRITE_REMOTE;
    pthread_mutex_lock(&pd->mutex);
    int result = acquire_region(pd, find_region(pd, access->key, remote), access, lease);
    pthread_mutex_unlock(&pd->mutex);
    return result;
}

int mrc_provider_mr_release(struct mrc_provider_mr_lease *lease)
{
    if (!lease || !lease->owner || !lease->lkey)
        return -EINVAL;
    struct mrc_provider_pd *pd = lease->owner;
    pthread_mutex_lock(&pd->mutex);
    struct region *slot = find_region(pd, lease->lkey, false);
    if (!slot || !slot->leases) {
        pthread_mutex_unlock(&pd->mutex);
        return -EINVAL;
    }
    slot->leases--;
    *lease = (struct mrc_provider_mr_lease){0};
    pthread_mutex_unlock(&pd->mutex);
    return 0;
}

int mrc_provider_comp_channel_create(struct mrc_provider_comp_channel **channel)
{
    if (!channel)
        return -EINVAL;
    struct mrc_provider_comp_channel *object = calloc(1, sizeof(*object));
    if (!object)
        return -ENOMEM;
    int error = pthread_mutex_init(&object->mutex, NULL);
    if (error) {
        free(object);
        return -error;
    }
    object->fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE | EFD_NONBLOCK);
    if (object->fd < 0) {
        error = errno;
        pthread_mutex_destroy(&object->mutex);
        free(object);
        return -error;
    }
    *channel = object;
    return 0;
}

int mrc_provider_comp_channel_fd(const struct mrc_provider_comp_channel *channel)
{
    return channel ? channel->fd : -EINVAL;
}

int mrc_provider_comp_channel_destroy(struct mrc_provider_comp_channel *channel)
{
    if (!channel)
        return -EINVAL;
    pthread_mutex_lock(&channel->mutex);
    bool busy = channel->references || channel->head;
    pthread_mutex_unlock(&channel->mutex);
    if (busy)
        return -EBUSY;
    close(channel->fd);
    pthread_mutex_destroy(&channel->mutex);
    free(channel);
    return 0;
}

static int change_notification_count(int fd, bool add)
{
    eventfd_t value;
    int result;
    do {
        result = add ? eventfd_write(fd, 1) : eventfd_read(fd, &value);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : 0;
}

int mrc_provider_comp_channel_next(struct mrc_provider_comp_channel *channel,
                                   struct mrc_provider_cq **cq, void **context)
{
    if (!channel || !cq || !context)
        return -EINVAL;
    pthread_mutex_lock(&channel->mutex);
    struct completion_notification *notification = channel->head;
    int result = notification ? change_notification_count(channel->fd, false) : -EAGAIN;
    if (!result) {
        channel->head = notification->next;
        if (!channel->head)
            channel->tail = NULL;
    }
    pthread_mutex_unlock(&channel->mutex);
    if (result)
        return result;

    struct mrc_provider_cq *object = notification->cq;
    free(notification);
    /* ready pins the CQ until ownership of the event passes to the caller. */
    pthread_mutex_lock(&object->mutex);
    object->ready--;
    object->unacked++;
    *cq = object;
    *context = object->context;
    pthread_mutex_unlock(&object->mutex);
    return 0;
}

int mrc_provider_cq_create_on_channel(size_t capacity, struct mrc_provider_comp_channel *channel,
                                      void *context, struct mrc_provider_cq **cq)
{
    if (!cq || !capacity)
        return -EINVAL;
    if (capacity > (SIZE_MAX - sizeof(struct mrc_provider_cq)) / sizeof(struct mrc_provider_wc))
        return -EOVERFLOW;
    struct mrc_provider_cq *object =
        calloc(1, sizeof(*object) + capacity * sizeof(struct mrc_provider_wc));
    if (!object)
        return -ENOMEM;
    int result = -pthread_mutex_init(&object->mutex, NULL);
    if (result)
        goto free_object;
    result = -pthread_cond_init(&object->events_acked, NULL);
    if (result)
        goto destroy_mutex;
    if (channel && (result = change_reference(&channel->mutex, &channel->references, true)))
        goto destroy_condition;

    object->capacity = capacity;
    atomic_init(&object->health, CQ_READY);
    object->channel = channel;
    object->context = context;
    *cq = object;
    return 0;

destroy_condition:
    pthread_cond_destroy(&object->events_acked);
destroy_mutex:
    pthread_mutex_destroy(&object->mutex);
free_object:
    free(object);
    return result;
}

int mrc_provider_cq_create(size_t capacity, struct mrc_provider_cq **cq)
{
    return mrc_provider_cq_create_on_channel(capacity, NULL, NULL, cq);
}

void mrc_provider_cq_on_overrun(struct mrc_provider_cq *cq, void (*notify)(void *), void *context)
{
    pthread_mutex_lock(&cq->mutex);
    cq->on_overrun = notify;
    cq->overrun_context = context;
    pthread_mutex_unlock(&cq->mutex);
}

bool mrc_provider_cq_failed(struct mrc_provider_cq *cq)
{
    if (!cq)
        return false;
    int health = atomic_load_explicit(&cq->health, memory_order_acquire);
    if (health == CQ_PUBLISHING_ERROR) {
        /* Queue the CQ error before a dependent QP reports its failure. */
        pthread_mutex_lock(&cq->mutex);
        pthread_mutex_unlock(&cq->mutex);
    }
    return health != CQ_READY;
}

static bool cq_failed_locked(const struct mrc_provider_cq *cq)
{
    return atomic_load_explicit(&cq->health, memory_order_relaxed) != CQ_READY;
}

int mrc_provider_cq_retain(struct mrc_provider_cq *cq)
{
    if (!cq)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    int result = 0;
    if (cq->closing)
        result = -EBUSY;
    else if (cq_failed_locked(cq))
        result = -EOVERFLOW;
    else if (cq->references == SIZE_MAX)
        result = -EOVERFLOW;
    else
        cq->references++;
    pthread_mutex_unlock(&cq->mutex);
    return result;
}

int mrc_provider_cq_release(struct mrc_provider_cq *cq)
{
    return cq ? change_reference(&cq->mutex, &cq->references, false) : -EINVAL;
}

static void free_cq(struct mrc_provider_cq *cq)
{
    if (cq->channel)
        change_reference(&cq->channel->mutex, &cq->channel->references, false);
    free(cq->notification);
    pthread_cond_destroy(&cq->events_acked);
    pthread_mutex_destroy(&cq->mutex);
    free(cq);
}

int mrc_provider_cq_destroy(struct mrc_provider_cq *cq)
{
    if (!cq)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    bool busy = cq->closing || cq->references || cq->count ||
                cq->ready || cq->unacked || cq->overflow_event;
    pthread_mutex_unlock(&cq->mutex);
    if (busy)
        return -EBUSY;
    free_cq(cq);
    return 0;
}

static int discard_notifications(struct mrc_provider_cq *cq)
{
    struct mrc_provider_comp_channel *channel = cq->channel;
    if (!channel) {
        cq->ready = 0;
        return 0;
    }

    pthread_mutex_lock(&channel->mutex);
    struct completion_notification **link = &channel->head;
    struct completion_notification *previous = NULL;
    int result = 0;
    while (*link) {
        struct completion_notification *notification = *link;
        if (notification->cq != cq) {
            previous = notification;
            link = &notification->next;
            continue;
        }
        result = change_notification_count(channel->fd, false);
        if (result)
            break;
        *link = notification->next;
        if (channel->tail == notification)
            channel->tail = previous;
        cq->ready--;
        free(notification);
    }
    pthread_mutex_unlock(&channel->mutex);
    return result;
}

int mrc_provider_cq_destroy_wait(struct mrc_provider_cq *cq)
{
    if (!cq)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    if (cq->closing || cq->references) {
        pthread_mutex_unlock(&cq->mutex);
        return -EBUSY;
    }
    cq->closing = true;
    int result = discard_notifications(cq);
    if (result) {
        cq->closing = false;
    } else {
        while (cq->unacked || cq->ready)
            pthread_cond_wait(&cq->events_acked, &cq->mutex);
    }
    pthread_mutex_unlock(&cq->mutex);
    if (!result)
        free_cq(cq);
    return result;
}

static int signal_notification(struct mrc_provider_cq *cq)
{
    struct mrc_provider_comp_channel *channel = cq->channel;
    if (!channel)
        return 0;
    pthread_mutex_lock(&channel->mutex);
    int result = change_notification_count(channel->fd, true);
    if (!result) {
        if (channel->tail)
            channel->tail->next = cq->notification;
        else
            channel->head = cq->notification;
        channel->tail = cq->notification;
        cq->notification = NULL;
    }
    pthread_mutex_unlock(&channel->mutex);
    return result;
}

static int cq_overrun(struct mrc_provider_cq *cq)
{
    cq->overflow_event = true;
    cq->count = 0;
    cq->armed = false;
    free(cq->notification);
    cq->notification = NULL;
    if (cq->on_overrun) {
        atomic_store_explicit(&cq->health, CQ_PUBLISHING_ERROR, memory_order_release);
        cq->on_overrun(cq->overrun_context);
    }
    atomic_store_explicit(&cq->health, CQ_OVERRUN, memory_order_release);
    return -EOVERFLOW;
}

int mrc_provider_cq_push(struct mrc_provider_cq *cq, const struct mrc_provider_wc *wc,
                         bool solicited)
{
    if (!cq || !wc)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);

    int result = 0;
    bool notify = cq->armed && (!cq->solicited_only || solicited || wc->status != MRC_PROVIDER_WC_SUCCESS);
    if (cq->closing) {
        result = -EBUSY;
    } else if (cq_failed_locked(cq)) {
        result = -EOVERFLOW;
    } else if (cq->count == cq->capacity ||
               (notify && (cq->ready == SIZE_MAX - cq->unacked || signal_notification(cq)))) {
        result = cq_overrun(cq);
    } else {
        size_t tail = (cq->head + cq->count) % cq->capacity;
        cq->entries[tail] = *wc;
        cq->count++;
        if (notify) {
            cq->ready++;
            cq->armed = false;
        }
    }
    pthread_mutex_unlock(&cq->mutex);
    return result;
}

int mrc_provider_cq_poll(struct mrc_provider_cq *cq, struct mrc_provider_wc *entries,
                         size_t count)
{
    if (!cq || (count && !entries) || count > INT_MAX)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    if (cq->closing || cq_failed_locked(cq)) {
        int result = cq->closing ? -EBUSY : -EOVERFLOW;
        pthread_mutex_unlock(&cq->mutex);
        return result;
    }

    size_t copied = count < cq->count ? count : cq->count;
    for (size_t i = 0; i < copied; i++)
        entries[i] = cq->entries[(cq->head + i) % cq->capacity];
    cq->head = (cq->head + copied) % cq->capacity;
    cq->count -= copied;
    pthread_mutex_unlock(&cq->mutex);
    return (int)copied;
}

int mrc_provider_cq_arm(struct mrc_provider_cq *cq, bool solicited_only)
{
    if (!cq)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    int result = 0;
    if (cq->closing) {
        result = -EBUSY;
    } else if (cq_failed_locked(cq)) {
        result = -EOVERFLOW;
    } else {
        if (cq->channel && !cq->armed) {
            cq->notification = calloc(1, sizeof(*cq->notification));
            if (!cq->notification)
                result = -ENOMEM;
            else
                cq->notification->cq = cq;
        }
        if (!result) {
            cq->armed = true;
            cq->solicited_only = solicited_only;
        }
    }
    pthread_mutex_unlock(&cq->mutex);
    return result;
}

int mrc_provider_cq_next_event(struct mrc_provider_cq *cq, enum mrc_provider_cq_event *event)
{
    if (!cq || !event)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    int result = 0;
    if (cq->closing) {
        result = -EBUSY;
    } else if (cq->unacked == SIZE_MAX) {
        result = -EOVERFLOW;
    } else if (!cq->channel && cq->ready) {
        cq->ready--;
        *event = MRC_PROVIDER_CQ_COMPLETION;
    } else if (cq->overflow_event) {
        if (cq->ready == SIZE_MAX - cq->unacked) {
            pthread_mutex_unlock(&cq->mutex);
            return -EOVERFLOW;
        }
        cq->overflow_event = false;
        *event = MRC_PROVIDER_CQ_OVERFLOW;
    } else {
        result = -EAGAIN;
    }
    if (!result)
        cq->unacked++;
    pthread_mutex_unlock(&cq->mutex);
    return result;
}

int mrc_provider_cq_ack_events(struct mrc_provider_cq *cq, size_t count)
{
    if (!cq)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    int result = 0;
    if (count > cq->unacked)
        result = -EINVAL;
    else {
        cq->unacked -= count;
        if (cq->closing && !cq->unacked && !cq->ready)
            pthread_cond_signal(&cq->events_acked);
    }
    pthread_mutex_unlock(&cq->mutex);
    return result;
}
