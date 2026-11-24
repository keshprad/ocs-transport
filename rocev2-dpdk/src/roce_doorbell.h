#ifndef ROCE_DOORBELL_H
#define ROCE_DOORBELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

enum { ROCE_DOORBELL_SLOTS = 64 };

struct roce_doorbell {
    _Alignas(64) _Atomic uint32_t producer_hint;
    _Atomic uint64_t notifications;
    _Alignas(64) _Atomic uint64_t slots[ROCE_DOORBELL_SLOTS];
};

/* Notification state uses padding in the original shared layout. */
_Static_assert(offsetof(struct roce_doorbell, notifications) == 8, "unexpected doorbell notification offset");
_Static_assert(offsetof(struct roce_doorbell, slots) == 64, "doorbell slots moved");
_Static_assert(sizeof(struct roce_doorbell) == 64 + 8 * ROCE_DOORBELL_SLOTS, "doorbell size changed");

static inline bool roce_doorbell_init(struct roce_doorbell *queue)
{
    atomic_init(&queue->producer_hint, 0);
    atomic_init(&queue->notifications, 0);
    for (uint32_t i = 0; i < ROCE_DOORBELL_SLOTS; ++i)
        atomic_init(&queue->slots[i], 0);
    return atomic_is_lock_free(&queue->producer_hint) && atomic_is_lock_free(&queue->notifications) &&
           atomic_is_lock_free(&queue->slots[0]);
}

/* An item becomes visible in one write, so a producer exit leaves no reserved slot. */
static inline bool roce_doorbell_publish(struct roce_doorbell *queue, uint64_t item)
{
    if (!item)
        return false;
    uint32_t start = atomic_fetch_add_explicit(&queue->producer_hint, 1, memory_order_relaxed);
    for (uint32_t i = 0; i < ROCE_DOORBELL_SLOTS; ++i) {
        uint32_t index = (start + i) % ROCE_DOORBELL_SLOTS;
        uint64_t empty = 0;
        if (atomic_compare_exchange_strong_explicit(&queue->slots[index], &empty, item,
                                                    memory_order_release, memory_order_relaxed))
            return true;
    }
    return false;
}

/* A single consumer owns the cursor. Work ordering remains per QP. */
static inline uint64_t roce_doorbell_take(struct roce_doorbell *queue, uint32_t *cursor)
{
    for (uint32_t i = 0; i < ROCE_DOORBELL_SLOTS; ++i) {
        uint32_t index = (*cursor + i) % ROCE_DOORBELL_SLOTS;
        if (!atomic_load_explicit(&queue->slots[index], memory_order_relaxed))
            continue;
        uint64_t item = atomic_exchange_explicit(&queue->slots[index], 0, memory_order_acquire);
        *cursor = (index + 1) % ROCE_DOORBELL_SLOTS;
        return item;
    }
    return 0;
}

#endif
