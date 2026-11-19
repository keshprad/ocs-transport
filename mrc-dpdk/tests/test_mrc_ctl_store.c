#define _POSIX_C_SOURCE 200809L
#include "mrc_ctl_store.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static struct mrc_ctl_store_config explicit_config(void)
{
    return (struct mrc_ctl_store_config){
        .mode = MRC_CTL_STORE_EXPLICIT, .count = 2, .min_active = 1,
        .populated = 3, .entropy = { 0x107c1401, 0x207c1402 },
    };
}

static void remove_fixture(struct mrc_ctl_store *store, const char *directory)
{
    assert(unlinkat(store->fd, "profiles.v1", 0) == 0);
    assert(unlinkat(store->fd, "profiles.lock", 0) == 0);
    mrc_ctl_store_close(store);
    assert(rmdir(directory) == 0);
}

static int child_status(pid_t pid)
{
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
}

static void test_lifecycle(void)
{
    char directory[] = "/tmp/mrc-ctl-lifecycle.XXXXXX";
    assert(mkdtemp(directory));
    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    struct mrc_ctl_store_ref ref = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_ref second_ref = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_profile record, previous;
    uint64_t id = UINT64_C(0x0123456789abcdef);
    size_t count = 1;

    assert(mrc_ctl_store_open(&store, directory) == 0);
    assert(mrc_ctl_store_open(&store, directory) == -EBUSY);
    assert(mrc_ctl_store_list(&store, NULL, 0, &count) == 0 && count == 0);
    assert(mrc_ctl_store_create(&store, 0) == -EINVAL);
    assert(mrc_ctl_store_create(&store, id) == 0);
    assert(mrc_ctl_store_create(&store, id) == -EEXIST);
    assert(mrc_ctl_store_list(&store, NULL, 0, &count) == -ENOSPC && count == 1);
    assert(mrc_ctl_store_list(&store, &record, 1, &count) == 0);
    assert(record.id == id && record.state == MRC_CTL_STORE_INIT);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_ONLINE) == -EINVAL);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_OFFLINE) == -EINVAL);
    assert(mrc_ctl_store_acquire(&store, id, &ref, &record) == -EAGAIN);
    assert(mrc_ctl_store_update_denied(&store, id, 1, 0) == -EINVAL);

    struct mrc_ctl_store_config config = explicit_config();
    config.count = MRC_EV_MAX_PATHS + 1;
    assert(mrc_ctl_store_configure(&store, id, &config) == -EINVAL);
    config = explicit_config();
    config.populated = 1;
    config.entropy[1] = 0;
    assert(mrc_ctl_store_configure(&store, id, &config) == 0);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_ONLINE) == -EINVAL);
    assert(mrc_ctl_store_get(&store, id, &record) == 0);
    assert(record.state == MRC_CTL_STORE_OFFLINE && record.config.populated == 1);

    config = explicit_config();
    config.entropy[1] = config.entropy[0];
    config.denied = 2;
    assert(mrc_ctl_store_configure(&store, id, &config) == 0);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_ONLINE) == 0);
    assert(mrc_ctl_store_acquire(&store, id, &ref, &previous) == 0);
    assert(mrc_ctl_store_acquire(&store, id, &ref, &record) == -EBUSY);
    assert(mrc_ctl_store_acquire(&store, id, &second_ref, &record) == 0);
    assert(record.id == id && record.config.entropy[0] == record.config.entropy[1]);
    assert(mrc_ctl_store_configure(&store, id, &config) == -EBUSY);
    assert(mrc_ctl_store_remove(&store, id) == -EBUSY);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_OFFLINE) == -EBUSY);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_ONLINE) == 0);

    assert(mrc_ctl_store_update_denied(&store, id, 1, 1) == -EINVAL);
    assert(mrc_ctl_store_update_denied(&store, id, 4, 0) == -EINVAL);
    assert(mrc_ctl_store_update_denied(&store, id, 1, 2) == 0);
    assert(mrc_ctl_store_get(&store, id, &record) == 0);
    assert(previous.config.denied == 2 && record.config.denied == 1);
    mrc_ctl_store_close(&store);
    assert(mrc_ctl_store_open(&store, directory) == 0);
    assert(mrc_ctl_store_get(&store, id, &record) == 0);
    assert(record.state == MRC_CTL_STORE_ONLINE && record.config.denied == 1);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_OFFLINE) == -EBUSY);
    mrc_ctl_store_release(&ref);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_OFFLINE) == -EBUSY);
    mrc_ctl_store_release(&second_ref);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_OFFLINE) == 0);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_INIT) == 0);
    assert(mrc_ctl_store_get(&store, id, &record) == 0);
    assert(record.id == id && record.state == MRC_CTL_STORE_INIT && record.config.count == 0);
    assert(mrc_ctl_store_remove(&store, id) == 0);
    assert(mrc_ctl_store_get(&store, id, &record) == -ENOENT);
    remove_fixture(&store, directory);
}

static void test_process_leases_and_modes(void)
{
    char directory[] = "/tmp/mrc-ctl-leases.XXXXXX";
    assert(mkdtemp(directory));
    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    assert(mrc_ctl_store_open(&store, directory) == 0);
    assert(mrc_ctl_store_create(&store, 50) == 0);
    struct mrc_ctl_store_config config = {
        .mode = MRC_CTL_STORE_AUTO, .count = 2, .min_active = 1,
    };
    config.populated = 1;
    assert(mrc_ctl_store_configure(&store, 50, &config) == -EINVAL);
    config.populated = 0;
    assert(mrc_ctl_store_configure(&store, 50, &config) == 0);
    assert(mrc_ctl_store_set_state(&store, 50, MRC_CTL_STORE_ONLINE) == 0);

    int ready[2];
    assert(pipe(ready) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(ready[0]);
        struct mrc_ctl_store_ref ref = MRC_CTL_STORE_REF_INITIALIZER;
        struct mrc_ctl_store_profile record;
        assert(mrc_ctl_store_acquire(&store, 50, &ref, &record) == 0);
        assert(record.config.mode == MRC_CTL_STORE_AUTO);
        assert(write(ready[1], "x", 1) == 1);
        close(ready[1]);
        pause();
        _exit(1);
    }
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    assert(mrc_ctl_store_update_denied(&store, 50, 3, 0) == 0);
    assert(mrc_ctl_store_set_state(&store, 50, MRC_CTL_STORE_OFFLINE) == -EBUSY);

    assert(mrc_ctl_store_create(&store, 51) == 0);
    config.mode = MRC_CTL_STORE_GENERATED;
    config.populated = 3;
    config.entropy[0] = 3;
    config.entropy[1] = 7;
    assert(mrc_ctl_store_configure(&store, 51, &config) == 0);
    assert(mrc_ctl_store_set_state(&store, 51, MRC_CTL_STORE_ONLINE) == 0);
    assert(mrc_ctl_store_set_state(&store, 51, MRC_CTL_STORE_OFFLINE) == 0);
    assert(mrc_ctl_store_remove(&store, 51) == 0);

    int status;
    assert(kill(pid, SIGKILL) == 0);
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    assert(mrc_ctl_store_set_state(&store, 50, MRC_CTL_STORE_OFFLINE) == 0);
    struct mrc_ctl_store_profile record;
    assert(mrc_ctl_store_get(&store, 50, &record) == 0);
    assert(record.config.denied == 3);
    assert(mrc_ctl_store_remove(&store, 50) == 0);
    remove_fixture(&store, directory);
}

static int deny_slot(struct mrc_ctl_store_profile *profile, void *arg)
{
    const uint32_t *slot = arg;
    assert(profile->state == MRC_CTL_STORE_ONLINE);
    profile->config.denied |= *slot;
    return 0;
}

static void test_concurrent_writers(void)
{
    char directory[] = "/tmp/mrc-ctl-writers.XXXXXX";
    assert(mkdtemp(directory));
    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    assert(mrc_ctl_store_open(&store, directory) == 0);
    int start[2];
    assert(pipe(start) == 0);

    enum { WORKERS = 8 };
    pid_t workers[WORKERS];
    for (unsigned int i = 0; i < WORKERS; i++) {
        workers[i] = fork();
        assert(workers[i] >= 0);
        if (workers[i] == 0) {
            close(start[1]);
            char unused;
            assert(read(start[0], &unused, 1) == 0);
            int result = mrc_ctl_store_create(&store, 5);
            _exit(result == 0 ? 0 : result == -EEXIST ? 1 : 2);
        }
    }
    close(start[0]);
    close(start[1]);
    unsigned int successes = 0;
    for (unsigned int i = 0; i < WORKERS; i++) {
        int status = child_status(workers[i]);
        assert(status <= 1);
        successes += status == 0;
    }
    assert(successes == 1);

    struct mrc_ctl_store_config config = { .mode = MRC_CTL_STORE_AUTO, .count = WORKERS };
    assert(mrc_ctl_store_configure(&store, 5, &config) == 0);
    assert(mrc_ctl_store_set_state(&store, 5, MRC_CTL_STORE_ONLINE) == 0);
    assert(pipe(start) == 0);
    for (unsigned int i = 0; i < WORKERS; i++) {
        workers[i] = fork();
        assert(workers[i] >= 0);
        if (workers[i] == 0) {
            close(start[1]);
            char unused;
            assert(read(start[0], &unused, 1) == 0);
            uint32_t bit = UINT32_C(1) << i;
            int result = i % 2 ? mrc_ctl_store_edit(&store, 5, deny_slot, &bit) :
                                 mrc_ctl_store_update_denied(&store, 5, bit, 0);
            _exit(result != 0);
        }
    }
    close(start[0]);
    close(start[1]);
    for (unsigned int i = 0; i < WORKERS; i++)
        assert(child_status(workers[i]) == 0);
    struct mrc_ctl_store_profile record;
    assert(mrc_ctl_store_get(&store, 5, &record) == 0);
    assert(record.config.denied == (UINT32_C(1) << WORKERS) - 1);

    for (unsigned int id = 0; id < MRC_CTL_STORE_MAX_PROFILES - 1; id++)
        assert(mrc_ctl_store_create(&store, 100 + id) == 0);
    assert(mrc_ctl_store_create(&store, 300) == -ENOSPC);
    size_t count = 0;
    assert(mrc_ctl_store_list(&store, NULL, 0, &count) == -ENOSPC);
    assert(count == MRC_CTL_STORE_MAX_PROFILES);
    remove_fixture(&store, directory);
}

static void test_failed_update_and_corruption(void)
{
    char directory[] = "/tmp/mrc-ctl-failure.XXXXXX";
    assert(mkdtemp(directory));
    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    assert(mrc_ctl_store_open(&store, directory) == 0);
    assert(mrc_ctl_store_create(&store, 5) == 0);
    struct mrc_ctl_store_config config = explicit_config();
    assert(mrc_ctl_store_configure(&store, 5, &config) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        const struct rlimit limit = { .rlim_cur = 16, .rlim_max = 16 };
        assert(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
        assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        _exit(mrc_ctl_store_update_denied(&store, 5, 1, 0) != -EFBIG);
    }
    assert(child_status(pid) == 0);
    struct mrc_ctl_store_profile record;
    assert(mrc_ctl_store_get(&store, 5, &record) == 0);
    assert(record.config.denied == 0 && record.state == MRC_CTL_STORE_OFFLINE);
    assert(mrc_ctl_store_update_denied(&store, 5, 1, 0) == 0);
    mrc_ctl_store_close(&store);
    assert(mrc_ctl_store_open(&store, directory) == 0);
    assert(mrc_ctl_store_get(&store, 5, &record) == 0 && record.config.denied == 1);

    int fd = openat(store.fd, "profiles.v1", O_RDWR);
    assert(fd >= 0);
    uint8_t byte;
    assert(pread(fd, &byte, 1, 20) == 1);
    byte ^= UINT8_C(0xff);
    assert(pwrite(fd, &byte, 1, 20) == 1);
    assert(close(fd) == 0);
    assert(mrc_ctl_store_get(&store, 5, &record) == -EBADMSG);
    assert(mrc_ctl_store_create(&store, 6) == -EBADMSG);
    assert(unlinkat(store.fd, "profiles.v1", 0) == 0);
    assert(symlinkat("profiles.lock", store.fd, "profiles.v1") == 0);
    assert(mrc_ctl_store_get(&store, 5, &record) == -ELOOP);
    remove_fixture(&store, directory);
}

static int init_typed_profile(struct mrc_ctl_store_profile *profile, void *arg)
{
    const uint64_t *expected_id = arg;
    assert(profile->id == *expected_id && profile->state == MRC_CTL_STORE_INIT);
    assert(!profile->config.mode && !profile->config.count);
    profile->state = MRC_CTL_STORE_OFFLINE;
    profile->config = explicit_config();
    profile->config.format = MRC_CTL_STORE_FORMAT_STEV;
    return 0;
}

static int replace_profile(struct mrc_ctl_store_profile *profile, void *arg)
{
    *profile = *(const struct mrc_ctl_store_profile *)arg;
    return 0;
}

static int clear_profile_id(struct mrc_ctl_store_profile *profile, void *arg)
{
    (void)arg;
    profile->id = 0;
    return 0;
}

static int cancel_profile_edit(struct mrc_ctl_store_profile *profile, void *arg)
{
    (void)arg;
    profile->config.format = MRC_CTL_STORE_FORMAT_UDP;
    return -ECANCELED;
}

static void assert_mode(const struct mrc_ctl_store *store, const char *name, mode_t mode)
{
    struct stat status;
    assert(fstatat(store->fd, name, &status, 0) == 0);
    assert(S_ISREG(status.st_mode) && (status.st_mode & 07777) == mode);
}

static void test_typed_edits(void)
{
    char directory[] = "/tmp/mrc-ctl-edits.XXXXXX";
    assert(mkdtemp(directory));
    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    assert(mrc_ctl_store_open(&store, directory) == 0);

    size_t count = 1;
    struct mrc_ctl_store_ref ref = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_profile recorded;
    uint64_t id = 457;
    assert(mrc_ctl_store_list(&store, NULL, 0, &count) == 0 && count == 0);
    assert(mrc_ctl_store_acquire(&store, id, &ref, &recorded) == -ENOENT);
    assert(faccessat(store.fd, "profiles.lock", F_OK, 0) == -1 && errno == ENOENT);

    mode_t mask = umask(0);
    assert(mrc_ctl_store_edit(&store, id, init_typed_profile, &id) == 0);
    umask(mask);
    assert_mode(&store, "profiles.lock", 0600);
    assert_mode(&store, "profiles.v1", 0600);
    assert(mrc_ctl_store_edit(&store, id, cancel_profile_edit, NULL) == -ECANCELED);
    assert(mrc_ctl_store_get(&store, id, &recorded) == 0);
    assert(recorded.config.format == MRC_CTL_STORE_FORMAT_STEV);

    struct mrc_ctl_store_profile updated = recorded;
    updated.config.format = MRC_CTL_STORE_FORMAT_STEV + 1;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == -EINVAL);
    updated = recorded;
    updated.id++;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == -EINVAL);
    updated = recorded;
    updated.state = MRC_CTL_STORE_ONLINE;
    updated.config.populated = 1;
    updated.config.entropy[1] = 0;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == -EINVAL);

    updated = recorded;
    updated.state = MRC_CTL_STORE_ONLINE;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == 0);
    assert(mrc_ctl_store_acquire(&store, id, &ref, &recorded) == 0);
    assert(recorded.config.format == MRC_CTL_STORE_FORMAT_STEV);
    updated.config.denied = 2;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == 0);
    assert(mrc_ctl_store_get(&store, id, &recorded) == 0 && recorded.config.denied == 2);
    updated.config.format = MRC_CTL_STORE_FORMAT_UDP;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == -EBUSY);
    updated.config.format = MRC_CTL_STORE_FORMAT_STEV;
    updated.state = MRC_CTL_STORE_OFFLINE;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == -EBUSY);
    assert(mrc_ctl_store_edit(&store, id, clear_profile_id, NULL) == -EBUSY);
    mrc_ctl_store_release(&ref);
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == 0);
    updated.state = MRC_CTL_STORE_ONLINE;
    assert(mrc_ctl_store_edit(&store, id, replace_profile, &updated) == 0);
    assert(mrc_ctl_store_edit(&store, id, clear_profile_id, NULL) == 0);
    assert(mrc_ctl_store_get(&store, id, &recorded) == -ENOENT);
    remove_fixture(&store, directory);
}

static void write_legacy_snapshot(const struct mrc_ctl_store *store)
{
    enum { HEADER = 16, ENTRY = 20 + 4 * MRC_EV_MAX_PATHS };
    uint8_t snapshot[HEADER + MRC_CTL_STORE_MAX_PROFILES * ENTRY] = {
        'M', 'R', 'C', '-', 'E', 'V', 0, 1,
        [HEADER + 7] = 17, /* ID, most-significant byte first. */
        [HEADER + 8] = 2, /* ONLINE */
        [HEADER + 9] = 2, /* EXPLICIT */
        [HEADER + 10] = 2, [HEADER + 11] = 1,
        [HEADER + 15] = 3, /* Slots 0 and 1 populated. */
        [HEADER + 23] = 49, [HEADER + 27] = 57,
    };
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = HEADER; i < sizeof(snapshot); i++) {
        hash ^= snapshot[i];
        hash *= UINT32_C(16777619);
    }
    for (unsigned int i = 0; i < 4; i++)
        snapshot[8 + i] = (uint8_t)(hash >> (24 - 8 * i));
    int fd = openat(store->fd, "profiles.v1", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, snapshot, sizeof(snapshot)) == (ssize_t)sizeof(snapshot));
    assert(close(fd) == 0);
    fd = openat(store->fd, "profiles.lock", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && close(fd) == 0);
}

static void test_legacy_shared_store(void)
{
    char directory[] = "/tmp/mrc-ctl-shared.XXXXXX";
    assert(mkdtemp(directory));
    assert(chmod(directory, 02750) == 0);
    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    assert(mrc_ctl_store_open(&store, directory) == 0);

    mode_t mask = umask(0077);
    write_legacy_snapshot(&store);
    assert_mode(&store, "profiles.lock", 0600);
    assert_mode(&store, "profiles.v1", 0600);
    struct mrc_ctl_store_profile record;
    assert(mrc_ctl_store_get(&store, 17, &record) == 0);
    assert(record.config.format == MRC_CTL_STORE_FORMAT_LEGACY && record.state == MRC_CTL_STORE_ONLINE);
    assert(record.config.entropy[0] == 49 && record.config.entropy[1] == 57);
    struct mrc_ctl_store_ref ref = MRC_CTL_STORE_REF_INITIALIZER;
    assert(mrc_ctl_store_acquire(&store, 17, &ref, &record) == 0);
    assert((fcntl(ref.fd, F_GETFL) & O_ACCMODE) == O_RDONLY);

    uint32_t bit = 1;
    assert(mrc_ctl_store_edit(&store, 17, deny_slot, &bit) == 0);
    assert_mode(&store, "profiles.lock", 0640);
    assert_mode(&store, "profiles.v1", 0640);
    assert(mrc_ctl_store_get(&store, 17, &record) == 0);
    assert(record.config.format == MRC_CTL_STORE_FORMAT_LEGACY && record.config.denied == 1);
    assert(mrc_ctl_store_set_state(&store, 17, MRC_CTL_STORE_OFFLINE) == -EBUSY);
    mrc_ctl_store_release(&ref);
    assert(mrc_ctl_store_set_state(&store, 17, MRC_CTL_STORE_OFFLINE) == 0);

    struct mrc_ctl_store_config config = explicit_config();
    config.format = MRC_CTL_STORE_FORMAT_UDP;
    config.entropy[0] = 49;
    config.entropy[1] = 57;
    assert(mrc_ctl_store_configure(&store, 17, &config) == 0);
    assert(mrc_ctl_store_get(&store, 17, &record) == 0);
    assert(record.config.format == MRC_CTL_STORE_FORMAT_UDP);
    umask(mask);
    remove_fixture(&store, directory);
}

static int configure_unconfigured_cc(struct mrc_ctl_store_cc_profile *profile, void *arg)
{
    const uint64_t *expected_id = arg;
    assert(profile->id == *expected_id && profile->state == MRC_CTL_STORE_INIT);
    assert(profile->algorithm == MRC_CC_DISABLED && !profile->configured);
    profile->state = MRC_CTL_STORE_OFFLINE;
    profile->algorithm = MRC_CC_NSCC;
    return 0;
}

static int replace_cc(struct mrc_ctl_store_cc_profile *profile, void *arg)
{
    *profile = *(const struct mrc_ctl_store_cc_profile *)arg;
    return 0;
}

static int delete_cc(struct mrc_ctl_store_cc_profile *profile, void *arg)
{
    (void)arg;
    profile->id = 0;
    return 0;
}

static void assert_cc_parameters(const struct mrc_cc_config *a, const struct mrc_cc_config *b)
{
    assert(a->disable_qa == b->disable_qa && a->qa_gate == b->qa_gate);
    assert(a->adjust_bytes_threshold == b->adjust_bytes_threshold);
    assert(a->adjust_period_threshold == b->adjust_period_threshold && a->base_rtt == b->base_rtt);
    assert(a->eta == b->eta && a->fi == b->fi && a->fi_scale == b->fi_scale);
    assert(a->gamma == b->gamma && a->max_md_jump == b->max_md_jump);
    assert(a->max_cwnd == b->max_cwnd && a->qa_threshold == b->qa_threshold);
    assert(a->target_delay == b->target_delay);
}

static void test_cc_profiles(void)
{
    char directory[] = "/tmp/mrc-ctl-cc.XXXXXX";
    assert(mkdtemp(directory));
    assert(chmod(directory, 02750) == 0);
    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    struct mrc_ctl_store_ref ev = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_ref cc = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_ref second_cc = MRC_CTL_STORE_REF_INITIALIZER;
    struct mrc_ctl_store_profile ev_profile;
    struct mrc_ctl_store_cc_profile record;
    uint64_t id = UINT64_C(0x0123456789abcdef);
    assert(mrc_ctl_store_open(&store, directory) == 0);
    assert(mrc_ctl_store_get_cc(&store, id, &record) == -ENOENT);
    assert(mrc_ctl_store_acquire_cc(&store, id, &cc, &record) == -ENOENT);
    assert(mrc_ctl_store_edit_cc(&store, id, delete_cc, NULL) == 0);

    /* Place the same ID in the first EV and CC slots, then hold the EV lease
     * while configuring CC and vice versa. */
    struct mrc_ctl_store_config ev_config = explicit_config();
    assert(mrc_ctl_store_create(&store, id) == 0);
    assert(mrc_ctl_store_configure(&store, id, &ev_config) == 0);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_ONLINE) == 0);
    assert(mrc_ctl_store_acquire(&store, id, &ev, &ev_profile) == 0);
    assert(mrc_ctl_store_edit_cc(&store, id, configure_unconfigured_cc, &id) == 0);
    assert(mrc_ctl_store_get_cc(&store, id, &record) == 0);
    assert(record.state == MRC_CTL_STORE_OFFLINE && record.algorithm == MRC_CC_NSCC && !record.configured);
    assert(mrc_ctl_store_acquire_cc(&store, id, &cc, &record) == -EAGAIN);

    struct mrc_ctl_store_cc_profile updated = record;
    updated.state = MRC_CTL_STORE_ONLINE;
    assert(mrc_ctl_store_edit_cc(&store, id, replace_cc, &updated) == -EINVAL);
    updated.config = (struct mrc_cc_config){
        .disable_qa = 1, .qa_gate = 4, .adjust_bytes_threshold = 8721,
        .adjust_period_threshold = 30001, .base_rtt = 10240, .eta = 0.75f,
        .fi = 0.31f, .fi_scale = 0.67f, .gamma = 0.43f, .max_md_jump = 0.65f,
        .max_cwnd = 0x001a2607, .qa_threshold = 6200, .target_delay = 20000,
    };
    updated.configured = true;
    assert(mrc_ctl_store_edit_cc(&store, id, replace_cc, &updated) == 0);
    assert_mode(&store, "cc-profiles.v1", 0640);
    assert(mrc_ctl_store_acquire_cc(&store, id, &cc, &record) == 0);
    assert((fcntl(cc.fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
    assert(mrc_ctl_store_acquire_cc(&store, id, &cc, &record) == -EBUSY);
    assert(mrc_ctl_store_acquire_cc(&store, id, &second_cc, &record) == 0);
    assert(record.id == id && record.state == MRC_CTL_STORE_ONLINE && record.configured);
    assert_cc_parameters(&record.config, &updated.config);

    mrc_ctl_store_close(&store);
    assert(mrc_ctl_store_open(&store, directory) == 0);
    assert(mrc_ctl_store_get_cc(&store, id, &record) == 0);
    assert_cc_parameters(&record.config, &updated.config);
    assert(mrc_ctl_store_edit_cc(&store, id, replace_cc, &updated) == 0);
    updated.config.target_delay++;
    assert(mrc_ctl_store_edit_cc(&store, id, replace_cc, &updated) == -EBUSY);
    updated.state = MRC_CTL_STORE_OFFLINE;
    assert(mrc_ctl_store_edit_cc(&store, id, replace_cc, &updated) == -EBUSY);
    mrc_ctl_store_release(&cc);
    assert(mrc_ctl_store_edit_cc(&store, id, delete_cc, NULL) == -EBUSY);
    mrc_ctl_store_release(&ev);
    assert(mrc_ctl_store_set_state(&store, id, MRC_CTL_STORE_OFFLINE) == 0);
    mrc_ctl_store_release(&second_cc);
    assert(mrc_ctl_store_edit_cc(&store, id, delete_cc, NULL) == 0);
    assert(mrc_ctl_store_get_cc(&store, id, &record) == -ENOENT);

    updated = (struct mrc_ctl_store_cc_profile){ .id = id, .state = MRC_CTL_STORE_ONLINE };
    assert(mrc_ctl_store_edit_cc(&store, id, replace_cc, &updated) == 0);
    assert(mrc_ctl_store_acquire_cc(&store, id, &cc, &record) == 0);
    assert(record.algorithm == MRC_CC_DISABLED && !record.configured);
    mrc_ctl_store_release(&cc);

    int fd = openat(store.fd, "cc-profiles.v1", O_RDWR);
    assert(fd >= 0);
    uint8_t corrupt = 10;
    assert(pwrite(fd, &corrupt, 1, 24) == 1);
    assert(close(fd) == 0);
    assert(mrc_ctl_store_get_cc(&store, id, &record) == -EBADMSG);
    assert(mrc_ctl_store_edit_cc(&store, id, delete_cc, NULL) == -EBADMSG);
    assert(mrc_ctl_store_get(&store, id, &ev_profile) == 0 && ev_profile.state == MRC_CTL_STORE_OFFLINE);
    assert(unlinkat(store.fd, "cc-profiles.v1", 0) == 0);
    remove_fixture(&store, directory);
}

int main(void)
{
    test_lifecycle();
    test_process_leases_and_modes();
    test_concurrent_writers();
    test_failed_update_and_corruption();
    test_typed_edits();
    test_legacy_shared_store();
    test_cc_profiles();
    puts("MRC controller profile store tests passed");
    return 0;
}
