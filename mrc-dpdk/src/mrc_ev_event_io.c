#define _GNU_SOURCE
#include "mrc_ev_event_io.h"
#include "mrc_ev.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static const char socket_name[] = "ev-events.sock";
static const char lock_name[] = "ev-events.lock";
static const uint8_t event_magic[] = {'M', 'E', 'V', 1};

enum {
    EVENT_ID_OFFSET = 4,
    EVENT_PORT_OFFSET = 12,
    EVENT_STATE_OFFSET,
    EVENT_DROPPED_OFFSET,
    EVENT_RESERVED_OFFSET,
    EVENT_VALUE_OFFSET,
    EVENT_SIZE = EVENT_VALUE_OFFSET + 32,
};

static int event_address(int dirfd, struct sockaddr_un *address, socklen_t *length)
{
    *address = (struct sockaddr_un){ .sun_family = AF_UNIX };
    int size = snprintf(address->sun_path, sizeof(address->sun_path),
                        "/proc/self/fd/%d/%s", dirfd, socket_name);
    if (size < 0 || (size_t)size >= sizeof(address->sun_path))
        return -ENAMETOOLONG;
    *length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)size + 1);
    return 0;
}

static int listener_lock(int dirfd)
{
    int fd = openat(dirfd, lock_name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
        return -errno;
    struct stat status;
    int result = 0;
    if (fstat(fd, &status) < 0) {
        result = -errno;
    } else if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
        result = -EBADMSG;
    } else {
        do {
            result = flock(fd, LOCK_EX | LOCK_NB);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            result = errno == EWOULDBLOCK ? -EBUSY : -errno;
    }
    if (result) {
        close(fd);
        return result;
    }
    return fd;
}

static int remove_stale_socket(int dirfd)
{
    struct stat status;
    if (fstatat(dirfd, socket_name, &status, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : -errno;
    if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid())
        return -EEXIST;
    return unlinkat(dirfd, socket_name, 0) < 0 ? -errno : 0;
}

static int create_private_directory(int dirfd, char *name, size_t size)
{
    for (unsigned int attempt = 0; attempt < 64; attempt++) {
        snprintf(name, size, ".ev-events.%ld.%u", (long)getpid(), attempt);
        if (mkdirat(dirfd, name, 0700) == 0)
            return 0;
        if (errno != EEXIST)
            return -errno;
    }
    return -EEXIST;
}

static int publish_socket(int dirfd, int socket_fd, const struct stat *directory,
                           struct stat *socket_status)
{
    /* Set the socket mode in a private directory before exposing its name.
     * This also preserves group inheritance from a setgid profile store. */
    char private_name[64];
    int result = create_private_directory(dirfd, private_name, sizeof(private_name));
    if (result)
        return result;
    int private_fd = openat(dirfd, private_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (private_fd < 0) {
        result = -errno;
        goto remove_directory;
    }
    struct sockaddr_un address;
    socklen_t length;
    result = event_address(private_fd, &address, &length);
    if (result)
        goto close_directory;
    if (bind(socket_fd, (struct sockaddr *)&address, length) < 0) {
        result = -errno;
        goto close_directory;
    }

    const mode_t shared_permissions = S_ISGID | S_IRGRP | S_IXGRP;
    bool shared = (directory->st_mode & shared_permissions) == shared_permissions;
    if (fstatat(private_fd, socket_name, socket_status, AT_SYMLINK_NOFOLLOW) < 0)
        result = -errno;
    else if (shared && socket_status->st_gid != directory->st_gid)
        result = -EACCES;
    else if (fchmodat(private_fd, socket_name, shared ? 0660 : 0600, 0) < 0)
        result = -errno;
    else
        result = remove_stale_socket(dirfd);
    if (!result && renameat2(private_fd, socket_name, dirfd, socket_name, RENAME_NOREPLACE) < 0)
        result = -errno;
    if (result)
        unlinkat(private_fd, socket_name, 0);

close_directory:
    close(private_fd);
remove_directory:
    unlinkat(dirfd, private_name, AT_REMOVEDIR);
    return result;
}

int mrc_ev_event_open_sender(void)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return fd < 0 ? -errno : fd;
}

int mrc_ev_event_listen(int dirfd, struct mrc_ev_event_listener *listener)
{
    if (dirfd < 0 || !listener)
        return -EINVAL;
    if (listener->fd >= 0)
        return -EBUSY;
    int owned_dirfd = fcntl(dirfd, F_DUPFD_CLOEXEC, 0);
    if (owned_dirfd < 0)
        return -errno;

    struct stat directory;
    int result = 0;
    if (fstat(owned_dirfd, &directory) < 0)
        result = -errno;
    else if (!S_ISDIR(directory.st_mode))
        result = -ENOTDIR;
    if (result)
        goto close_directory;

    int lock_fd = listener_lock(owned_dirfd);
    if (lock_fd < 0) {
        result = lock_fd;
        goto close_directory;
    }
    int fd = mrc_ev_event_open_sender();
    if (fd < 0) {
        result = fd;
        goto close_lock;
    }
    struct stat socket_status;
    result = publish_socket(owned_dirfd, fd, &directory, &socket_status);
    if (result) {
        close(fd);
        goto close_lock;
    }
    *listener = (struct mrc_ev_event_listener){
        .fd = fd, .lock_fd = lock_fd, .dir_fd = owned_dirfd,
        .socket_device = socket_status.st_dev, .socket_inode = socket_status.st_ino,
    };
    return 0;

close_lock:
    close(lock_fd);
close_directory:
    close(owned_dirfd);
    return result;
}

void mrc_ev_event_close(struct mrc_ev_event_listener *listener)
{
    if (!listener || listener->fd < 0)
        return;
    struct stat status;
    if (fstatat(listener->dir_fd, socket_name, &status, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(status.st_mode) && status.st_dev == listener->socket_device &&
        status.st_ino == listener->socket_inode)
        unlinkat(listener->dir_fd, socket_name, 0);
    close(listener->fd);
    close(listener->dir_fd);
    close(listener->lock_fd);
    *listener = (struct mrc_ev_event_listener)MRC_EV_EVENT_LISTENER_INITIALIZER;
}

static bool valid_record(const struct mrc_ev_event_record *record)
{
    return record->profile_id && record->port && record->port <= MRC_EV_MAX_PATHS &&
           (record->state == MRC_EV_EVENT_GOOD || record->state == MRC_EV_EVENT_ASSUMED_BAD) &&
           record->dropped <= 1;
}

int mrc_ev_event_send(int fd, int dirfd, const struct mrc_ev_event_record *record)
{
    if (fd < 0 || dirfd < 0 || !record || !valid_record(record))
        return -EINVAL;
    struct sockaddr_un address;
    socklen_t length;
    int result = event_address(dirfd, &address, &length);
    if (result)
        return result;

    uint8_t data[EVENT_SIZE] = {0};
    memcpy(data, event_magic, sizeof(event_magic));
    for (unsigned int i = 0; i < 8; i++)
        data[EVENT_ID_OFFSET + i] = (uint8_t)(record->profile_id >> (56 - 8 * i));
    data[EVENT_PORT_OFFSET] = record->port;
    data[EVENT_STATE_OFFSET] = record->state;
    data[EVENT_DROPPED_OFFSET] = record->dropped;
    memcpy(data + EVENT_VALUE_OFFSET, record->value, sizeof(record->value));
    ssize_t sent = sendto(fd, data, sizeof(data), MSG_DONTWAIT | MSG_NOSIGNAL,
                          (struct sockaddr *)&address, length);
    if (sent < 0)
        return -errno;
    return sent == sizeof(data) ? 0 : -EIO;
}

int mrc_ev_event_receive(int fd, struct mrc_ev_event_record *record)
{
    if (fd < 0 || !record)
        return -EINVAL;
    uint8_t data[EVENT_SIZE];
    ssize_t received = recv(fd, data, sizeof(data), MSG_DONTWAIT | MSG_TRUNC);
    if (received < 0)
        return -errno;
    if (received != sizeof(data) || memcmp(data, event_magic, sizeof(event_magic)) ||
        data[EVENT_RESERVED_OFFSET])
        return -EBADMSG;

    struct mrc_ev_event_record event = {
        .port = data[EVENT_PORT_OFFSET], .state = data[EVENT_STATE_OFFSET],
        .dropped = data[EVENT_DROPPED_OFFSET],
    };
    for (unsigned int i = 0; i < 8; i++)
        event.profile_id = (event.profile_id << 8) | data[EVENT_ID_OFFSET + i];
    memcpy(event.value, data + EVENT_VALUE_OFFSET, sizeof(event.value));
    if (!valid_record(&event))
        return -EBADMSG;
    *record = event;
    return 0;
}
