/* Reuse the public live application and observe its ABI-to-endpoint boundary.
 * Link wrappers verify configuration without replacing any production behavior. */
#define main public_application_main
#include "mrc_abi_application.c"
#undef main
#include "mrc_endpoint.h"

enum { WITNESS_QPS = 32 };
static struct { uint32_t qpn; unsigned int caps; } observed_qps[WITNESS_QPS];
static unsigned int observed_count;
static unsigned int requester_modes;
static unsigned int responder_count;

int __real_mrc_modify_qp(struct mrc_qp *, struct ibv_qp_attr *, int, struct mrc_qp_attr *, int);
int __wrap_mrc_modify_qp(struct mrc_qp *qp, struct ibv_qp_attr *verbs, int verbs_mask,
                          struct mrc_qp_attr *attrs, int attrs_mask)
{
    int result = __real_mrc_modify_qp(qp, verbs, verbs_mask, attrs, attrs_mask);
    if (result)
        return result;
    uint32_t qpn;
    assert(mrc_get_qpn(qp, &qpn) == 0);
    unsigned int index = 0;
    while (index < observed_count && observed_qps[index].qpn != qpn)
        index++;
    if (index == observed_count) {
        assert(index < WITNESS_QPS);
        observed_qps[index].qpn = qpn;
        observed_count++;
    }
    if (attrs_mask & MRC_QP_CAP)
        observed_qps[index].caps = attrs->cap;
    return 0;
}

int __real_mrc_requester_create(const struct mrc_endpoint_config *, struct mrc_provider_qp *,
                                 struct mrc_endpoint_counters *, struct mrc_requester **);
int __wrap_mrc_requester_create(const struct mrc_endpoint_config *config, struct mrc_provider_qp *qp,
                                 struct mrc_endpoint_counters *counts, struct mrc_requester **requester)
{
    unsigned int index = 0;
    while (index < observed_count && observed_qps[index].qpn != config->requestor_qpn)
        index++;
    assert(index < observed_count);
    bool unsupported = (observed_qps[index].caps & MRC_QP_CAP_SVC_TIME_DEST_UNSUPPORTED) != 0;
    assert(config->sender && config->report_service_time);
    assert(config->peer_service_time == !unsupported);
    requester_modes |= 1u << unsupported;
    return __real_mrc_requester_create(config, qp, counts, requester);
}

int __real_mrc_responder_create(const struct mrc_endpoint_config *, struct mrc_provider_qp *,
                                 struct mrc_endpoint_counters *, struct mrc_responder **);
int __wrap_mrc_responder_create(const struct mrc_endpoint_config *config, struct mrc_provider_qp *qp,
                                 struct mrc_endpoint_counters *counts, struct mrc_responder **responder)
{
    assert(!config->sender && config->report_service_time && !config->peer_service_time);
    responder_count++;
    return __real_mrc_responder_create(config, qp, counts, responder);
}

int main(int argc, char **argv)
{
    int result = public_application_main(argc, argv);
    assert(result == 0 && requester_modes == 3 && responder_count >= 2);
    return result;
}
