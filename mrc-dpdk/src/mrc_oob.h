#ifndef MRC_OOB_H
#define MRC_OOB_H

#include "mrc_connect.h"

/* Exchange one lab QP offer in each direction over a numeric TCP endpoint. */
int mrc_oob_exchange(const char *address, uint16_t port, bool listen_mode,
                     unsigned int timeout_ms, const struct mrc_connect_offer *local,
                     struct mrc_connect_qp *qp);

#endif
