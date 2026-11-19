#ifndef MRC_CTL_STORE_H
#define MRC_CTL_STORE_H

#include "mrc_ev.h"

#include <stddef.h>

/* Local Linux controller storage. The caller supplies the directory and
 * enforces its administrative policy. This interface is independent of the
 * upstream mrc_ctl ABI and stores expanded 32-bit EV values. */
#define MRC_CTL_STORE_MAX_PROFILES 32u
#define MRC_CTL_STORE_INITIALIZER { .fd = -1 }
#define MRC_CTL_STORE_REF_INITIALIZER { .fd = -1 }

enum mrc_ctl_store_state {
    MRC_CTL_STORE_INIT,
    MRC_CTL_STORE_OFFLINE,
    MRC_CTL_STORE_ONLINE,
};

enum mrc_ctl_store_mode {
    MRC_CTL_STORE_AUTO = 1,
    MRC_CTL_STORE_EXPLICIT,
    MRC_CTL_STORE_GENERATED,
};

enum mrc_ctl_store_format {
    MRC_CTL_STORE_FORMAT_LEGACY = 0,
    MRC_CTL_STORE_FORMAT_UDP = 1,
    MRC_CTL_STORE_FORMAT_STEV = 2,
};

struct mrc_ctl_store_config {
    enum mrc_ctl_store_mode mode;
    enum mrc_ctl_store_format format;
    uint8_t count;
    uint8_t min_active;
    uint32_t populated; /* Bit i identifies a configured entropy slot. */
    uint32_t denied; /* Administrative DENY updates are allowed while ONLINE. */
    uint32_t entropy[MRC_EV_MAX_PATHS];
};

struct mrc_ctl_store_profile {
    uint64_t id;
    enum mrc_ctl_store_state state;
    struct mrc_ctl_store_config config;
};

struct mrc_ctl_store {
    int fd;
};

struct mrc_ctl_store_ref {
    int fd;
};

/* Initialize handles before use. Functions return zero or negative errno.
 * Keep the directory and lock inode stable. The controller's files are 0600
 * by default. A setgid directory with group read and search access enables
 * 0640 files for the directory's group, such as a directory with mode 02750.
 * The lock inode must have that same group. A valid writer commit upgrades
 * legacy owner-only files in a shared directory. */
int mrc_ctl_store_open(struct mrc_ctl_store *store, const char *directory);
void mrc_ctl_store_close(struct mrc_ctl_store *store);

int mrc_ctl_store_create(const struct mrc_ctl_store *store, uint64_t id);
int mrc_ctl_store_get(const struct mrc_ctl_store *store, uint64_t id,
                      struct mrc_ctl_store_profile *profile);
/* Returns the required count with -ENOSPC if the supplied array is too small. */
int mrc_ctl_store_list(const struct mrc_ctl_store *store, struct mrc_ctl_store_profile *profiles,
                       size_t capacity, size_t *count);
/* Configures INIT to OFFLINE, or replaces an OFFLINE configuration. AUTO
 * uses provider-selected entropies. GENERATED stores caller-expanded values. */
int mrc_ctl_store_configure(const struct mrc_ctl_store *store, uint64_t id,
                            const struct mrc_ctl_store_config *config);
int mrc_ctl_store_set_state(const struct mrc_ctl_store *store, uint64_t id,
                            enum mrc_ctl_store_state state);
int mrc_ctl_store_remove(const struct mrc_ctl_store *store, uint64_t id);
/* Edit a copy under the exclusive metadata lock. A missing profile starts
 * in INIT with the requested ID. Return an error to discard all changes,
 * or clear the ID to remove the record. An ONLINE edit can change only
 * the deny mask until the profile goes offline or is removed. Do not
 * reenter this store from the callback. */
int mrc_ctl_store_edit(const struct mrc_ctl_store *store, uint64_t id,
                       int (*edit)(struct mrc_ctl_store_profile *, void *), void *arg);
int mrc_ctl_store_update_denied(const struct mrc_ctl_store *store, uint64_t id,
                               uint32_t set, uint32_t clear);

/* Receive an ONLINE snapshot and hold a shared reference until release or
 * process exit. Query the store again to refresh administrative state. */
int mrc_ctl_store_acquire(const struct mrc_ctl_store *store, uint64_t id,
                          struct mrc_ctl_store_ref *ref, struct mrc_ctl_store_profile *profile);
void mrc_ctl_store_release(struct mrc_ctl_store_ref *ref);

#endif
