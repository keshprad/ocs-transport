#include "mrc_ev.h"

#include <string.h>

bool mrc_ev_init(struct mrc_ev_profile *profile, unsigned int count, unsigned int min_active,
                 uint64_t skip_interval, uint64_t probe_interval)
{
    if (!count || count > MRC_EV_MAX_PATHS || min_active > count || !skip_interval || !probe_interval)
        return false;
    memset(profile, 0, sizeof(*profile));
    profile->count = count;
    profile->min_active = min_active;
    profile->skip_interval = skip_interval;
    profile->probe_interval = probe_interval;
    return true;
}

static void transition(struct mrc_ev_path *ev, enum mrc_ev_state state,
                       uint64_t now, uint64_t next)
{
    ev->state = state;
    ev->changed_at = now;
    ev->next_action = next;
}

void mrc_ev_deny(struct mrc_ev_profile *profile, unsigned int path, bool denied,
                 uint64_t now)
{
    struct mrc_ev_path *ev = &profile->paths[path];
    if (denied || ev->state == MRC_EV_DENIED)
        transition(ev, denied ? MRC_EV_DENIED : MRC_EV_GOOD, now, 0);
}

void mrc_ev_timeout(struct mrc_ev_profile *profile, unsigned int path, uint64_t now)
{
    struct mrc_ev_path *ev = &profile->paths[path];
    if (ev->state == MRC_EV_DENIED || ev->state == MRC_EV_ASSUMED_BAD)
        return;
    if (profile->min_active) {
        uint32_t others = mrc_ev_active(profile, ~(UINT32_C(1) << path), now);
        unsigned int active = 0;
        for (; others; others &= others - 1)
            active++;
        if (active < profile->min_active)
            return;
    }
    transition(ev, MRC_EV_ASSUMED_BAD, now, now);
    profile->failed++;
    profile->failed_mask |= UINT32_C(1) << path;
}

void mrc_ev_feedback(struct mrc_ev_profile *profile, unsigned int path,
                     enum mrc_ev_feedback feedback, bool probe,
                     uint64_t probe_sent_at, uint64_t now)
{
    struct mrc_ev_path *ev = &profile->paths[path];
    if (ev->state == MRC_EV_DENIED)
        return;
    if (feedback == MRC_EV_ALWAYS_SKIP) {
        mrc_ev_timeout(profile, path, now);
        return;
    }

    if (ev->state == MRC_EV_ASSUMED_BAD) {
        if (!probe || probe_sent_at < ev->changed_at)
            return;
        transition(ev, MRC_EV_GOOD, now, 0);
        profile->recovered++;
        profile->recovered_mask |= UINT32_C(1) << path;
    }

    if (feedback == MRC_EV_SKIP_ONCE) {
        if (ev->state != MRC_EV_SKIP)
            profile->skipped++;
        transition(ev, MRC_EV_SKIP, now, now + profile->skip_interval);
    }
}

uint32_t mrc_ev_active(struct mrc_ev_profile *profile, uint32_t available, uint64_t now)
{
    uint32_t active = 0;
    for (unsigned int path = 0; path < profile->count; path++) {
        struct mrc_ev_path *ev = &profile->paths[path];
        if (ev->state == MRC_EV_SKIP && now >= ev->next_action)
            transition(ev, MRC_EV_GOOD, now, 0);
        if (ev->state == MRC_EV_GOOD)
            active |= UINT32_C(1) << path;
    }
    return active & available;
}

int mrc_ev_pick(struct mrc_ev_profile *profile, uint32_t available,
                uint32_t avoid, uint64_t now)
{
    uint32_t active = mrc_ev_active(profile, available, now);
    if (active & ~avoid)
        active &= ~avoid;
    for (unsigned int step = 0; step < profile->count; step++) {
        unsigned int path = (profile->cursor + step) % profile->count;
        if (active & (UINT32_C(1) << path))
            return (int)path;
    }
    return -1;
}

void mrc_ev_sent(struct mrc_ev_profile *profile, unsigned int path)
{
    profile->cursor = (path + 1) % profile->count;
}

bool mrc_ev_probe_due(const struct mrc_ev_profile *profile, unsigned int path, uint64_t now)
{
    const struct mrc_ev_path *ev = &profile->paths[path];
    return ev->state == MRC_EV_ASSUMED_BAD && now >= ev->next_action;
}

bool mrc_ev_probe_allowed(const struct mrc_ev_profile *profile, unsigned int path)
{
    return profile->paths[path].state != MRC_EV_DENIED;
}

void mrc_ev_probe_attempt(struct mrc_ev_profile *profile, unsigned int path, uint64_t now)
{
    if (profile->paths[path].state == MRC_EV_ASSUMED_BAD)
        profile->paths[path].next_action = now + profile->probe_interval;
}
