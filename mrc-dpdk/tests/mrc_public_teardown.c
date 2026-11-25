/* Observe the real public API release boundary without replacing behavior. */
#define main public_application_main
#include "mrc_abi_application.c"
#undef main
#include "mrc_abi.h"

static unsigned int released_routes;
static unsigned int retained_sends;

void __real_mrc_abi_release_route(struct mrc_qp *qp);
void __wrap_mrc_abi_release_route(struct mrc_qp *qp)
{
    if (qp->route) {
        struct mrc_provider_qp_attr attr;
        assert(mrc_provider_qp_query(qp->provider, &attr) == 0);
        assert(attr.state == MRC_PROVIDER_QP_ERROR);
        released_routes++;
        retained_sends += attr.send_pending;
    }
    __real_mrc_abi_release_route(qp);
}

int main(int argc, char **argv)
{
    bool requester = false, explicit_error = false;
    for (int i = 1; i < argc; i++) {
        requester |= !strcmp(argv[i], "tx");
        explicit_error |= !strcmp(argv[i], "error_pending") || !strcmp(argv[i], "error_pending_overflow");
    }
    int result = public_application_main(argc, argv);
    assert(result == 0 && released_routes == 1);
    assert(retained_sends == (unsigned int)(requester && !explicit_error));
    return result;
}
