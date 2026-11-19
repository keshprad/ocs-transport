#include "mrc_ev.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct mrc_ev_profile profile;
    assert(!mrc_ev_init(&profile, 0, 0, 5, 10));
    assert(!mrc_ev_init(&profile, 33, 0, 5, 10));
    assert(mrc_ev_init(&profile, 2, 0, 5, 10));

    assert(mrc_ev_pick(&profile, 3, 0, 0) == 0);
    mrc_ev_sent(&profile, 0);
    assert(mrc_ev_pick(&profile, 3, 0, 0) == 1);
    assert(mrc_ev_pick(&profile, 1, 0, 0) == 0);
    assert(mrc_ev_pick(&profile, 3, 2, 0) == 0);
    assert(mrc_ev_pick(&profile, 2, 2, 0) == 1);

    mrc_ev_feedback(&profile, 1, MRC_EV_SKIP_ONCE, false, 0, 10);
    assert(profile.paths[1].state == MRC_EV_SKIP);
    assert(profile.skipped == 1 && mrc_ev_active(&profile, 3, 14) == 1);
    assert(mrc_ev_active(&profile, 3, 15) == 3);

    mrc_ev_timeout(&profile, 1, 20);
    assert(profile.paths[1].state == MRC_EV_ASSUMED_BAD);
    assert(profile.failed == 1 && mrc_ev_probe_due(&profile, 1, 20));
    mrc_ev_probe_attempt(&profile, 1, 20);
    assert(!mrc_ev_probe_due(&profile, 1, 29));
    assert(mrc_ev_probe_due(&profile, 1, 30));
    mrc_ev_feedback(&profile, 1, MRC_EV_UNMARKED, true, 19, 30);
    mrc_ev_feedback(&profile, 1, MRC_EV_UNMARKED, false, 0, 30);
    assert(mrc_ev_active(&profile, 3, 31) == 1);
    mrc_ev_feedback(&profile, 1, MRC_EV_SKIP_ONCE, true, 30, 32);
    assert(profile.recovered == 1 && profile.skipped == 2);
    assert(mrc_ev_active(&profile, 3, 36) == 1);
    assert(mrc_ev_active(&profile, 3, 37) == 3);
    mrc_ev_feedback(&profile, 1, MRC_EV_ALWAYS_SKIP, false, 0, 38);
    assert(profile.failed == 2 && mrc_ev_active(&profile, 3, 39) == 1);
    mrc_ev_feedback(&profile, 1, MRC_EV_UNMARKED, true, 40, 41);
    assert(profile.recovered == 2 && mrc_ev_active(&profile, 3, 41) == 3);

    mrc_ev_deny(&profile, 1, true, 42);
    mrc_ev_feedback(&profile, 1, MRC_EV_UNMARKED, true, 42, 43);
    mrc_ev_timeout(&profile, 0, 43);
    assert(!mrc_ev_probe_allowed(&profile, 1));
    assert(!mrc_ev_probe_due(&profile, 1, 100));
    assert(mrc_ev_pick(&profile, 3, 0, 100) == -1);
    mrc_ev_deny(&profile, 1, false, 100);
    assert(mrc_ev_pick(&profile, 3, 0, 101) == 1);

    assert(!mrc_ev_init(&profile, 2, 3, 5, 10));
    assert(mrc_ev_init(&profile, 2, 1, 5, 10));
    mrc_ev_timeout(&profile, 0, 110);
    mrc_ev_timeout(&profile, 1, 111);
    assert(profile.failed == 1 && mrc_ev_active(&profile, 3, 111) == 2);
    mrc_ev_deny(&profile, 1, true, 112);
    assert(mrc_ev_active(&profile, 3, 112) == 0);

    puts("MRC EV state tests passed");
    return 0;
}
