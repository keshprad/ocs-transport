#include "mrc_dpdk.h"

#include <mrc_ctl.h>

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    PROFILE_ID = 7,
    CC_PROFILE_ID = 8,
    DISABLED_CC_PROFILE_ID = 9,
    QA_CC_PROFILE_ID = 10,
    RAILS = 2,
    FIRST_EV = 0xc101,
    SECOND_EV = 0xc102,
};

static const char nscc_algorithm[] = "uet-1.0-nscc";
static const struct mrc_ctl_cc_nscc_cfg nscc_config = {
    .disable_qa = 1,
    .adjust_bytes_threshold = 32768,
    .adjust_period_threshold = 10000,
    .base_rtt = 18000,
    .eta = 0.375f,
    .fi = 0.25f,
    .fi_scale = 0.5f,
    .gamma = 0.75f,
    .max_md_jump = 0.5f,
    .max_cwnd = 1112,
    .target_delay = 9000,
};

static const struct mrc_ctl_cc_nscc_cfg qa_config = {
    .disable_qa = 0,
    .adjust_bytes_threshold = 32768,
    .adjust_period_threshold = 50000,
    .base_rtt = 50000,
    .eta = 0,
    .fi = 0,
    .fi_scale = 0,
    .gamma = 0.5f,
    .max_md_jump = 0.5f,
    .max_cwnd = 16384,
    .qa_gate = 1,
    .qa_threshold = 3000000,
    .target_delay = 500000,
};

static struct ibv_context *open_supported_device(void)
{
    struct ibv_device **devices = ibv_get_device_list(NULL);
    if (!devices)
        return NULL;
    struct ibv_context *verbs = NULL;
    for (struct ibv_device **device = devices; *device; device++) {
        verbs = ibv_open_device(*device);
        if (!verbs)
            continue;
        struct mrc_device_attr attr;
        int supported;
        if (mrc_query_device(verbs, &attr, &supported) == 0 && supported)
            break;
        assert(ibv_close_device(verbs) == 0);
        verbs = NULL;
    }
    ibv_free_device_list(devices);
    return verbs;
}

static struct mrc_context *open_store(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *context = mrc_create_context(verbs, NULL);
    assert(context);
    assert(mrc_dpdk_set_ev_store(context, directory) == 0);
    return context;
}

static void remove_test_store(const char *directory)
{
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            assert(unlinkat(dirfd(dir), entry->d_name, 0) == 0);
    }
    assert(closedir(dir) == 0);
    assert(rmdir(directory) == 0);
}

static void test_controller_discovery(struct ibv_context *verbs, struct mrc_context *context)
{
    uint32_t current = 0, oldest = 0;
    mrc_ctl_query_version(&current, &oldest, NULL, NULL);
    assert(current >= MRC_CTL_API_VER(1, 0, 0) && oldest <= current);

    struct mrc_ctl_device_attr legacy = {0}, extended = {0};
    int supported = 0;
    assert(mrc_ctl_query_device(verbs, &legacy) == 0);
    assert(mrc_ctl_query_device_ex(verbs, context, &extended, &supported) == 0 && supported);
    assert(extended.mrc_ctl_protocol_version & MRC_CTL_PROTOCOL_VERSION_1);
    assert(legacy.mrc_ctl_protocol_version == extended.mrc_ctl_protocol_version);
    assert(legacy.ev.ev_max_count_profile == extended.ev.ev_max_count_profile);
    assert(extended.ev.ev_max_count_profile >= RAILS);
    assert(extended.ev.ev_mode_mask & MRC_CTL_EV_MODE_EXP);
    assert(extended.ev.ev_fmt_mode_mask & MRC_CTL_EV_FMT_MODE_UDP);
    assert(extended.ev.ev_fmt_mode_mask & MRC_CTL_EV_FMT_MODE_STEV);
    assert(legacy.cc.cc_max_profiles == 32 && extended.cc.cc_max_profiles == 32);
    assert(legacy.cc.algorithms && legacy.cc.algorithms[0] &&
           strcmp(legacy.cc.algorithms[0], nscc_algorithm) == 0 && !legacy.cc.algorithms[1]);
    assert(extended.cc.algorithms && extended.cc.algorithms[0] &&
           strcmp(extended.cc.algorithms[0], nscc_algorithm) == 0 && !extended.cc.algorithms[1]);
    assert(!((legacy.opt_attr | extended.opt_attr) & MRC_CTL_OPT_CAP_CC_PROFILE_MODIFY_ONLINE));

    static const struct {
        uint64_t id;
        enum mrc_ctl_ev_fmt_mode mode;
        unsigned int width;
    } formats[] = {
        { MRC_DPDK_EV_FORMAT_UDP, MRC_CTL_EV_FMT_MODE_UDP, 16 },
        { MRC_DPDK_EV_FORMAT_STEV, MRC_CTL_EV_FMT_MODE_STEV, 32 },
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct mrc_ctl_ev_fmt_field fields[2] = {0};
        struct mrc_ctl_ev_fmt_profile_attr attr = {
            .ev_fmt_op = {
                .op = MRC_CTL_EV_FMT_OP_QUERY_FIELDS, .fmt_fields = fields, .fmt_field_count = 2,
            },
        };
        assert(mrc_ctl_query_ev_fmt_profile(context, formats[i].id, &attr,
                                          MRC_CTL_EV_FMT_PROFILE_MODE | MRC_CTL_EV_FMT_PROFILE_OP) == 0);
        assert(attr.ev_fmt_mode == formats[i].mode);
        assert(attr.ev_fmt_op.fmt_field_count > 0 && attr.ev_fmt_op.fmt_field_count <= 2);
        unsigned int width = 0;
        for (int field = 0; field < attr.ev_fmt_op.fmt_field_count; field++)
            width += fields[field].width;
        assert(width == formats[i].width);
    }
}

static int configure_explicit(struct mrc_context *context)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE,
        .ev_mode = MRC_CTL_EV_MODE_EXP,
        .ev_fmt_profile_id = MRC_DPDK_EV_FORMAT_UDP,
        .ev_count = RAILS,
    };
    return mrc_ctl_modify_ev_profile(context, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_STATE |
                                     MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_FMT_ID |
                                     MRC_CTL_EV_PROFILE_COUNT);
}

static int set_profile_state(struct mrc_context *context, enum mrc_ctl_profile_state state)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = state,
        .ev_min_active = 1,
    };
    int mask = MRC_CTL_EV_PROFILE_STATE;
    if (state == MRC_CTL_PROFILE_ONLINE)
        mask |= MRC_CTL_EV_PROFILE_MIN_ACTIVE;
    return mrc_ctl_modify_ev_profile(context, PROFILE_ID, &attr, mask);
}

static enum mrc_ctl_profile_state profile_state(struct mrc_context *context)
{
    struct mrc_ctl_ev_profile_attr attr = {0};
    assert(mrc_ctl_query_ev_profile(context, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_CUR_STATE) == 0);
    return attr.cur_profile_state;
}

static struct mrc_ctl_ev ev(uint8_t rail, uint32_t value)
{
    struct mrc_ctl_ev entry = { .port = rail };
    uint32_t network = htonl(value);
    memcpy(entry.val, &network, sizeof(network));
    return entry;
}

static uint32_t ev_value(const struct mrc_ctl_ev *entry)
{
    uint32_t network;
    memcpy(&network, entry->val, sizeof(network));
    return ntohl(network);
}

static int replace_evs(struct mrc_context *context, struct mrc_ctl_ev_op_entry *entries, int count)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_REPLACE_EV, .entries = entries, .entry_count = count },
    };
    return mrc_ctl_modify_ev_profile(context, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EV_OP);
}

static void check_evs(struct mrc_context *context, uint32_t first, uint32_t second)
{
    struct mrc_ctl_ev_op_entry entries[RAILS] = {0};
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_QUERY_EV_ARRAY, .entries = entries, .entry_count = 1 },
    };
    const int mask = MRC_CTL_EV_PROFILE_EV_OP | MRC_CTL_EV_PROFILE_COUNT |
                     MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_FMT_ID;
    assert(mrc_ctl_query_ev_profile(context, PROFILE_ID, &attr, mask) == -1 && errno == E2BIG);
    assert(attr.ev_op.entry_count == RAILS);
    assert(mrc_ctl_query_ev_profile(context, PROFILE_ID, &attr, mask) == 0);
    assert(attr.ev_op.entry_count == RAILS && attr.ev_count == RAILS);
    assert(attr.ev_mode == MRC_CTL_EV_MODE_EXP && attr.ev_fmt_profile_id == MRC_DPDK_EV_FORMAT_UDP);
    assert(entries[0].ev.port == 1 && ev_value(&entries[0].ev) == first);
    assert(entries[1].ev.port == 2 && ev_value(&entries[1].ev) == second);
}

static void populate_evs(struct mrc_context *controller)
{
    struct mrc_ctl_ev_op_entry entries[RAILS] = {
        { .ev = ev(1, 0), .ev_new = ev(1, FIRST_EV), .state = MRC_CTL_EV_GOOD },
        { .ev = ev(2, FIRST_EV), .ev_new = ev(2, SECOND_EV), .state = MRC_CTL_EV_GOOD },
    };
    assert(replace_evs(controller, entries, RAILS) == -1 && errno == ENOENT);
    check_evs(controller, FIRST_EV, 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_ONLINE) == -1 && errno == EINVAL);
    assert(profile_state(controller) == MRC_CTL_PROFILE_OFFLINE);

    entries[1].ev = ev(2, 0);
    assert(replace_evs(controller, &entries[1], 1) == 0);
    check_evs(controller, FIRST_EV, SECOND_EV);
}

static void set_ev_state(struct mrc_context *context, enum mrc_ctl_ev_state state)
{
    struct mrc_ctl_ev_op_entry entry = { .ev = ev(2, SECOND_EV), .state = state };
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_MODIFY_EV_STATE, .entries = &entry, .entry_count = 1 },
    };
    assert(mrc_ctl_modify_ev_profile(context, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EV_OP) == 0);
}

static enum mrc_ctl_ev_state get_ev_state(struct mrc_context *context)
{
    struct mrc_ctl_ev_op_entry entry = { .ev = ev(2, SECOND_EV) };
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_QUERY_EV_STATE, .entries = &entry, .entry_count = 1 },
    };
    assert(mrc_ctl_query_ev_profile(context, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EV_OP) == 0);
    return entry.state;
}

static void select_profile(struct mrc_qp *qp)
{
    struct ibv_qp_attr verbs = { .qp_state = IBV_QPS_INIT };
    struct mrc_qp_attr attrs = { .profile.ev_profile_id = PROFILE_ID };
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE, &attrs, MRC_QP_EV_PROFILE_ID) == 0);

    attrs = (struct mrc_qp_attr){0};
    assert(mrc_query_qp(qp, &verbs, IBV_QP_STATE, &attrs, MRC_QP_EV_PROFILE_ID, NULL) == 0);
    assert(verbs.qp_state == IBV_QPS_INIT && attrs.profile.ev_profile_id == PROFILE_ID);
}

static void test_auto_profile(struct mrc_context *controller)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE, .ev_mode = MRC_CTL_EV_MODE_AUTO, .ev_count = RAILS,
    };
    assert(mrc_ctl_modify_ev_profile(controller, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_STATE |
                                     MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_COUNT) == 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_ONLINE) == 0);
    struct mrc_ctl_ev_op_entry entries[RAILS];
    attr = (struct mrc_ctl_ev_profile_attr){
        .ev_op = { .op = MRC_CTL_EV_OP_QUERY_EV_ARRAY, .entries = entries, .entry_count = RAILS },
    };
    assert(mrc_ctl_query_ev_profile(controller, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EV_OP |
                                    MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_FMT_ID) == 0);
    assert(attr.ev_mode == MRC_CTL_EV_MODE_AUTO && attr.ev_fmt_profile_id == 0);
    assert(attr.ev_op.entry_count == RAILS);
    for (unsigned int i = 0; i < RAILS; i++) {
        assert(entries[i].ev.port == i + 1 && ev_value(&entries[i].ev));
        assert(entries[i].state == MRC_CTL_EV_GOOD);
    }
    assert(set_profile_state(controller, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_INIT) == 0);
}

static void test_profile_lifecycle(struct ibv_context *verbs, struct mrc_context *controller,
                                   const char *directory)
{
    test_controller_discovery(verbs, controller);
    test_auto_profile(controller);
    assert(profile_state(controller) == MRC_CTL_PROFILE_INIT);
    assert(configure_explicit(controller) == 0);
    assert(profile_state(controller) == MRC_CTL_PROFILE_OFFLINE);
    check_evs(controller, 0, 0);
    populate_evs(controller);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_ONLINE) == 0);

    struct mrc_context *client = open_store(verbs, directory);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    struct mrc_cq *cq = mrc_create_cq(client, 4, NULL, NULL, 0);
    assert(pd && cq);
    struct mrc_qp_init_attr init = {
        .send_cq = cq, .pd = pd, .cap = { .max_send_wr = 1, .max_send_sge = 1 },
    };
    struct mrc_qp *qp = mrc_create_qp(client, &init);
    assert(qp);

    select_profile(qp);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    set_ev_state(controller, MRC_CTL_EV_DENIED);
    assert(get_ev_state(client) == MRC_CTL_EV_DENIED);

    assert(mrc_destroy_context(controller) == 0);
    controller = open_store(verbs, directory);
    assert(profile_state(controller) == MRC_CTL_PROFILE_ONLINE);
    check_evs(controller, FIRST_EV, SECOND_EV);
    assert(get_ev_state(controller) == MRC_CTL_EV_DENIED);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    set_ev_state(controller, MRC_CTL_EV_GOOD);
    assert(get_ev_state(client) == MRC_CTL_EV_GOOD);

    struct ibv_qp_attr reset = { .qp_state = IBV_QPS_RESET };
    assert(mrc_modify_qp(qp, &reset, IBV_QP_STATE, NULL, 0) == 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_ONLINE) == 0);

    select_profile(qp);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    assert(mrc_destroy_qp(qp) == 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_INIT) == 0);
    assert(profile_state(controller) == MRC_CTL_PROFILE_INIT);

    assert(mrc_destroy_cq(cq) == 0);
    assert(ibv_dealloc_pd(pd) == 0);
    assert(mrc_destroy_context(client) == 0);
    assert(mrc_destroy_context(controller) == 0);
}

static int configure_cc_algorithm(struct mrc_context *controller, uint64_t id,
                                  const char *algorithm)
{
    struct mrc_ctl_cc_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE, .algorithm = algorithm,
    };
    return mrc_ctl_modify_cc_profile(controller, id, &attr,
                                     MRC_CTL_CC_PROFILE_STATE | MRC_CTL_CC_PROFILE_ALGORITHM);
}

static int activate_cc(struct mrc_context *controller, uint64_t id,
                       const struct mrc_ctl_cc_nscc_cfg *config)
{
    struct mrc_ctl_cc_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_ONLINE, .cc_config = config,
    };
    return mrc_ctl_modify_cc_profile(controller, id, &attr,
                                     MRC_CTL_CC_PROFILE_STATE | MRC_CTL_CC_PROFILE_CONFIG);
}

static int set_cc_state(struct mrc_context *controller, uint64_t id, enum mrc_ctl_profile_state state)
{
    struct mrc_ctl_cc_profile_attr attr = { .profile_state = state };
    return mrc_ctl_modify_cc_profile(controller, id, &attr, MRC_CTL_CC_PROFILE_STATE);
}

static struct mrc_ctl_cc_profile_attr query_cc(struct mrc_context *controller, uint64_t id)
{
    struct mrc_ctl_cc_profile_attr attr = {0};
    assert(mrc_ctl_query_cc_profile(controller, id, &attr,
                                   MRC_CTL_CC_PROFILE_CUR_STATE | MRC_CTL_CC_PROFILE_ALGORITHM |
                                   MRC_CTL_CC_PROFILE_CONFIG) == 0);
    return attr;
}

static void check_cc_config(struct mrc_context *controller, uint64_t id,
                            const struct mrc_ctl_cc_nscc_cfg *expected)
{
    struct mrc_ctl_cc_profile_attr observed = query_cc(controller, id);
    assert(observed.cur_profile_state == MRC_CTL_PROFILE_ONLINE);
    assert(observed.algorithm && strcmp(observed.algorithm, nscc_algorithm) == 0);
    const struct mrc_ctl_cc_nscc_cfg *config = observed.cc_config;
    assert(config);
    assert(config->disable_qa == expected->disable_qa);
    assert(config->adjust_bytes_threshold == expected->adjust_bytes_threshold);
    assert(config->adjust_period_threshold == expected->adjust_period_threshold);
    assert(config->base_rtt == expected->base_rtt);
    assert(config->eta == expected->eta && config->fi == expected->fi &&
           config->fi_scale == expected->fi_scale && config->gamma == expected->gamma &&
           config->max_md_jump == expected->max_md_jump);
    assert(config->max_cwnd == expected->max_cwnd && config->target_delay == expected->target_delay);
    assert(config->qa_gate == expected->qa_gate && config->qa_threshold == expected->qa_threshold);
}

static void select_cc_profile(struct mrc_qp *qp, uint64_t id)
{
    struct ibv_qp_attr verbs = { .qp_state = IBV_QPS_INIT };
    struct mrc_qp_attr attrs = { .profile.cc_profile_id = id };
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE, &attrs, MRC_QP_CC_PROFILE_ID) == 0);

    attrs = (struct mrc_qp_attr){0};
    assert(mrc_query_qp(qp, &verbs, IBV_QP_STATE, &attrs, MRC_QP_CC_PROFILE_ID, NULL) == 0);
    assert(verbs.qp_state == IBV_QPS_INIT && attrs.profile.cc_profile_id == id);
}

static void test_cc_lifecycle(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *controller = open_store(verbs, directory);
    struct mrc_ctl_cc_profile_attr observed = query_cc(controller, CC_PROFILE_ID);
    assert(observed.cur_profile_state == MRC_CTL_PROFILE_INIT && !observed.algorithm && !observed.cc_config);
    assert(set_cc_state(controller, CC_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EINVAL);
    assert(configure_cc_algorithm(controller, CC_PROFILE_ID, "unknown-cc") == -1 && errno == EINVAL);
    assert(configure_cc_algorithm(controller, CC_PROFILE_ID, nscc_algorithm) == 0);
    observed = query_cc(controller, CC_PROFILE_ID);
    assert(observed.cur_profile_state == MRC_CTL_PROFILE_OFFLINE && !observed.cc_config);
    assert(observed.algorithm && strcmp(observed.algorithm, nscc_algorithm) == 0);

    assert(set_cc_state(controller, CC_PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == -1 && errno == EINVAL);
    struct mrc_ctl_cc_nscc_cfg invalid = nscc_config;
    invalid.max_cwnd = 1111;
    assert(activate_cc(controller, CC_PROFILE_ID, &invalid) == -1 && errno == EINVAL);
    assert(query_cc(controller, CC_PROFILE_ID).cur_profile_state == MRC_CTL_PROFILE_OFFLINE);
    assert(activate_cc(controller, CC_PROFILE_ID, &nscc_config) == 0);
    struct mrc_ctl_cc_profile_attr replacement = { .cc_config = &nscc_config };
    assert(mrc_ctl_modify_cc_profile(controller, CC_PROFILE_ID, &replacement,
                                     MRC_CTL_CC_PROFILE_CONFIG) == -1 && errno == EBUSY);
    check_cc_config(controller, CC_PROFILE_ID, &nscc_config);

    struct mrc_context *client = open_store(verbs, directory);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    struct mrc_cq *cq = mrc_create_cq(client, 4, NULL, NULL, 0);
    assert(pd && cq);
    struct mrc_qp_init_attr init = {
        .send_cq = cq, .pd = pd, .cap = { .max_send_wr = 1, .max_send_sge = 1 },
    };
    struct mrc_qp *qp = mrc_create_qp(client, &init);
    assert(qp);
    select_cc_profile(qp, CC_PROFILE_ID);
    assert(mrc_destroy_context(controller) == 0);
    controller = open_store(verbs, directory);
    check_cc_config(controller, CC_PROFILE_ID, &nscc_config);
    assert(set_cc_state(controller, CC_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);

    struct ibv_qp_attr reset = { .qp_state = IBV_QPS_RESET };
    assert(mrc_modify_qp(qp, &reset, IBV_QP_STATE, NULL, 0) == 0);
    assert(set_cc_state(controller, CC_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(activate_cc(controller, CC_PROFILE_ID, &nscc_config) == 0);
    select_cc_profile(qp, CC_PROFILE_ID);
    assert(set_cc_state(controller, CC_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    assert(mrc_destroy_qp(qp) == 0);
    assert(set_cc_state(controller, CC_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_cc_state(controller, CC_PROFILE_ID, MRC_CTL_PROFILE_INIT) == 0);
    observed = query_cc(controller, CC_PROFILE_ID);
    assert(observed.cur_profile_state == MRC_CTL_PROFILE_INIT && !observed.algorithm && !observed.cc_config);

    assert(configure_cc_algorithm(controller, DISABLED_CC_PROFILE_ID, NULL) == 0);
    assert(activate_cc(controller, DISABLED_CC_PROFILE_ID, NULL) == 0);
    observed = query_cc(controller, DISABLED_CC_PROFILE_ID);
    assert(observed.cur_profile_state == MRC_CTL_PROFILE_ONLINE && !observed.algorithm && !observed.cc_config);

    assert(mrc_destroy_cq(cq) == 0);
    assert(ibv_dealloc_pd(pd) == 0);
    assert(mrc_destroy_context(client) == 0);
    assert(mrc_destroy_context(controller) == 0);
}

static void test_qa_lifecycle(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *controller = open_store(verbs, directory);
    assert(configure_cc_algorithm(controller, QA_CC_PROFILE_ID, nscc_algorithm) == 0);

    struct mrc_ctl_cc_nscc_cfg boundary = qa_config;
    boundary.qa_gate = 32;
    assert(activate_cc(controller, QA_CC_PROFILE_ID, &boundary) == -1 && errno == EINVAL);
    struct mrc_ctl_cc_profile_attr observed = query_cc(controller, QA_CC_PROFILE_ID);
    assert(observed.cur_profile_state == MRC_CTL_PROFILE_OFFLINE && !observed.cc_config);

    boundary.disable_qa = 1;
    assert(activate_cc(controller, QA_CC_PROFILE_ID, &boundary) == 0);
    check_cc_config(controller, QA_CC_PROFILE_ID, &boundary);
    assert(set_cc_state(controller, QA_CC_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);

    boundary.disable_qa = 0;
    boundary.qa_gate = 31;
    boundary.qa_threshold = 0;
    assert(activate_cc(controller, QA_CC_PROFILE_ID, &boundary) == 0);
    check_cc_config(controller, QA_CC_PROFILE_ID, &boundary);
    assert(set_cc_state(controller, QA_CC_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);

    assert(activate_cc(controller, QA_CC_PROFILE_ID, &qa_config) == 0);
    assert(mrc_destroy_context(controller) == 0);
    controller = open_store(verbs, directory);
    check_cc_config(controller, QA_CC_PROFILE_ID, &qa_config);
    assert(mrc_destroy_context(controller) == 0);
}

static void seed_denied_profile(struct mrc_context *controller)
{
    assert(configure_explicit(controller) == 0);
    struct mrc_ctl_ev_op_entry entries[RAILS] = {
        { .ev = ev(1, 0), .ev_new = ev(1, FIRST_EV), .state = MRC_CTL_EV_DENIED },
        { .ev = ev(2, 0), .ev_new = ev(2, SECOND_EV), .state = MRC_CTL_EV_DENIED },
    };
    assert(replace_evs(controller, entries, RAILS) == 0);
    assert(set_profile_state(controller, MRC_CTL_PROFILE_ONLINE) == 0);
    printf("%u\n", (unsigned int)PROFILE_ID);
}

int main(int argc, char **argv)
{
    bool expect_no_cap = argc == 2 && strcmp(argv[1], "--expect-no-cap") == 0;
    bool seed = argc == 3 && strcmp(argv[1], "--seed-denied") == 0;
    bool unblock = argc == 3 && strcmp(argv[1], "--unblock") == 0;
    bool seed_cc = argc == 3 && strcmp(argv[1], "--seed-cc") == 0;
    bool seed_qa = argc == 3 && strcmp(argv[1], "--seed-qa") == 0;
    if (argc != 1 && !expect_no_cap && !seed && !unblock && !seed_cc && !seed_qa) {
        fprintf(stderr, "Usage: %s [--expect-no-cap | --seed-denied DIR | --unblock DIR | "
                        "--seed-cc DIR | --seed-qa DIR]\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct ibv_context *verbs = open_supported_device();
    if (!verbs) {
        fputs("The MRC controller ABI test needs an accessible Ethernet verbs device.\n", stderr);
        return EXIT_FAILURE;
    }
    if (seed || unblock || seed_cc || seed_qa) {
        struct mrc_context *controller = open_store(verbs, argv[2]);
        if (seed) {
            seed_denied_profile(controller);
        } else if (unblock) {
            assert(set_profile_state(controller, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
            set_ev_state(controller, MRC_CTL_EV_GOOD);
        } else {
            unsigned int id = seed_qa ? QA_CC_PROFILE_ID : CC_PROFILE_ID;
            const struct mrc_ctl_cc_nscc_cfg *config = seed_qa ? &qa_config : &nscc_config;
            assert(configure_cc_algorithm(controller, id, nscc_algorithm) == 0);
            assert(activate_cc(controller, id, config) == 0);
            printf("%u\n", id);
        }
        assert(mrc_destroy_context(controller) == 0);
        assert(ibv_close_device(verbs) == 0);
        return EXIT_SUCCESS;
    }

    char directory[] = "/tmp/mrc-ctl-abi.XXXXXX";
    assert(mkdtemp(directory));
    struct mrc_context *controller = open_store(verbs, directory);
    if (expect_no_cap) {
        assert(configure_explicit(controller) == -1 && errno == EPERM);
        assert(profile_state(controller) == MRC_CTL_PROFILE_INIT);
        struct mrc_ctl_cc_profile_attr cc = {0};
        assert(mrc_ctl_query_cc_profile(controller, CC_PROFILE_ID, &cc,
                                        MRC_CTL_CC_PROFILE_CUR_STATE) == -1 && errno == EPERM);
        assert(configure_cc_algorithm(controller, CC_PROFILE_ID, nscc_algorithm) == -1 && errno == EPERM);
        assert(mrc_destroy_context(controller) == 0);
        puts("Controller operations without CAP_NET_ADMIN were rejected");
    } else {
        test_profile_lifecycle(verbs, controller, directory);
        puts("OCP controller EV profiles, live EV state, persistence, and QP leases passed");
        test_cc_lifecycle(verbs, directory);
        puts("OCP controller CC profiles, persistence, and QP leases passed");
        test_qa_lifecycle(verbs, directory);
        puts("OCP controller NSCC Quick Adapt configuration and persistence passed");
    }

    remove_test_store(directory);
    assert(ibv_close_device(verbs) == 0);
    return EXIT_SUCCESS;
}
