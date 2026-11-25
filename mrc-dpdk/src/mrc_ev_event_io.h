#ifndef MRC_EV_EVENT_IO_H
#define MRC_EV_EVENT_IO_H

#include <stdint.h>
#include <sys/types.h>

enum mrc_ev_event_state {
    MRC_EV_EVENT_GOOD = 1,
    MRC_EV_EVENT_ASSUMED_BAD = 2,
};

struct mrc_ev_event_record {
    uint64_t profile_id;
    uint8_t value[32];
    uint8_t port;
    uint8_t state;
    uint8_t dropped;
};

struct mrc_ev_event_listener {
    int fd;
    int lock_fd;
    int dir_fd;
    dev_t socket_device;
    ino_t socket_inode;
};

#define MRC_EV_EVENT_LISTENER_INITIALIZER { .fd = -1, .lock_fd = -1, .dir_fd = -1 }

/* The listener retains the directory and exposes a nonblocking FD for poll.
 * Close the listener with mrc_ev_event_close rather than closing its FD. */
int mrc_ev_event_listen(int dirfd, struct mrc_ev_event_listener *listener);
void mrc_ev_event_close(struct mrc_ev_event_listener *listener);

/* The caller closes its send socket. All operations return negative errno
 * on failure, and sending and receiving never wait for queue space or data. */
int mrc_ev_event_open_sender(void);
int mrc_ev_event_send(int fd, int dirfd, const struct mrc_ev_event_record *record);
int mrc_ev_event_receive(int fd, struct mrc_ev_event_record *record);

#endif
