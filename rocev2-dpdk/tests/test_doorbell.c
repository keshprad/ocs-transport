#define _GNU_SOURCE
#include "roce_doorbell.h"

#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { PRODUCERS = 6, ITEMS = 10000, TIMEOUT = 15 };

static time_t monotonic_seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec;
}

static void wait_success(pid_t child)
{
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    struct roce_doorbell *queue = mmap(NULL, sizeof(*queue), PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(queue != MAP_FAILED && roce_doorbell_init(queue));

    for (uint32_t i = 1; i <= ROCE_DOORBELL_SLOTS; ++i)
        assert(roce_doorbell_publish(queue, i));
    assert(!roce_doorbell_publish(queue, ROCE_DOORBELL_SLOTS + 1));
    uint32_t cursor = 0;
    for (uint32_t i = 1; i <= ROCE_DOORBELL_SLOTS; ++i)
        assert(roce_doorbell_take(queue, &cursor) == i);
    assert(roce_doorbell_take(queue, &cursor) == 0);

    pid_t interrupted = fork();
    assert(interrupted >= 0);
    if (!interrupted) {
        /* Exit after taking a starting hint, before publishing any item. */
        atomic_fetch_add_explicit(&queue->producer_hint, 1, memory_order_relaxed);
        _exit(0);
    }
    wait_success(interrupted);

    pid_t children[PRODUCERS];
    time_t deadline = monotonic_seconds() + TIMEOUT;
    for (uint32_t i = 0; i < PRODUCERS; ++i) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (children[i])
            continue;
        for (uint32_t j = 0; j < ITEMS; ++j) {
            uint64_t item = (uint64_t)i * ITEMS + j + 1;
            while (!roce_doorbell_publish(queue, item)) {
                if (monotonic_seconds() >= deadline)
                    _exit(1);
                sched_yield();
            }
        }
        _exit(0);
    }

    bool *seen = calloc(PRODUCERS * ITEMS, sizeof(*seen));
    assert(seen);
    for (uint32_t i = 0; i < PRODUCERS * ITEMS; ++i) {
        uint64_t item;
        while (!(item = roce_doorbell_take(queue, &cursor))) {
            assert(monotonic_seconds() < deadline);
            sched_yield();
        }
        assert(item <= PRODUCERS * ITEMS && !seen[item - 1]);
        seen[item - 1] = true;
    }
    for (uint32_t i = 0; i < PRODUCERS; ++i)
        wait_success(children[i]);
    assert(roce_doorbell_take(queue, &cursor) == 0);
    free(seen);
    assert(munmap(queue, sizeof(*queue)) == 0);
    puts("process-shared MPSC doorbell verified 60000 unique notifications");
    return 0;
}
