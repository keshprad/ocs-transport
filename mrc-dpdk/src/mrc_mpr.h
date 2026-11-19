#ifndef MRC_MPR_H
#define MRC_MPR_H

#include <stdbool.h>
#include <stdint.h>

/* OCP MRC 1.0 §7.5.4. Each MPR unit represents 128 packets. */
struct mrc_mpr {
    bool dynamic;
    uint8_t max_units;
    uint8_t units;
};

enum mrc_mpr_update_result {
    MRC_MPR_IGNORED,
    MRC_MPR_UNCHANGED,
    MRC_MPR_CHANGED,
    MRC_MPR_ZERO,
    MRC_MPR_EXCEEDS_MAX,
};

/* Pass the directional maximum from QP setup and the negotiated capability. */
bool mrc_mpr_init(struct mrc_mpr *mpr, bool dynamic, uint8_t max_units);

/* For an accepted SACK, compare against the QP cack before processing it.
 * The caller may report an invalid enabled-mode MPR to its application. */
enum mrc_mpr_update_result mrc_mpr_update(struct mrc_mpr *mpr, uint32_t previous_cack,
                                          uint32_t sack_cack, uint8_t sack_units);

uint32_t mrc_mpr_packets(const struct mrc_mpr *mpr);

/* Apply to both new transmissions and retries. Existing QP tracking validates
 * that any PSN at or before cack actually belongs to a pending operation. */
bool mrc_mpr_can_transmit(const struct mrc_mpr *mpr, uint32_t cack, uint32_t psn);

/* Set a responder's advertised limit without discarding tracked receive state. */
bool mrc_mpr_set_advertised(struct mrc_mpr *mpr, uint8_t units);
uint8_t mrc_mpr_advertised(const struct mrc_mpr *mpr);

#endif
