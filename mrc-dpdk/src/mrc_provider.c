#include "mrc_provider.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

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

struct mrc_provider_cq {
    pthread_mutex_t mutex;
    size_t capacity;
    size_t references;
    size_t head;
    size_t count;
    size_t ready;
    size_t unacked;
    bool armed;
    bool solicited_only;
    bool overrun;
    bool overflow_event;
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

int mrc_provider_mr_register(struct mrc_provider_pd *pd, void *base, size_t length,
                             uint64_t iova, unsigned int flags,
                             struct mrc_provider_mr_keys *keys)
{
    unsigned int supported = MRC_PROVIDER_MR_LOCAL_WRITE | MRC_PROVIDER_MR_REMOTE_WRITE;
    bool remote_write = (flags & MRC_PROVIDER_MR_REMOTE_WRITE) != 0;
    if (!pd || !base || !length || !keys || (flags & ~supported) ||
        (remote_write && !(flags & MRC_PROVIDER_MR_LOCAL_WRITE)))
        return -EINVAL;
    if (length > UINTPTR_MAX - (uintptr_t)base || length > UINT64_MAX - iova)
        return -EOVERFLOW;

    pthread_mutex_lock(&pd->mutex);
    int result = -ENOSPC;
    for (size_t i = 0; i < pd->capacity; i++) {
        struct region *slot = &pd->regions[i];
        if (slot->keys.lkey)
            continue;
        struct mrc_provider_mr_keys generated;
        result = reserve_keys(&generated, remote_write);
        if (!result) {
            *slot = (struct region){
                .base = base, .length = length, .iova = iova, .flags = flags, .keys = generated,
            };
            *keys = generated;
        }
        break;
    }
    pthread_mutex_unlock(&pd->mutex);
    return result;
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
        .address = address, .length = access->length, .owner = pd, .lkey = slot->keys.lkey,
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

int mrc_provider_cq_create(size_t capacity, struct mrc_provider_cq **cq)
{
    if (!cq || !capacity)
        return -EINVAL;
    if (capacity > (SIZE_MAX - sizeof(struct mrc_provider_cq)) / sizeof(struct mrc_provider_wc))
        return -EOVERFLOW;
    struct mrc_provider_cq *object =
        calloc(1, sizeof(*object) + capacity * sizeof(struct mrc_provider_wc));
    if (!object)
        return -ENOMEM;
    int error = pthread_mutex_init(&object->mutex, NULL);
    if (error) {
        free(object);
        return -error;
    }
    object->capacity = capacity;
    *cq = object;
    return 0;
}

int mrc_provider_cq_retain(struct mrc_provider_cq *cq)
{
    return cq ? change_reference(&cq->mutex, &cq->references, true) : -EINVAL;
}

int mrc_provider_cq_release(struct mrc_provider_cq *cq)
{
    return cq ? change_reference(&cq->mutex, &cq->references, false) : -EINVAL;
}

int mrc_provider_cq_destroy(struct mrc_provider_cq *cq)
{
    if (!cq)
        return -EINVAL;
    pthread_mutex_lock(&cq->mutex);
    bool busy = cq->references || cq->count || cq->ready || cq->unacked || cq->overflow_event;
    pthread_mutex_unlock(&cq->mutex);
    if (busy)
        return -EBUSY;
    pthread_mutex_destroy(&cq->mutex);
    free(cq);
    return 0;
}

static int cq_overrun(struct mrc_provider_cq *cq)
{
    cq->overrun = true;
    cq->overflow_event = true;
    cq->count = 0;
    cq->armed = false;
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
    if (cq->overrun) {
        result = -EOVERFLOW;
    } else if (cq->count == cq->capacity || (notify && cq->ready == SIZE_MAX)) {
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
    if (cq->overrun) {
        pthread_mutex_unlock(&cq->mutex);
        return -EOVERFLOW;
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
    if (cq->overrun) {
        result = -EOVERFLOW;
    } else {
        cq->armed = true;
        cq->solicited_only = solicited_only;
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
    if (cq->unacked == SIZE_MAX) {
        result = -EOVERFLOW;
    } else if (cq->ready) {
        cq->ready--;
        *event = MRC_PROVIDER_CQ_COMPLETION;
    } else if (cq->overflow_event) {
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
    else
        cq->unacked -= count;
    pthread_mutex_unlock(&cq->mutex);
    return result;
}
