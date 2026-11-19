#define _POSIX_C_SOURCE 200809L
#include "mrc_ctl_store.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
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
            _exit(mrc_ctl_store_update_denied(&store, 5, UINT32_C(1) << i, 0) != 0);
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

int main(void)
{
    test_lifecycle();
    test_process_leases_and_modes();
    test_concurrent_writers();
    test_failed_update_and_corruption();
    puts("MRC controller profile store tests passed");
    return 0;
}
