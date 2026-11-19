#ifndef MRC_EV_H
#define MRC_EV_H

#include <stdbool.h>
#include <stdint.h>

#define MRC_EV_MAX_PATHS 32u

enum mrc_ev_state {
    MRC_EV_GOOD,
    MRC_EV_DENIED,
    MRC_EV_SKIP,
    MRC_EV_ASSUMED_BAD,
};

enum mrc_ev_feedback {
    MRC_EV_UNMARKED,
    MRC_EV_SKIP_ONCE,
    MRC_EV_ALWAYS_SKIP,
};

struct mrc_ev_path {
    enum mrc_ev_state state;
    uint64_t changed_at;
    uint64_t next_action;
};

struct mrc_ev_profile {
    struct mrc_ev_path paths[MRC_EV_MAX_PATHS];
    uint64_t skip_interval;
    uint64_t probe_interval;
    unsigned int count;
    unsigned int min_active;
    unsigned int cursor;
    uint32_t skipped;
    uint32_t failed;
    uint32_t recovered;
    uint32_t failed_mask;
    uint32_t recovered_mask;
};

bool mrc_ev_init(struct mrc_ev_profile *profile, unsigned int count, unsigned int min_active,
                 uint64_t skip_interval, uint64_t probe_interval);

void mrc_ev_deny(struct mrc_ev_profile *profile, unsigned int path, bool denied,
                 uint64_t now);
void mrc_ev_timeout(struct mrc_ev_profile *profile, unsigned int path, uint64_t now);
void mrc_ev_feedback(struct mrc_ev_profile *profile, unsigned int path,
                     enum mrc_ev_feedback feedback, bool probe,
                     uint64_t probe_sent_at, uint64_t now);

uint32_t mrc_ev_active(struct mrc_ev_profile *profile, uint32_t available, uint64_t now);
int mrc_ev_pick(struct mrc_ev_profile *profile, uint32_t available,
                uint32_t avoid, uint64_t now);
void mrc_ev_sent(struct mrc_ev_profile *profile, unsigned int path);

bool mrc_ev_probe_due(const struct mrc_ev_profile *profile, unsigned int path, uint64_t now);
bool mrc_ev_probe_allowed(const struct mrc_ev_profile *profile, unsigned int path);
void mrc_ev_probe_attempt(struct mrc_ev_profile *profile, unsigned int path, uint64_t now);

#endif
