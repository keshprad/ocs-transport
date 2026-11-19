#define _GNU_SOURCE
#include "mrc_ctl_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    HEADER_SIZE = 16,
    LEGACY_VERSION = 1,
    TYPED_VERSION = 2,
    LEGACY_ENTRY_HEADER_SIZE = 20,
    ENTRY_HEADER_SIZE = 24,
    ENTROPY_SIZE = 4 * MRC_EV_MAX_PATHS,
    ENTRY_SIZE = ENTRY_HEADER_SIZE + ENTROPY_SIZE,
    LEGACY_SNAPSHOT_SIZE = HEADER_SIZE + MRC_CTL_STORE_MAX_PROFILES *
        (LEGACY_ENTRY_HEADER_SIZE + ENTROPY_SIZE),
    SNAPSHOT_SIZE = HEADER_SIZE + MRC_CTL_STORE_MAX_PROFILES * ENTRY_SIZE,
};

/* Use the existing filename and lock inode when migrating the wire version. */
static const char snapshot_name[] = "profiles.v1";
static const char lock_name[] = "profiles.lock";
static const uint8_t magic[] = { 'M', 'R', 'C', '-', 'E', 'V', 0 };

struct transaction {
    int fd;
    struct mrc_ctl_store_profile profiles[MRC_CTL_STORE_MAX_PROFILES];
};

static uint32_t path_mask(unsigned int count)
{
    return count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1;
}

static bool config_valid(const struct mrc_ctl_store_config *config)
{
    if (config->mode < MRC_CTL_STORE_AUTO || config->mode > MRC_CTL_STORE_GENERATED ||
        (unsigned int)config->format > MRC_CTL_STORE_FORMAT_STEV ||
        !config->count || config->count > MRC_EV_MAX_PATHS || config->min_active > config->count)
        return false;

    uint32_t paths = path_mask(config->count);
    if ((config->denied | config->populated) & ~paths ||
        (config->mode == MRC_CTL_STORE_AUTO && config->populated))
        return false;

    for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++) {
        if (!(config->populated & (UINT32_C(1) << i)) && config->entropy[i])
            return false;
    }
    return true;
}

static bool config_ready(const struct mrc_ctl_store_config *config)
{
    return config->mode == MRC_CTL_STORE_AUTO || config->populated == path_mask(config->count);
}

static bool profile_valid(const struct mrc_ctl_store_profile *profile)
{
    const struct mrc_ctl_store_config *config = &profile->config;
    switch (profile->state) {
    case MRC_CTL_STORE_INIT:
        if (config->mode || config->format || config->count || config->min_active ||
            config->populated || config->denied)
            return false;
        for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++) {
            if (config->entropy[i])
                return false;
        }
        return true;
    case MRC_CTL_STORE_OFFLINE:
        return config_valid(config);
    case MRC_CTL_STORE_ONLINE:
        return config_valid(config) && config_ready(config);
    default:
        return false;
    }
}

static bool same_layout(const struct mrc_ctl_store_config *previous,
                        const struct mrc_ctl_store_config *updated)
{
    return previous->mode == updated->mode && previous->format == updated->format &&
           previous->count == updated->count && previous->min_active == updated->min_active &&
           previous->populated == updated->populated &&
           memcmp(previous->entropy, updated->entropy, sizeof(previous->entropy)) == 0;
}

static uint32_t read32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void write32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint32_t checksum(const uint8_t *bytes, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

static bool decode_profile(const uint8_t *bytes, unsigned int header_size,
                           struct mrc_ctl_store_profile *profile)
{
    uint64_t id = (uint64_t)read32(bytes) << 32 | read32(bytes + 4);
    if (!id || bytes[8] == MRC_CTL_STORE_INIT) {
        /* Empty slots and INIT profiles never contain configuration. */
        for (unsigned int i = 8; i < header_size + ENTROPY_SIZE; i++) {
            if (bytes[i])
                return false;
        }
        *profile = (struct mrc_ctl_store_profile){ .id = id };
        return true;
    }

    bool typed = header_size == ENTRY_HEADER_SIZE;
    if (typed && (bytes[21] || bytes[22] || bytes[23]))
        return false;
    *profile = (struct mrc_ctl_store_profile){
        .id = id, .state = bytes[8],
        .config = {
            .mode = bytes[9], .count = bytes[10], .min_active = bytes[11],
            .format = typed ? bytes[20] : MRC_CTL_STORE_FORMAT_LEGACY,
            .populated = read32(bytes + 12), .denied = read32(bytes + 16),
        },
    };
    for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++)
        profile->config.entropy[i] = read32(bytes + header_size + 4 * i);
    return profile_valid(profile);
}

static void encode_profile(uint8_t *bytes, const struct mrc_ctl_store_profile *profile)
{
    const struct mrc_ctl_store_config *config = &profile->config;
    write32(bytes, (uint32_t)(profile->id >> 32));
    write32(bytes + 4, (uint32_t)profile->id);
    bytes[8] = (uint8_t)profile->state;
    bytes[9] = (uint8_t)config->mode;
    bytes[10] = config->count;
    bytes[11] = config->min_active;
    write32(bytes + 12, config->populated);
    write32(bytes + 16, config->denied);
    bytes[20] = (uint8_t)config->format;
    for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++)
        write32(bytes + ENTRY_HEADER_SIZE + 4 * i, config->entropy[i]);
}

static int read_all(int fd, uint8_t *bytes, size_t length)
{
    while (length) {
        ssize_t n = read(fd, bytes, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -errno;
        if (!n)
            return -EBADMSG;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const uint8_t *bytes, size_t length)
{
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -errno;
        if (!n)
            return -EIO;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static int sync_file(int fd)
{
    int result;
    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : 0;
}

static int load_snapshot(const struct mrc_ctl_store *store, struct transaction *txn)
{
    int fd = openat(store->fd, snapshot_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -errno;

    uint8_t bytes[SNAPSHOT_SIZE];
    struct stat status;
    int result = 0;
    size_t length = 0;
    if (fstat(fd, &status) < 0) {
        result = -errno;
    } else if (!S_ISREG(status.st_mode) ||
               (status.st_size != LEGACY_SNAPSHOT_SIZE && status.st_size != SNAPSHOT_SIZE)) {
        result = -EBADMSG;
    } else {
        length = (size_t)status.st_size;
        result = read_all(fd, bytes, length);
    }
    close(fd);
    if (result)
        return result;

    uint8_t version = bytes[sizeof(magic)];
    unsigned int header_size = version == LEGACY_VERSION ? LEGACY_ENTRY_HEADER_SIZE : ENTRY_HEADER_SIZE;
    unsigned int entry_size = header_size + ENTROPY_SIZE;
    if (memcmp(bytes, magic, sizeof(magic)) || (version != LEGACY_VERSION && version != TYPED_VERSION) ||
        length != HEADER_SIZE + MRC_CTL_STORE_MAX_PROFILES * entry_size || read32(bytes + 12) ||
        read32(bytes + 8) != checksum(bytes + HEADER_SIZE, length - HEADER_SIZE))
        return -EBADMSG;

    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        struct mrc_ctl_store_profile *profile = &txn->profiles[i];
        if (!decode_profile(bytes + HEADER_SIZE + i * entry_size, header_size, profile))
            return -EBADMSG;
        for (unsigned int j = 0; profile->id && j < i; j++) {
            if (profile->id == txn->profiles[j].id)
                return -EBADMSG;
        }
    }
    return 0;
}

static int create_temp_file(const struct mrc_ctl_store *store, char *name, size_t length)
{
    for (unsigned int attempt = 0; attempt < 64; attempt++) {
        snprintf(name, length, ".mrc-profile.tmp.%ld.%u", (long)getpid(), attempt);
        int fd = openat(store->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd >= 0 ? fd : -errno;
    }
    return -EEXIST;
}

static int set_file_permissions(const struct mrc_ctl_store *store, const struct transaction *txn,
                                mode_t *snapshot_mode)
{
    struct stat directory, lock;
    if (fstat(store->fd, &directory) < 0 || fstat(txn->fd, &lock) < 0)
        return -errno;

    const mode_t shared = S_ISGID | S_IRGRP | S_IXGRP;
    bool share = (directory.st_mode & shared) == shared;
    *snapshot_mode = share ? 0640 : 0600;
    /* A legacy lock may predate the directory's group assignment. Do not
     * make it readable by that other group. Preserve the stable lock inode. */
    mode_t lock_mode = share && lock.st_gid == directory.st_gid ? 0640 : 0600;
    if ((lock.st_mode & 07777) != lock_mode && fchmod(txn->fd, lock_mode) < 0)
        return -errno;
    return 0;
}

static int save_snapshot(const struct mrc_ctl_store *store, const struct transaction *txn)
{
    mode_t mode = 0600;
    int result = set_file_permissions(store, txn, &mode);
    if (result)
        return result;

    uint8_t bytes[SNAPSHOT_SIZE] = {0};
    memcpy(bytes, magic, sizeof(magic));
    bytes[sizeof(magic)] = TYPED_VERSION;
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++)
        encode_profile(bytes + HEADER_SIZE + i * ENTRY_SIZE, &txn->profiles[i]);
    write32(bytes + 8, checksum(bytes + HEADER_SIZE, sizeof(bytes) - HEADER_SIZE));

    char temp_name[64];
    int fd = create_temp_file(store, temp_name, sizeof(temp_name));
    if (fd < 0)
        return fd;

    result = write_all(fd, bytes, sizeof(bytes));
    if (!result && fchmod(fd, mode) < 0)
        result = -errno;
    if (!result)
        result = sync_file(fd);
    if (close(fd) < 0 && !result)
        result = -errno;
    if (!result && renameat(store->fd, temp_name, store->fd, snapshot_name) < 0)
        result = -errno;
    if (result) {
        unlinkat(store->fd, temp_name, 0);
        return result;
    }

    /* After rename, an fsync error means the commit's durability is uncertain. */
    return sync_file(store->fd);
}

static int lock_file(int fd, int operation)
{
    int result;
    do {
        result = flock(fd, operation);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : 0;
}

static int begin(const struct mrc_ctl_store *store, struct transaction *txn, bool write)
{
    if (!store || store->fd < 0)
        return -EINVAL;

    *txn = (struct transaction){ .fd = -1 };
    int flags = write ? O_RDWR | O_CREAT : O_RDONLY;
    int fd = openat(store->fd, lock_name, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
        return -errno;

    struct stat status;
    int result;
    if (fstat(fd, &status) < 0)
        result = -errno;
    else if (!S_ISREG(status.st_mode))
        result = -EBADMSG;
    else
        result = lock_file(fd, write ? LOCK_EX : LOCK_SH);
    if (!result)
        result = load_snapshot(store, txn);
    if (result)
        close(fd);
    else
        txn->fd = fd;
    return result;
}

static int finish(const struct mrc_ctl_store *store, const struct transaction *txn,
                  int result, bool save)
{
    if (!result && save)
        result = save_snapshot(store, txn);
    close(txn->fd);
    return result;
}

static int find_profile(const struct transaction *txn, uint64_t id)
{
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        if (txn->profiles[i].id == id)
            return (int)i;
    }
    return -ENOENT;
}

static int begin_profile(const struct mrc_ctl_store *store, struct transaction *txn,
                          uint64_t id, bool write)
{
    if (!id)
        return -EINVAL;
    int result = begin(store, txn, write);
    if (result)
        return result;
    int slot = find_profile(txn, id);
    if (slot < 0)
        return finish(store, txn, slot, false);
    return slot;
}

/* The metadata lock serializes slot reuse with acquisition of a lease.
 * Each lease is owned by its open file description, independent of the
 * store handle, and the kernel drops it when its last descriptor closes. */
static int lock_reference(const struct transaction *txn, int slot, short type)
{
    struct flock lock = {
        .l_type = type, .l_whence = SEEK_SET, .l_start = slot, .l_len = 1,
    };
    int result;
    do {
        result = fcntl(txn->fd, F_OFD_SETLK, &lock);
    } while (result < 0 && errno == EINTR);
    if (!result)
        return 0;
    return errno == EACCES || errno == EAGAIN ? -EBUSY : -errno;
}

int mrc_ctl_store_open(struct mrc_ctl_store *store, const char *directory)
{
    if (!store || !directory || !*directory)
        return -EINVAL;
    if (store->fd >= 0)
        return -EBUSY;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -errno;
    store->fd = fd;
    return 0;
}

void mrc_ctl_store_close(struct mrc_ctl_store *store)
{
    if (store && store->fd >= 0) {
        close(store->fd);
        store->fd = -1;
    }
}

int mrc_ctl_store_create(const struct mrc_ctl_store *store, uint64_t id)
{
    if (!id)
        return -EINVAL;
    struct transaction txn;
    int result = begin(store, &txn, true);
    if (result)
        return result;
    int slot = find_profile(&txn, id);
    if (slot >= 0)
        return finish(store, &txn, -EEXIST, false);
    slot = find_profile(&txn, 0);
    if (slot < 0)
        return finish(store, &txn, -ENOSPC, false);
    txn.profiles[slot] = (struct mrc_ctl_store_profile){ .id = id };
    return finish(store, &txn, 0, true);
}

int mrc_ctl_store_get(const struct mrc_ctl_store *store, uint64_t id,
                      struct mrc_ctl_store_profile *profile)
{
    if (!store || store->fd < 0 || !id || !profile)
        return -EINVAL;

    /* Writers publish a new inode by rename, so an open snapshot remains
     * stable while a writer persists or publishes its replacement. */
    struct transaction txn = {0};
    int result = load_snapshot(store, &txn);
    if (result)
        return result;
    int slot = find_profile(&txn, id);
    if (slot < 0)
        return slot;
    *profile = txn.profiles[slot];
    return 0;
}

int mrc_ctl_store_list(const struct mrc_ctl_store *store, struct mrc_ctl_store_profile *profiles,
                       size_t capacity, size_t *count)
{
    if (!store || store->fd < 0 || !count || (capacity && !profiles))
        return -EINVAL;
    struct transaction txn = {0};
    int result = load_snapshot(store, &txn);
    if (result)
        return result;

    size_t required = 0;
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++)
        required += txn.profiles[i].id != 0;
    *count = required;
    if (required > capacity)
        return -ENOSPC;

    size_t next = 0;
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        if (txn.profiles[i].id)
            profiles[next++] = txn.profiles[i];
    }
    return 0;
}

int mrc_ctl_store_configure(const struct mrc_ctl_store *store, uint64_t id,
                            const struct mrc_ctl_store_config *config)
{
    if (!config || !config_valid(config))
        return -EINVAL;
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    struct mrc_ctl_store_profile *profile = &txn.profiles[slot];
    if (profile->state == MRC_CTL_STORE_ONLINE)
        return finish(store, &txn, -EBUSY, false);
    int result = lock_reference(&txn, slot, F_WRLCK);
    if (!result) {
        profile->config = *config;
        profile->state = MRC_CTL_STORE_OFFLINE;
    }
    return finish(store, &txn, result, true);
}

int mrc_ctl_store_set_state(const struct mrc_ctl_store *store, uint64_t id,
                            enum mrc_ctl_store_state state)
{
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    struct mrc_ctl_store_profile *profile = &txn.profiles[slot];
    if (state == profile->state)
        return finish(store, &txn, 0, false);
    bool valid = (profile->state == MRC_CTL_STORE_ONLINE && state == MRC_CTL_STORE_OFFLINE) ||
                 (profile->state == MRC_CTL_STORE_OFFLINE &&
                  (state == MRC_CTL_STORE_INIT ||
                   (state == MRC_CTL_STORE_ONLINE && config_ready(&profile->config))));
    if (!valid)
        return finish(store, &txn, -EINVAL, false);
    int result = lock_reference(&txn, slot, F_WRLCK);
    if (!result) {
        profile->state = state;
        if (state == MRC_CTL_STORE_INIT)
            profile->config = (struct mrc_ctl_store_config){0};
    }
    return finish(store, &txn, result, true);
}

int mrc_ctl_store_remove(const struct mrc_ctl_store *store, uint64_t id)
{
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    if (txn.profiles[slot].state == MRC_CTL_STORE_ONLINE)
        return finish(store, &txn, -EBUSY, false);
    int result = lock_reference(&txn, slot, F_WRLCK);
    if (!result)
        txn.profiles[slot] = (struct mrc_ctl_store_profile){0};
    return finish(store, &txn, result, true);
}

int mrc_ctl_store_edit(const struct mrc_ctl_store *store, uint64_t id,
                       int (*edit)(struct mrc_ctl_store_profile *, void *), void *arg)
{
    if (!id || !edit)
        return -EINVAL;
    struct transaction txn;
    int result = begin(store, &txn, true);
    if (result)
        return result;

    int slot = find_profile(&txn, id);
    struct mrc_ctl_store_profile previous = slot >= 0 ? txn.profiles[slot] :
        (struct mrc_ctl_store_profile){ .id = id };
    struct mrc_ctl_store_profile updated = previous;
    result = edit(&updated, arg);
    if (result)
        return finish(store, &txn, result, false);

    if (!updated.id) {
        if (slot < 0)
            return finish(store, &txn, 0, false);
        updated = (struct mrc_ctl_store_profile){0};
    } else if (updated.id != id || !profile_valid(&updated)) {
        return finish(store, &txn, -EINVAL, false);
    }

    bool same = same_layout(&previous.config, &updated.config);
    if (slot >= 0 && updated.id && same && updated.state == previous.state &&
        updated.config.denied == previous.config.denied)
        return finish(store, &txn, 0, false);
    if (slot < 0) {
        slot = find_profile(&txn, 0);
        if (slot < 0)
            return finish(store, &txn, -ENOSPC, false);
    }

    if (previous.state == MRC_CTL_STORE_ONLINE && updated.state == MRC_CTL_STORE_ONLINE) {
        if (!same)
            return finish(store, &txn, -EBUSY, false);
    } else {
        result = lock_reference(&txn, slot, F_WRLCK);
        if (result)
            return finish(store, &txn, result, false);
    }
    txn.profiles[slot] = updated;
    return finish(store, &txn, 0, true);
}

int mrc_ctl_store_update_denied(const struct mrc_ctl_store *store, uint64_t id,
                               uint32_t set, uint32_t clear)
{
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    struct mrc_ctl_store_config *config = &txn.profiles[slot].config;
    if (txn.profiles[slot].state == MRC_CTL_STORE_INIT || (set & clear) ||
        ((set | clear) & ~path_mask(config->count)))
        return finish(store, &txn, -EINVAL, false);
    uint32_t updated = (config->denied | set) & ~clear;
    if (updated == config->denied)
        return finish(store, &txn, 0, false);
    config->denied = updated;
    return finish(store, &txn, 0, true);
}

int mrc_ctl_store_acquire(const struct mrc_ctl_store *store, uint64_t id,
                          struct mrc_ctl_store_ref *ref, struct mrc_ctl_store_profile *profile)
{
    if (!ref || !profile)
        return -EINVAL;
    if (ref->fd >= 0)
        return -EBUSY;
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, false);
    if (slot < 0)
        return slot;
    if (txn.profiles[slot].state != MRC_CTL_STORE_ONLINE)
        return finish(store, &txn, -EAGAIN, false);
    int result = lock_reference(&txn, slot, F_RDLCK);
    if (!result)
        result = lock_file(txn.fd, LOCK_UN);
    if (result)
        return finish(store, &txn, result, false);
    *profile = txn.profiles[slot];
    ref->fd = txn.fd;
    return 0;
}

void mrc_ctl_store_release(struct mrc_ctl_store_ref *ref)
{
    if (ref && ref->fd >= 0) {
        close(ref->fd);
        ref->fd = -1;
    }
}
