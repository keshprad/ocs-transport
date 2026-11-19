#include "mrc_mpr.h"
#include "mrc_qp.h"

bool mrc_mpr_init(struct mrc_mpr *mpr, bool dynamic, uint8_t max_units)
{
    if (!mpr || !max_units)
        return false;

    *mpr = (struct mrc_mpr){
        .dynamic = dynamic, .max_units = max_units, .units = max_units,
    };
    return true;
}

enum mrc_mpr_update_result mrc_mpr_update(struct mrc_mpr *mpr, uint32_t previous_cack,
                                          uint32_t sack_cack, uint8_t sack_units)
{
    if (!mpr->dynamic)
        return MRC_MPR_IGNORED;
    if (!sack_units)
        return MRC_MPR_ZERO;
    if (previous_cack > MRC_PSN_MASK || sack_cack > MRC_PSN_MASK ||
        mrc_psn_delta(sack_cack, previous_cack) <= 0)
        return MRC_MPR_IGNORED;
    if (sack_units > mpr->max_units)
        return MRC_MPR_EXCEEDS_MAX;
    if (sack_units == mpr->units)
        return MRC_MPR_UNCHANGED;

    mpr->units = sack_units;
    return MRC_MPR_CHANGED;
}

uint32_t mrc_mpr_packets(const struct mrc_mpr *mpr)
{
    return mpr->units * MRC_MPR_UNIT;
}

bool mrc_mpr_can_transmit(const struct mrc_mpr *mpr, uint32_t cack, uint32_t psn)
{
    if (!mpr->units || cack > MRC_PSN_MASK || psn > MRC_PSN_MASK)
        return false;

    int32_t distance = mrc_psn_delta(psn, cack);
    return distance > -(int32_t)MRC_PSN_HALF &&
           distance <= (int32_t)mrc_mpr_packets(mpr);
}

bool mrc_mpr_set_advertised(struct mrc_mpr *mpr, uint8_t units)
{
    if (!mpr->dynamic || !units || units > mpr->max_units)
        return false;

    mpr->units = units;
    return true;
}

uint8_t mrc_mpr_advertised(const struct mrc_mpr *mpr)
{
    return mpr->dynamic ? mpr->units : 0;
}
