#ifndef MRC_VERBS_ASYNC_H
#define MRC_VERBS_ASYNC_H

#include <infiniband/verbs.h>
#include <stdint.h>

/* Allocate a nonblocking, close-on-exec async event FD on the verbs context.
 * The caller owns the new FD. Errors are returned as negative errno values. */
int mrc_verbs_async_open(const struct ibv_context *verbs);

/* Read one raw uverbs event. Returns zero or a negative errno. */
int mrc_verbs_async_read(int fd, enum ibv_event_type *type, uint64_t *element);

#endif
