#include "mrc_dpdk.h"

#include <mrc_ctl.h>

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    PROFILE_ID = 7,
    CC_PROFILE_ID = 8,
    DISABLED_CC_PROFILE_ID = 9,
    QA_CC_PROFILE_ID = 10,
    GEN_UDP_PROFILE_ID = 11,
    GEN_STEV_PROFILE_ID = 12,
    EXP_SRV6_PROFILE_ID = 13,
    EXP_SRV6_SRH_PROFILE_ID = 14,
    GEN_SRV6_PROFILE_ID = 15,
    GEN_SRV6_SRH_PROFILE_ID = 16,
    GEN_CUSTOM_STEV_PROFILE_ID = 17,
    GEN_CUSTOM_SRV6_PROFILE_ID = 18,
    EXP_CUSTOM_UDP_PROFILE_ID = 19,
    EVENT_PROFILE_ID = 20,
    CUSTOM_STEV_FORMAT_ID = 100,
    CUSTOM_SRV6_FORMAT_ID = 101,
    CUSTOM_UDP_FORMAT_ID = 102,
    SRV6_SID_BYTES = 16,
    SRV6_SID_FIELDS = SRV6_SID_BYTES / 2,
    SRV6_MAX_FIELDS = MRC_CTL_EV_MAX_BYTES / 2,
    RAILS = 2,
    FIRST_EV = 0xc101,
    SECOND_EV = 0xc102,
};

struct generated_profile {
    unsigned int id;
    uint64_t format;
    int field_count;
    struct mrc_ctl_ev_field fields[3];
    uint32_t values[RAILS];
};

static const struct generated_profile generated_udp = {
    .id = GEN_UDP_PROFILE_ID,
    .format = MRC_DPDK_EV_FORMAT_UDP,
    .field_count = 1,
    .fields = {{ .width = 16, .init_val = 0xc400, .min_val = 3, .max_val = 8, .mask = 0x35 }},
    .values = { 0xc405, 0xc410 },
};

static const struct generated_profile generated_stev = {
    .id = GEN_STEV_PROFILE_ID,
    .format = MRC_DPDK_EV_FORMAT_STEV,
    .field_count = 2,
    .fields = {
        { .width = 12, .init_val = 0x0a00, .max_val = 1, .mask = 0xa0 },
        { .width = 16, .init_val = 0xe499 },
    },
    .values = { 0x0a00e499, 0x0a20e499 },
};

static const struct generated_profile generated_custom_stev = {
    .id = GEN_CUSTOM_STEV_PROFILE_ID,
    .format = CUSTOM_STEV_FORMAT_ID,
    .field_count = 3,
    .fields = {
        { .width = 9, .init_val = 0x1ab },
        { .width = 8, .init_val = 0xa3 },
        { .width = 8, .init_val = 0xc0, .min_val = 1, .max_val = 2, .mask = 3 },
    },
    .values = { 0x1ab28cc1, 0x1ab28cc2 },
};

enum {
    GENERATED_PROFILE_ATTRS = MRC_CTL_EV_PROFILE_STATE | MRC_CTL_EV_PROFILE_MODE |
        MRC_CTL_EV_PROFILE_FMT_ID | MRC_CTL_EV_PROFILE_COUNT | MRC_CTL_EV_PROFILE_EV_FIELDS_OP,
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
    assert((legacy.ev.ev_mode_mask & extended.ev.ev_mode_mask) & MRC_CTL_EV_MODE_GEN);
    assert(extended.ev.ev_fmt_mode_mask & MRC_CTL_EV_FMT_MODE_UDP);
    assert(extended.ev.ev_fmt_mode_mask & MRC_CTL_EV_FMT_MODE_STEV);
    assert(extended.ev.ev_fmt_mode_mask & MRC_CTL_EV_FMT_MODE_SRV6);
    assert(extended.ev.ev_max_fmt_profiles > 4 && extended.ev.ev_max_width == 256);
    assert(legacy.cc.cc_max_profiles == 32 && extended.cc.cc_max_profiles == 32);
    assert(legacy.cc.algorithms && legacy.cc.algorithms[0] &&
           strcmp(legacy.cc.algorithms[0], nscc_algorithm) == 0 && !legacy.cc.algorithms[1]);
    assert(extended.cc.algorithms && extended.cc.algorithms[0] &&
           strcmp(extended.cc.algorithms[0], nscc_algorithm) == 0 && !extended.cc.algorithms[1]);
    assert(!((legacy.opt_attr | extended.opt_attr) & MRC_CTL_OPT_CAP_CC_PROFILE_MODIFY_ONLINE));
    assert((legacy.opt_attr & extended.opt_attr) & MRC_CTL_OPT_CAP_EV_EVENT);
    assert((legacy.opt_attr & extended.opt_attr) & MRC_CTL_OPT_CAP_EP_OP_EV_PROBE);
    assert(!((legacy.opt_attr | extended.opt_attr) &
             (MRC_CTL_OPT_CAP_EV_PROFILE_MODIFY_ONLINE | MRC_CTL_OPT_CAP_EV_EVENT_PRECISE_DROP_CNT)));

    static const struct {
        uint64_t id;
        enum mrc_ctl_ev_fmt_mode mode;
        unsigned int width;
    } formats[] = {
        { MRC_DPDK_EV_FORMAT_UDP, MRC_CTL_EV_FMT_MODE_UDP, 16 },
        { MRC_DPDK_EV_FORMAT_STEV, MRC_CTL_EV_FMT_MODE_STEV, 32 },
        { MRC_DPDK_EV_FORMAT_SRV6, MRC_CTL_EV_FMT_MODE_SRV6, 128 },
        { MRC_DPDK_EV_FORMAT_SRV6_SRH, MRC_CTL_EV_FMT_MODE_SRV6, 256 },
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        struct mrc_ctl_ev_fmt_field fields[SRV6_MAX_FIELDS] = {0};
        struct mrc_ctl_ev_fmt_profile_attr attr = {
            .ev_fmt_op = {
                .op = MRC_CTL_EV_FMT_OP_QUERY_FIELDS, .fmt_fields = fields, .fmt_field_count = 0,
            },
        };
        assert(mrc_ctl_query_ev_fmt_profile(context, formats[i].id, &attr,
                                          MRC_CTL_EV_FMT_PROFILE_OP) == -1 && errno == E2BIG);
        assert(attr.ev_fmt_op.fmt_field_count == (int)formats[i].width / 16);
        assert(mrc_ctl_query_ev_fmt_profile(context, formats[i].id, &attr,
                                          MRC_CTL_EV_FMT_PROFILE_MODE | MRC_CTL_EV_FMT_PROFILE_OP) == 0);
        assert(attr.ev_fmt_mode == formats[i].mode);
        assert(attr.ev_fmt_op.fmt_field_count == (int)formats[i].width / 16);
        for (int field = 0; field < attr.ev_fmt_op.fmt_field_count; field++)
            assert(fields[field].width == 16);
    }
}

static int configure_explicit(struct mrc_context *context, uint64_t id, uint64_t format)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE,
        .ev_mode = MRC_CTL_EV_MODE_EXP,
        .ev_fmt_profile_id = format,
        .ev_count = RAILS,
    };
    return mrc_ctl_modify_ev_profile(context, id, &attr, MRC_CTL_EV_PROFILE_STATE |
                                     MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_FMT_ID |
                                     MRC_CTL_EV_PROFILE_COUNT);
}

static int set_profile_state(struct mrc_context *context, uint64_t id, enum mrc_ctl_profile_state state)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = state,
        .ev_min_active = 1,
    };
    int mask = MRC_CTL_EV_PROFILE_STATE;
    if (state == MRC_CTL_PROFILE_ONLINE)
        mask |= MRC_CTL_EV_PROFILE_MIN_ACTIVE;
    return mrc_ctl_modify_ev_profile(context, id, &attr, mask);
}

static enum mrc_ctl_profile_state profile_state(struct mrc_context *context, uint64_t id)
{
    struct mrc_ctl_ev_profile_attr attr = {0};
    assert(mrc_ctl_query_ev_profile(context, id, &attr, MRC_CTL_EV_PROFILE_CUR_STATE) == 0);
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

static int replace_evs(struct mrc_context *context, uint64_t id, struct mrc_ctl_ev_op_entry *entries, int count)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_REPLACE_EV, .entries = entries, .entry_count = count },
    };
    return mrc_ctl_modify_ev_profile(context, id, &attr, MRC_CTL_EV_PROFILE_EV_OP);
}

static void query_evs(struct mrc_context *context, uint64_t id, enum mrc_ctl_ev_mode mode,
                      uint64_t format, struct mrc_ctl_ev_op_entry entries[RAILS])
{
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_QUERY_EV_ARRAY, .entries = entries, .entry_count = 1 },
    };
    const int mask = MRC_CTL_EV_PROFILE_EV_OP | MRC_CTL_EV_PROFILE_COUNT |
                     MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_FMT_ID;
    assert(mrc_ctl_query_ev_profile(context, id, &attr, mask) == -1 && errno == E2BIG);
    assert(attr.ev_op.entry_count == RAILS);
    assert(mrc_ctl_query_ev_profile(context, id, &attr, mask) == 0);
    assert(attr.ev_op.entry_count == RAILS && attr.ev_count == RAILS);
    assert(attr.ev_mode == mode && attr.ev_fmt_profile_id == format);
}

static void check_evs(struct mrc_context *context, uint32_t first, uint32_t second)
{
    struct mrc_ctl_ev_op_entry entries[RAILS] = {0};
    query_evs(context, PROFILE_ID, MRC_CTL_EV_MODE_EXP, MRC_DPDK_EV_FORMAT_UDP, entries);
    assert(entries[0].ev.port == 1 && ev_value(&entries[0].ev) == first);
    assert(entries[1].ev.port == 2 && ev_value(&entries[1].ev) == second);
}

static void populate_evs(struct mrc_context *controller)
{
    struct mrc_ctl_ev_op_entry entries[RAILS] = {
        { .ev = ev(1, 0), .ev_new = ev(1, FIRST_EV), .state = MRC_CTL_EV_GOOD },
        { .ev = ev(2, FIRST_EV), .ev_new = ev(2, SECOND_EV), .state = MRC_CTL_EV_GOOD },
    };
    assert(replace_evs(controller, PROFILE_ID, entries, RAILS) == -1 && errno == ENOENT);
    check_evs(controller, FIRST_EV, 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == -1 && errno == EINVAL);
    assert(profile_state(controller, PROFILE_ID) == MRC_CTL_PROFILE_OFFLINE);

    entries[1].ev = ev(2, 0);
    assert(replace_evs(controller, PROFILE_ID, &entries[1], 1) == 0);
    check_evs(controller, FIRST_EV, SECOND_EV);
}

static void set_ev_state(struct mrc_context *context, uint64_t id, struct mrc_ctl_ev value,
                         enum mrc_ctl_ev_state state)
{
    struct mrc_ctl_ev_op_entry entry = { .ev = value, .state = state };
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_MODIFY_EV_STATE, .entries = &entry, .entry_count = 1 },
    };
    assert(mrc_ctl_modify_ev_profile(context, id, &attr, MRC_CTL_EV_PROFILE_EV_OP) == 0);
}

static enum mrc_ctl_ev_state get_ev_state(struct mrc_context *context, uint64_t id,
                                          struct mrc_ctl_ev value)
{
    struct mrc_ctl_ev_op_entry entry = { .ev = value };
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_QUERY_EV_STATE, .entries = &entry, .entry_count = 1 },
    };
    assert(mrc_ctl_query_ev_profile(context, id, &attr, MRC_CTL_EV_PROFILE_EV_OP) == 0);
    return entry.state;
}

static void select_profile(struct mrc_qp *qp, uint64_t id)
{
    struct ibv_qp_attr verbs = { .qp_state = IBV_QPS_INIT };
    struct mrc_qp_attr attrs = { .profile.ev_profile_id = id };
    assert(mrc_modify_qp(qp, &verbs, IBV_QP_STATE, &attrs, MRC_QP_EV_PROFILE_ID) == 0);

    attrs = (struct mrc_qp_attr){0};
    assert(mrc_query_qp(qp, &verbs, IBV_QP_STATE, &attrs, MRC_QP_EV_PROFILE_ID, NULL) == 0);
    assert(verbs.qp_state == IBV_QPS_INIT && attrs.profile.ev_profile_id == id);
}

static void test_auto_profile(struct mrc_context *controller)
{
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE, .ev_mode = MRC_CTL_EV_MODE_AUTO, .ev_count = RAILS,
    };
    assert(mrc_ctl_modify_ev_profile(controller, PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_STATE |
                                     MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_COUNT) == 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == 0);
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
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_INIT) == 0);
}

static int query_event_mask(struct mrc_context *context)
{
    struct mrc_ctl_ev_profile_attr attr = {0};
    assert(mrc_ctl_query_ev_profile(context, EVENT_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EVENT_MASK) == 0);
    return attr.ev_event_mask;
}

static void test_event_masks(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *controller = open_store(verbs, directory);
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE, .ev_mode = MRC_CTL_EV_MODE_AUTO, .ev_count = RAILS,
    };
    assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_STATE |
                                     MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_COUNT) == 0);
    assert(query_event_mask(controller) == 0);
    attr.ev_event_mask = MRC_CTL_EV_DENIED;
    assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EVENT_MASK) == -1 &&
           errno == EINVAL);
    assert(query_event_mask(controller) == 0);
    attr.ev_event_mask = MRC_CTL_EV_ASSUMED_BAD;
    assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EVENT_MASK) == 0);
    assert(query_event_mask(controller) == MRC_CTL_EV_ASSUMED_BAD);

    attr.profile_state = MRC_CTL_PROFILE_ONLINE;
    attr.ev_event_mask = MRC_CTL_EV_GOOD | MRC_CTL_EV_ASSUMED_BAD;
    assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &attr,
                                     MRC_CTL_EV_PROFILE_STATE | MRC_CTL_EV_PROFILE_EVENT_MASK) == 0);
    struct mrc_context *reader = open_store(verbs, directory);
    assert(query_event_mask(reader) == attr.ev_event_mask);
    assert(mrc_destroy_context(reader) == 0);
    attr.ev_event_mask = 0;
    assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EVENT_MASK) == -1 &&
           errno == EBUSY);
    assert(query_event_mask(controller) == (MRC_CTL_EV_GOOD | MRC_CTL_EV_ASSUMED_BAD));
    assert(set_profile_state(controller, EVENT_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EVENT_MASK) == 0);
    assert(query_event_mask(controller) == 0);
    assert(set_profile_state(controller, EVENT_PROFILE_ID, MRC_CTL_PROFILE_INIT) == 0);
    assert(mrc_destroy_context(controller) == 0);
}

static void test_profile_lifecycle(struct ibv_context *verbs, struct mrc_context *controller,
                                   const char *directory)
{
    test_controller_discovery(verbs, controller);
    test_auto_profile(controller);
    assert(profile_state(controller, PROFILE_ID) == MRC_CTL_PROFILE_INIT);
    assert(configure_explicit(controller, PROFILE_ID, MRC_DPDK_EV_FORMAT_UDP) == 0);
    assert(profile_state(controller, PROFILE_ID) == MRC_CTL_PROFILE_OFFLINE);
    check_evs(controller, 0, 0);
    populate_evs(controller);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == 0);

    struct mrc_context *client = open_store(verbs, directory);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    struct mrc_cq *cq = mrc_create_cq(client, 4, NULL, NULL, 0);
    assert(pd && cq);
    struct mrc_qp_init_attr init = {
        .send_cq = cq, .pd = pd, .cap = { .max_send_wr = 1, .max_send_sge = 1 },
    };
    struct mrc_qp *qp = mrc_create_qp(client, &init);
    assert(qp);

    select_profile(qp, PROFILE_ID);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    set_ev_state(controller, PROFILE_ID, ev(2, SECOND_EV), MRC_CTL_EV_DENIED);
    assert(get_ev_state(client, PROFILE_ID, ev(2, SECOND_EV)) == MRC_CTL_EV_DENIED);

    assert(mrc_destroy_context(controller) == 0);
    controller = open_store(verbs, directory);
    assert(profile_state(controller, PROFILE_ID) == MRC_CTL_PROFILE_ONLINE);
    check_evs(controller, FIRST_EV, SECOND_EV);
    assert(get_ev_state(controller, PROFILE_ID, ev(2, SECOND_EV)) == MRC_CTL_EV_DENIED);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    set_ev_state(controller, PROFILE_ID, ev(2, SECOND_EV), MRC_CTL_EV_GOOD);
    assert(get_ev_state(client, PROFILE_ID, ev(2, SECOND_EV)) == MRC_CTL_EV_GOOD);

    struct ibv_qp_attr reset = { .qp_state = IBV_QPS_RESET };
    assert(mrc_modify_qp(qp, &reset, IBV_QP_STATE, NULL, 0) == 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == 0);

    select_profile(qp, PROFILE_ID);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    assert(mrc_destroy_qp(qp) == 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_INIT) == 0);
    assert(profile_state(controller, PROFILE_ID) == MRC_CTL_PROFILE_INIT);

    assert(mrc_destroy_cq(cq) == 0);
    assert(ibv_dealloc_pd(pd) == 0);
    assert(mrc_destroy_context(client) == 0);
    assert(mrc_destroy_context(controller) == 0);
}

static struct mrc_ctl_ev_profile_attr generated_attributes(uint64_t format,
                                                           struct mrc_ctl_ev_field *fields, int count)
{
    return (struct mrc_ctl_ev_profile_attr){
        .profile_state = MRC_CTL_PROFILE_OFFLINE, .ev_mode = MRC_CTL_EV_MODE_GEN,
        .ev_fmt_profile_id = format, .ev_count = RAILS,
        .ev_fields_op = { .op = MRC_CTL_EV_FIELDS_OP_MODIFY_FIELDS, .fields = fields, .field_count = count },
    };
}

static int configure_generated(struct mrc_context *context, struct generated_profile profile)
{
    struct mrc_ctl_ev_profile_attr attr =
        generated_attributes(profile.format, profile.fields, profile.field_count);
    return mrc_ctl_modify_ev_profile(context, profile.id, &attr, GENERATED_PROFILE_ATTRS);
}

static int set_format_state(struct mrc_context *context, uint64_t id, enum mrc_ctl_profile_state state)
{
    struct mrc_ctl_ev_fmt_profile_attr attr = { .profile_state = state };
    return mrc_ctl_modify_ev_fmt_profile(context, id, &attr, MRC_CTL_EV_FMT_PROFILE_STATE);
}

static int configure_format(struct mrc_context *context, uint64_t id, enum mrc_ctl_ev_fmt_mode mode)
{
    struct mrc_ctl_ev_fmt_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE, .ev_fmt_mode = mode,
    };
    return mrc_ctl_modify_ev_fmt_profile(context, id, &attr, MRC_CTL_EV_FMT_PROFILE_STATE |
                                         MRC_CTL_EV_FMT_PROFILE_MODE);
}

static int activate_format(struct mrc_context *context, uint64_t id,
                            struct mrc_ctl_ev_fmt_field *fields, int count)
{
    struct mrc_ctl_ev_fmt_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_ONLINE,
        .ev_fmt_op = {
            .op = MRC_CTL_EV_FMT_OP_MODIFY_FIELDS, .fmt_fields = fields, .fmt_field_count = count,
        },
    };
    return mrc_ctl_modify_ev_fmt_profile(context, id, &attr, MRC_CTL_EV_FMT_PROFILE_STATE |
                                         MRC_CTL_EV_FMT_PROFILE_OP);
}

static void check_format(struct mrc_context *context, uint64_t id, enum mrc_ctl_ev_fmt_mode mode,
                          const struct mrc_ctl_ev_fmt_field *expected, int count)
{
    struct mrc_ctl_ev_fmt_field fields[SRV6_MAX_FIELDS] = {0};
    struct mrc_ctl_ev_fmt_profile_attr attr = {
        .ev_fmt_op = {
            .op = MRC_CTL_EV_FMT_OP_QUERY_FIELDS, .fmt_fields = fields, .fmt_field_count = count - 1,
        },
    };
    const int mask = MRC_CTL_EV_FMT_PROFILE_CUR_STATE | MRC_CTL_EV_FMT_PROFILE_MODE |
        MRC_CTL_EV_FMT_PROFILE_OP;
    assert(mrc_ctl_query_ev_fmt_profile(context, id, &attr, mask) == -1 && errno == E2BIG);
    assert(attr.ev_fmt_op.fmt_field_count == count);
    assert(mrc_ctl_query_ev_fmt_profile(context, id, &attr, mask) == 0);
    assert(attr.cur_profile_state == MRC_CTL_PROFILE_ONLINE && attr.ev_fmt_mode == mode);
    assert(attr.ev_fmt_op.fmt_field_count == count);
    for (int i = 0; i < count; i++)
        assert(fields[i].width == expected[i].width);
}

static void prepare_custom_stev_format(struct mrc_context *context)
{
    const uint64_t id = CUSTOM_STEV_FORMAT_ID;
    struct mrc_ctl_ev_fmt_field widths[] = {{12}, {10}, {10}};
    assert(configure_format(context, id, MRC_CTL_EV_FMT_MODE_STEV) == 0);
    assert(activate_format(context, id, widths, 3) == 0);
}

static void check_generated_fields(struct mrc_context *context, uint64_t id,
                                     const struct mrc_ctl_ev_field *expected, int count)
{
    struct mrc_ctl_ev_field fields[SRV6_MAX_FIELDS] = {0};
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_fields_op = {
            .op = MRC_CTL_EV_FIELDS_OP_QUERY_FIELDS, .fields = fields, .field_count = count - 1,
        },
    };
    assert(mrc_ctl_query_ev_profile(context, id, &attr,
                                    MRC_CTL_EV_PROFILE_EV_FIELDS_OP) == -1 && errno == E2BIG);
    assert(attr.ev_fields_op.field_count == count);
    assert(mrc_ctl_query_ev_profile(context, id, &attr, MRC_CTL_EV_PROFILE_EV_FIELDS_OP) == 0);
    assert(attr.ev_fields_op.field_count == count);
    for (int i = 0; i < count; i++) {
        const struct mrc_ctl_ev_field *field = &expected[i];
        assert(fields[i].width == field->width && fields[i].init_val == field->init_val &&
               fields[i].min_val == field->min_val && fields[i].max_val == field->max_val &&
               fields[i].mask == field->mask);
    }
}

static void check_generated(struct mrc_context *context, const struct generated_profile *expected)
{
    check_generated_fields(context, expected->id, expected->fields, expected->field_count);
    struct mrc_ctl_ev_op_entry entries[RAILS] = {0};
    query_evs(context, expected->id, MRC_CTL_EV_MODE_GEN, expected->format, entries);
    for (unsigned int i = 0; i < RAILS; i++) {
        assert(entries[i].ev.port == i + 1 && ev_value(&entries[i].ev) == expected->values[i]);
        assert(entries[i].state == MRC_CTL_EV_GOOD);
    }
}

static void check_generated_errors(struct mrc_context *context)
{
    struct mrc_ctl_ev_field fields[2] = { generated_udp.fields[0] };
    struct mrc_ctl_ev_profile_attr attr = generated_attributes(generated_udp.format, fields, 1);
    static const int required[] = {
        MRC_CTL_EV_PROFILE_MODE, MRC_CTL_EV_PROFILE_FMT_ID, MRC_CTL_EV_PROFILE_COUNT,
        MRC_CTL_EV_PROFILE_EV_FIELDS_OP,
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
        assert(mrc_ctl_modify_ev_profile(context, GEN_UDP_PROFILE_ID, &attr,
                                         GENERATED_PROFILE_ATTRS & ~required[i]) == -1 && errno == EINVAL);
    attr.ev_fields_op.field_count = 2;
    assert(mrc_ctl_modify_ev_profile(context, GEN_UDP_PROFILE_ID, &attr,
                                     GENERATED_PROFILE_ATTRS) == -1 && errno == E2BIG);
    attr.ev_fields_op.field_count = 1;

    static const struct mrc_ctl_ev_field invalid[] = {
        { .width = 17, .init_val = 0xc400, .max_val = 1, .mask = 1 },
        { .width = 16, .init_val = 0x10000, .max_val = 1, .mask = 1 },
        { .width = 16, .init_val = 0xc400, .max_val = 1, .mask = 0x10000 },
        { .width = 16, .init_val = 0xc400, .min_val = 2, .max_val = 1, .mask = 3 },
        { .width = 16, .init_val = 0xc400, .max_val = 2, .mask = 1 },
        /* Two encodings, but zero is an invalid UDP source port. */
        { .width = 16, .max_val = 1, .mask = 1 },
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        fields[0] = invalid[i];
        assert(mrc_ctl_modify_ev_profile(context, GEN_UDP_PROFILE_ID, &attr,
                                         GENERATED_PROFILE_ATTRS) == -1 && errno == EINVAL);
        assert(profile_state(context, GEN_UDP_PROFILE_ID) == MRC_CTL_PROFILE_INIT);
    }
}

static void check_generation_order(struct mrc_context *context)
{
    struct generated_profile skip_zero = generated_udp;
    skip_zero.fields[0] = (struct mrc_ctl_ev_field){ .width = 16, .max_val = 2, .mask = 3 };
    skip_zero.values[0] = 1;
    skip_zero.values[1] = 2;
    assert(configure_generated(context, skip_zero) == 0);
    check_generated(context, &skip_zero);
    assert(set_profile_state(context, skip_zero.id, MRC_CTL_PROFILE_INIT) == 0);

    struct generated_profile mixed = generated_stev;
    mixed.fields[1] = (struct mrc_ctl_ev_field){
        .width = 16, .init_val = 0xe498, .min_val = 1, .max_val = 2, .mask = 3,
    };
    mixed.values[1] = 0x0a00e49a;
    assert(configure_generated(context, mixed) == 0);
    check_generated(context, &mixed);
    assert(set_profile_state(context, mixed.id, MRC_CTL_PROFILE_INIT) == 0);
}

static void test_generated_profiles(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *controller = open_store(verbs, directory);
    check_generated_errors(controller);
    check_generation_order(controller);
    assert(configure_generated(controller, generated_udp) == 0);
    assert(configure_generated(controller, generated_stev) == 0);
    assert(set_profile_state(controller, GEN_UDP_PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == 0);
    assert(set_profile_state(controller, GEN_STEV_PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == 0);
    assert(mrc_destroy_context(controller) == 0);
    controller = open_store(verbs, directory);
    assert(profile_state(controller, GEN_UDP_PROFILE_ID) == MRC_CTL_PROFILE_ONLINE);
    assert(profile_state(controller, GEN_STEV_PROFILE_ID) == MRC_CTL_PROFILE_ONLINE);
    check_generated(controller, &generated_udp);
    check_generated(controller, &generated_stev);

    struct mrc_ctl_ev_field field = generated_udp.fields[0];
    field.init_val = 0xc500;
    struct mrc_ctl_ev_profile_attr attr = generated_attributes(generated_udp.format, &field, 1);
    assert(mrc_ctl_modify_ev_profile(controller, GEN_UDP_PROFILE_ID, &attr,
                                     MRC_CTL_EV_PROFILE_EV_FIELDS_OP) == -1 && errno == EBUSY);
    check_generated(controller, &generated_udp);

    struct mrc_context *client = open_store(verbs, directory);
    struct ibv_pd *pd = ibv_alloc_pd(verbs);
    struct mrc_cq *cq = mrc_create_cq(client, 4, NULL, NULL, 0);
    assert(pd && cq);
    struct mrc_qp_init_attr init = {
        .send_cq = cq, .pd = pd, .cap = { .max_send_wr = 1, .max_send_sge = 1 },
    };
    struct mrc_qp *qp = mrc_create_qp(client, &init);
    assert(qp);
    select_profile(qp, GEN_STEV_PROFILE_ID);
    assert(set_profile_state(controller, GEN_STEV_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    struct mrc_ctl_ev varied = ev(2, generated_stev.values[1]);
    set_ev_state(controller, GEN_STEV_PROFILE_ID, varied, MRC_CTL_EV_DENIED);
    assert(get_ev_state(client, GEN_STEV_PROFILE_ID, varied) == MRC_CTL_EV_DENIED);
    set_ev_state(controller, GEN_STEV_PROFILE_ID, varied, MRC_CTL_EV_GOOD);
    assert(get_ev_state(client, GEN_STEV_PROFILE_ID, varied) == MRC_CTL_EV_GOOD);

    assert(mrc_destroy_qp(qp) == 0);
    assert(set_profile_state(controller, GEN_STEV_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(mrc_destroy_cq(cq) == 0);
    assert(ibv_dealloc_pd(pd) == 0);
    assert(mrc_destroy_context(client) == 0);
    assert(mrc_destroy_context(controller) == 0);
}

static void test_custom_stev_format(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *context = open_store(verbs, directory);
    const uint64_t format = CUSTOM_STEV_FORMAT_ID;
    const uint64_t profile = GEN_CUSTOM_STEV_PROFILE_ID;
    assert(set_format_state(context, MRC_DPDK_EV_FORMAT_STEV, MRC_CTL_PROFILE_OFFLINE) == -1 &&
           errno == EOPNOTSUPP);
    assert(set_format_state(context, format, MRC_CTL_PROFILE_ONLINE) == -1 && errno == EINVAL);
    assert(configure_format(context, format, MRC_CTL_EV_FMT_MODE_STEV) == 0);
    assert(configure_generated(context, generated_custom_stev) == -1 && errno == EINVAL);

    struct mrc_ctl_ev_fmt_field widths[] = {{12}, {10}, {9}};
    assert(activate_format(context, format, widths, 3) == -1 && errno == E2BIG);
    assert(set_format_state(context, format, MRC_CTL_PROFILE_ONLINE) == -1 && errno == EINVAL);
    widths[2].width = 10;
    assert(activate_format(context, format, widths, 3) == 0);

    struct generated_profile compact = generated_custom_stev;
    compact.field_count = 2;
    compact.fields[0] = (struct mrc_ctl_ev_field){ .width = 12, .max_val = 0xfff, .mask = 0xfff };
    compact.fields[1] = (struct mrc_ctl_ev_field){ .width = 10, .max_val = 15, .mask = 0x3c0 };
    /* All variable bits are in the flow label and the omitted UDP bits are zero. */
    assert(configure_generated(context, compact) == -1 && errno == EINVAL);
    compact.fields[0] = generated_custom_stev.fields[0];
    compact.fields[1] = (struct mrc_ctl_ev_field){ .width = 8, .init_val = 0xa0, .max_val = 1, .mask = 3 };
    compact.values[0] = 0x1ab28000;
    compact.values[1] = 0x1ab28400;
    assert(configure_generated(context, compact) == 0);
    check_generated(context, &compact);

    assert(configure_generated(context, generated_custom_stev) == 0);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_ONLINE) == 0);
    assert(set_format_state(context, format, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    assert(mrc_destroy_context(context) == 0);

    context = open_store(verbs, directory);
    check_format(context, format, MRC_CTL_EV_FMT_MODE_STEV, widths, 3);
    assert(profile_state(context, profile) == MRC_CTL_PROFILE_ONLINE);
    check_generated(context, &generated_custom_stev);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_format_state(context, format, MRC_CTL_PROFILE_OFFLINE) == 0);
    widths[0].width = 10;
    widths[1].width = 12;
    assert(activate_format(context, format, widths, 3) == 0);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_ONLINE) == -1 && errno == EINVAL);
    assert(profile_state(context, profile) == MRC_CTL_PROFILE_OFFLINE);

    assert(configure_generated(context, generated_custom_stev) == 0);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_ONLINE) == 0);
    struct generated_profile rebound = generated_custom_stev;
    rebound.values[0] = 0x6ac28cc1;
    rebound.values[1] = 0x6ac28cc2;
    check_generated(context, &rebound);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_INIT) == 0);
    assert(set_format_state(context, format, MRC_CTL_PROFILE_OFFLINE) == 0);
    assert(set_format_state(context, format, MRC_CTL_PROFILE_INIT) == 0);
    assert(mrc_destroy_context(context) == 0);
}

static void test_custom_udp_format(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *context = open_store(verbs, directory);
    const uint64_t format = CUSTOM_UDP_FORMAT_ID;
    const uint64_t profile = EXP_CUSTOM_UDP_PROFILE_ID;
    struct mrc_ctl_ev_fmt_field widths[] = {{3}, {5}, {8}};
    assert(configure_format(context, format, MRC_CTL_EV_FMT_MODE_UDP) == 0);
    assert(activate_format(context, format, widths, 3) == 0);
    assert(configure_explicit(context, profile, format) == 0);

    struct mrc_ctl_ev_op_entry entries[RAILS] = {
        { .ev = ev(1, 0), .ev_new = ev(1, 0x5421), .state = MRC_CTL_EV_GOOD },
        { .ev = ev(2, 0), .ev_new = ev(2, 0x9123), .state = MRC_CTL_EV_DENIED },
    };
    assert(replace_evs(context, profile, entries, RAILS) == 0);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_ONLINE) == 0);
    assert(mrc_destroy_context(context) == 0);

    context = open_store(verbs, directory);
    check_format(context, format, MRC_CTL_EV_FMT_MODE_UDP, widths, 3);
    query_evs(context, profile, MRC_CTL_EV_MODE_EXP, format, entries);
    assert(entries[0].ev.port == 1 && ev_value(&entries[0].ev) == 0x5421 && entries[0].state == MRC_CTL_EV_GOOD);
    assert(entries[1].ev.port == 2 && ev_value(&entries[1].ev) == 0x9123 && entries[1].state == MRC_CTL_EV_DENIED);
    assert(mrc_destroy_context(context) == 0);
}

static void test_custom_srv6_format(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *context = open_store(verbs, directory);
    const uint64_t format = CUSTOM_SRV6_FORMAT_ID;
    const uint64_t profile = GEN_CUSTOM_SRV6_PROFILE_ID;
    assert(configure_format(context, format, MRC_CTL_EV_FMT_MODE_SRV6) == 0);
    struct mrc_ctl_ev_fmt_field widths[] = {
        {11}, {21}, {32}, {32}, {31}, {30}, {3}, {32}, {32}, {16}, {16},
    };
    const int slots = (int)(sizeof(widths) / sizeof(widths[0]));
    /* The original widths total 256 but the sixth slot crosses the 128-bit boundary. */
    assert(activate_format(context, format, widths, slots) == -1 && errno == E2BIG);
    widths[4].width = 32;
    widths[5].width = 29;
    assert(activate_format(context, format, widths, slots) == 0);

    struct mrc_ctl_ev_field fields[] = {
        { .width = 11, .init_val = 0x100 },
        { .width = 21, .init_val = 0x10db8 },
        { .width = 32, .init_val = 0x12345678 },
        { .width = 32, .init_val = 0x9abc0000 },
        { .width = 32, .init_val = 1 },
        { .width = 29, .init_val = 0x040021b7 },
        { .width = 3, .init_val = 5 },
        { .width = 32, .init_val = 0xc001d00d },
        { .width = 32, .init_val = 0x12345678 },
        { .width = 16, .init_val = 0xbe00, .min_val = 1, .max_val = 2, .mask = 3 },
    };
    const int count = (int)(sizeof(fields) / sizeof(fields[0]));
    struct mrc_ctl_ev_profile_attr attr = generated_attributes(format, fields, count);
    assert(mrc_ctl_modify_ev_profile(context, profile, &attr, GENERATED_PROFILE_ATTRS) == 0);
    assert(set_profile_state(context, profile, MRC_CTL_PROFILE_ONLINE) == 0);
    assert(mrc_destroy_context(context) == 0);

    context = open_store(verbs, directory);
    check_format(context, format, MRC_CTL_EV_FMT_MODE_SRV6, widths, slots);
    assert(profile_state(context, profile) == MRC_CTL_PROFILE_ONLINE);
    check_generated_fields(context, profile, fields, count);
    struct mrc_ctl_ev_op_entry entries[RAILS] = {0};
    query_evs(context, profile, MRC_CTL_EV_MODE_GEN, format, entries);
    for (uint8_t i = 0; i < RAILS; i++) {
        const uint8_t expected[MRC_CTL_EV_MAX_BYTES] = {
            0x20, 1, 0x0d, 0xb8, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0, 0, 0, 0, 0, 1,
            0x20, 1, 0x0d, 0xbd, 0xc0, 1, 0xd0, 0x0d, 0x12, 0x34, 0x56, 0x78, 0xbe, i + 1, 0, 0,
        };
        assert(entries[i].ev.port == i + 1 && entries[i].state == MRC_CTL_EV_GOOD);
        assert(!memcmp(entries[i].ev.val, expected, sizeof(expected)));
    }
    assert(mrc_destroy_context(context) == 0);
}

static struct mrc_ctl_ev srv6_fixture_ev(uint8_t rail, bool srh)
{
    struct mrc_ctl_ev value = {
        .port = rail, .val = {0x20, 0x01, 0x0d, 0xb8, 0, 1, [15] = rail},
    };
    if (srh) {
        const uint8_t segment[SRV6_SID_BYTES] = {0x20, 0x01, 0x0d, 0xb8, 0, 2, [15] = 0x42};
        memcpy(value.val + SRV6_SID_BYTES, segment, sizeof(segment));
    }
    return value;
}

static int fixture_srv6_fields(struct mrc_ctl_ev_field fields[SRV6_MAX_FIELDS], bool srh)
{
    int count = srh ? SRV6_MAX_FIELDS : SRV6_SID_FIELDS;
    struct mrc_ctl_ev fixture = srv6_fixture_ev(1, srh);
    for (int i = 0; i < count; i++) {
        uint32_t word = (uint32_t)fixture.val[2 * i] << 8 | fixture.val[2 * i + 1];
        fields[i] = (struct mrc_ctl_ev_field){ .width = 16, .init_val = word };
    }
    fields[SRV6_SID_FIELDS - 1] = (struct mrc_ctl_ev_field){
        .width = 8, .min_val = 1, .max_val = RAILS, .mask = UINT8_MAX,
    };
    return count;
}

static void prepare_generated_srv6(struct mrc_context *context, bool srh)
{
    struct mrc_ctl_ev_field fields[SRV6_MAX_FIELDS];
    int count = fixture_srv6_fields(fields, srh);
    uint64_t id = srh ? GEN_SRV6_SRH_PROFILE_ID : GEN_SRV6_PROFILE_ID;
    uint64_t format = srh ? MRC_DPDK_EV_FORMAT_SRV6_SRH : MRC_DPDK_EV_FORMAT_SRV6;
    struct mrc_ctl_ev_profile_attr attr = generated_attributes(format, fields, count);
    assert(mrc_ctl_modify_ev_profile(context, id, &attr, GENERATED_PROFILE_ATTRS) == 0);
    assert(set_profile_state(context, id, MRC_CTL_PROFILE_ONLINE) == 0);
}

static void prepare_explicit_srv6(struct mrc_context *context, enum mrc_ctl_ev_state state)
{
    assert(configure_explicit(context, EXP_SRV6_PROFILE_ID, MRC_DPDK_EV_FORMAT_SRV6) == 0);
    struct mrc_ctl_ev_op_entry entries[RAILS] = {
        { .ev = { .port = 1 }, .ev_new = srv6_fixture_ev(1, false), .state = state },
        { .ev = { .port = 2 }, .ev_new = srv6_fixture_ev(2, false), .state = state },
    };
    assert(replace_evs(context, EXP_SRV6_PROFILE_ID, entries, RAILS) == 0);
    assert(set_profile_state(context, EXP_SRV6_PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == 0);
}

static void unblock_explicit_srv6(struct mrc_context *context)
{
    struct mrc_ctl_ev_op_entry entries[RAILS] = {
        { .ev = srv6_fixture_ev(1, false), .state = MRC_CTL_EV_GOOD },
        { .ev = srv6_fixture_ev(2, false), .state = MRC_CTL_EV_GOOD },
    };
    struct mrc_ctl_ev_profile_attr attr = {
        .ev_op = { .op = MRC_CTL_EV_OP_MODIFY_EV_STATE, .entries = entries, .entry_count = RAILS },
    };
    assert(set_profile_state(context, EXP_SRV6_PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
    assert(mrc_ctl_modify_ev_profile(context, EXP_SRV6_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_EV_OP) == 0);
}

static void check_srv6_fixture(struct mrc_context *context, uint64_t id, enum mrc_ctl_ev_mode mode,
                               bool srh, enum mrc_ctl_ev_state state)
{
    assert(profile_state(context, id) == MRC_CTL_PROFILE_ONLINE);
    uint64_t format = srh ? MRC_DPDK_EV_FORMAT_SRV6_SRH : MRC_DPDK_EV_FORMAT_SRV6;
    struct mrc_ctl_ev_op_entry entries[RAILS] = {0};
    query_evs(context, id, mode, format, entries);
    for (uint8_t i = 0; i < RAILS; i++) {
        struct mrc_ctl_ev expected = srv6_fixture_ev(i + 1, srh);
        assert(entries[i].ev.port == expected.port);
        assert(!memcmp(entries[i].ev.val, expected.val, sizeof(expected.val)));
        assert(entries[i].state == state);
        assert(get_ev_state(context, id, expected) == state);
    }
    if (mode == MRC_CTL_EV_MODE_GEN) {
        struct mrc_ctl_ev_field fields[SRV6_MAX_FIELDS];
        int count = fixture_srv6_fields(fields, srh);
        check_generated_fields(context, id, fields, count);
    }
}

static void test_explicit_srv6_values(struct mrc_context *context)
{
    uint64_t id = EXP_SRV6_SRH_PROFILE_ID;
    uint64_t format = MRC_DPDK_EV_FORMAT_SRV6_SRH;
    struct mrc_ctl_ev_profile_attr auto_attr = {
        .profile_state = MRC_CTL_PROFILE_OFFLINE, .ev_mode = MRC_CTL_EV_MODE_AUTO,
        .ev_fmt_profile_id = format, .ev_count = RAILS,
    };
    int mask = MRC_CTL_EV_PROFILE_STATE | MRC_CTL_EV_PROFILE_MODE |
        MRC_CTL_EV_PROFILE_FMT_ID | MRC_CTL_EV_PROFILE_COUNT;
    assert(mrc_ctl_modify_ev_profile(context, id, &auto_attr, mask) == -1 && errno == EINVAL);
    assert(configure_explicit(context, id, format) == 0);
    struct mrc_ctl_ev_op_entry entry = {
        .ev = { .port = 1 }, .ev_new = srv6_fixture_ev(1, false), .state = MRC_CTL_EV_GOOD,
    };
    assert(get_ev_state(context, id, entry.ev) == MRC_CTL_EV_UNKNOWN);
    assert(replace_evs(context, id, &entry, 1) == -1 && errno == EINVAL);
    entry.ev_new = srv6_fixture_ev(1, true);
    entry.ev_new.val[0] = 0xff;
    assert(replace_evs(context, id, &entry, 1) == -1 && errno == EINVAL);
    entry.ev_new.val[0] = 0x20;
    entry.ev_new.val[SRV6_SID_BYTES] = 0xff;
    assert(replace_evs(context, id, &entry, 1) == -1 && errno == EINVAL);

    /* The same route can be keyed by either rail, including a telemetry
     * copy of the first container as the SRH entry. */
    memcpy(entry.ev_new.val + SRV6_SID_BYTES, entry.ev_new.val, SRV6_SID_BYTES);
    assert(replace_evs(context, id, &entry, 1) == 0);
    struct mrc_ctl_ev first = entry.ev_new;
    assert(set_profile_state(context, id, MRC_CTL_PROFILE_ONLINE) == -1 && errno == EINVAL);
    entry.ev.port = entry.ev_new.port = 2;
    entry.state = MRC_CTL_EV_DENIED;
    assert(replace_evs(context, id, &entry, 1) == 0);
    assert(set_profile_state(context, id, MRC_CTL_PROFILE_ONLINE) == 0);
    assert(get_ev_state(context, id, first) == MRC_CTL_EV_GOOD);
    assert(get_ev_state(context, id, entry.ev_new) == MRC_CTL_EV_DENIED);
    struct mrc_ctl_ev_op_entry entries[RAILS] = {0};
    query_evs(context, id, MRC_CTL_EV_MODE_EXP, format, entries);
    for (uint8_t i = 0; i < RAILS; i++) {
        assert(entries[i].ev.port == i + 1);
        assert(!memcmp(entries[i].ev.val, first.val, sizeof(first.val)));
    }

    entry.ev = first;
    entry.ev.val[MRC_CTL_EV_MAX_BYTES - 1] ^= 1;
    struct mrc_ctl_ev_profile_attr query = {
        .ev_op = { .op = MRC_CTL_EV_OP_QUERY_EV_STATE, .entries = &entry, .entry_count = 1 },
    };
    assert(mrc_ctl_query_ev_profile(context, id, &query, MRC_CTL_EV_PROFILE_EV_OP) == -1 && errno == ENOENT);
}

static void test_generated_srv6_bounds(struct mrc_context *context)
{
    struct mrc_ctl_ev_field fields[SRV6_MAX_FIELDS];
    for (int i = 0; i < SRV6_MAX_FIELDS; i++) {
        fields[i] = (struct mrc_ctl_ev_field){
            .width = 16, .max_val = UINT16_MAX, .mask = UINT16_MAX,
        };
    }
    struct mrc_ctl_ev_profile_attr attr =
        generated_attributes(MRC_DPDK_EV_FORMAT_SRV6_SRH, fields, SRV6_MAX_FIELDS);
    const uint64_t id = GEN_SRV6_SRH_PROFILE_ID;
    fields[0].width = 17;
    assert(mrc_ctl_modify_ev_profile(context, id, &attr, GENERATED_PROFILE_ATTRS) == -1 && errno == EINVAL);
    fields[0].width = 16;

    /* The first container has 2^128 raw combinations. Prove an
     * impossible second container can be rejected independently. */
    struct mrc_ctl_ev_field saved = fields[SRV6_SID_FIELDS];
    fields[SRV6_SID_FIELDS] = (struct mrc_ctl_ev_field){ .width = 16, .init_val = 0xff00 };
    assert(mrc_ctl_modify_ev_profile(context, id, &attr, GENERATED_PROFILE_ATTRS) == -1 && errno == EINVAL);
    fields[SRV6_SID_FIELDS] = saved;
    assert(mrc_ctl_modify_ev_profile(context, id, &attr, GENERATED_PROFILE_ATTRS) == 0);
    struct mrc_ctl_ev_op_entry entries[RAILS] = {0};
    query_evs(context, id, MRC_CTL_EV_MODE_GEN, MRC_DPDK_EV_FORMAT_SRV6_SRH, entries);
    for (uint8_t i = 0; i < RAILS; i++) {
        /* Each container skips its all-zero minimum. The rightmost field
         * increments first, so the valid primary container stays fixed. */
        uint8_t expected[MRC_CTL_EV_MAX_BYTES] = { [15] = 1, [31] = i + 1 };
        assert(entries[i].ev.port == i + 1 && entries[i].state == MRC_CTL_EV_GOOD);
        assert(!memcmp(entries[i].ev.val, expected, sizeof(expected)));
    }
    assert(set_profile_state(context, id, MRC_CTL_PROFILE_INIT) == 0);
}

static void test_srv6_profiles(struct ibv_context *verbs, const char *directory)
{
    struct mrc_context *context = open_store(verbs, directory);
    test_explicit_srv6_values(context);
    test_generated_srv6_bounds(context);
    prepare_explicit_srv6(context, MRC_CTL_EV_DENIED);
    prepare_generated_srv6(context, false);
    prepare_generated_srv6(context, true);
    assert(mrc_destroy_context(context) == 0);

    context = open_store(verbs, directory);
    check_srv6_fixture(context, EXP_SRV6_PROFILE_ID, MRC_CTL_EV_MODE_EXP, false, MRC_CTL_EV_DENIED);
    check_srv6_fixture(context, GEN_SRV6_PROFILE_ID, MRC_CTL_EV_MODE_GEN, false, MRC_CTL_EV_GOOD);
    check_srv6_fixture(context, GEN_SRV6_SRH_PROFILE_ID, MRC_CTL_EV_MODE_GEN, true, MRC_CTL_EV_GOOD);
    struct mrc_ctl_ev route = srv6_fixture_ev(2, true);
    set_ev_state(context, GEN_SRV6_SRH_PROFILE_ID, route, MRC_CTL_EV_DENIED);
    assert(get_ev_state(context, GEN_SRV6_SRH_PROFILE_ID, route) == MRC_CTL_EV_DENIED);
    assert(mrc_destroy_context(context) == 0);

    context = open_store(verbs, directory);
    assert(get_ev_state(context, GEN_SRV6_SRH_PROFILE_ID, route) == MRC_CTL_EV_DENIED);
    assert(mrc_destroy_context(context) == 0);
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
    assert(configure_explicit(controller, PROFILE_ID, MRC_DPDK_EV_FORMAT_UDP) == 0);
    struct mrc_ctl_ev_op_entry entries[RAILS] = {
        { .ev = ev(1, 0), .ev_new = ev(1, FIRST_EV), .state = MRC_CTL_EV_DENIED },
        { .ev = ev(2, 0), .ev_new = ev(2, SECOND_EV), .state = MRC_CTL_EV_DENIED },
    };
    assert(replace_evs(controller, PROFILE_ID, entries, RAILS) == 0);
    assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_ONLINE) == 0);
    printf("%u\n", (unsigned int)PROFILE_ID);
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void watch_ev_events(struct mrc_context *controller, const char *directory, bool bad_only)
{
    struct mrc_ctl_device_attr device;
    int supported;
    assert(mrc_ctl_query_device_ex(NULL, controller, &device, &supported) == 0 && supported);
    assert(device.opt_attr & MRC_CTL_OPT_CAP_EV_EVENT);
    assert(configure_explicit(controller, EVENT_PROFILE_ID, MRC_DPDK_EV_FORMAT_UDP) == 0);
    struct mrc_ctl_ev_op_entry evs[RAILS] = {
        { .ev = ev(1, 0), .ev_new = ev(1, 0xc405), .state = MRC_CTL_EV_GOOD },
        { .ev = ev(2, 0), .ev_new = ev(2, 0xc410), .state = MRC_CTL_EV_GOOD },
    };
    assert(replace_evs(controller, EVENT_PROFILE_ID, evs, RAILS) == 0);
    struct mrc_ctl_ev_profile_attr attr = {
        .profile_state = MRC_CTL_PROFILE_ONLINE, .ev_min_active = 1,
        .ev_event_mask = MRC_CTL_EV_ASSUMED_BAD | (bad_only ? 0 : MRC_CTL_EV_GOOD),
    };
    assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &attr, MRC_CTL_EV_PROFILE_STATE |
                                     MRC_CTL_EV_PROFILE_MIN_ACTIVE | MRC_CTL_EV_PROFILE_EVENT_MASK) == 0);

    enum { EVENT_CAPACITY = 32, DEADLINE_MS = 8000, DRAIN_MS = 200 };
    struct mrc_comp_channel *channel;
    assert(mrc_create_comp_channel(controller, &channel) == 0);
    struct mrc_cq *cq = mrc_ctl_create_ev_event_cq(controller, EVENT_CAPACITY, &device, channel, 0);
    assert(cq && mrc_req_notify_cq(cq, 0) == 0);
    struct pollfd wait = { .events = POLLIN };
    assert(mrc_get_comp_channel_fd(channel, &wait.fd) == 0);
    char finished[PATH_MAX];
    int length = snprintf(finished, sizeof(finished), "%s/tx.finished", directory);
    assert(length > 0 && (size_t)length < sizeof(finished));

    printf("%u\n", (unsigned int)EVENT_PROFILE_ID);
    assert(fflush(stdout) == 0);
    uint64_t deadline = monotonic_ms() + DEADLINE_MS;
    uint64_t drain_until = 0;
    struct mrc_ctl_ev_event events[EVENT_CAPACITY];
    int count = 0;
    for (;;) {
        uint64_t now = monotonic_ms();
        if (drain_until && now >= drain_until)
            break;
        assert(now < deadline);
        if (!drain_until && access(finished, F_OK) == 0)
            drain_until = now + DRAIN_MS;
        int ready = poll(&wait, 1, 20);
        if (ready < 0 && errno == EINTR)
            continue;
        assert(ready >= 0);
        if (!ready)
            continue;
        assert(wait.revents == POLLIN);
        struct mrc_cq *source;
        void *context;
        assert(mrc_get_cq_event(channel, &source, &context) == 0);
        assert(source == cq && context == &device);
        mrc_ack_cq_events(cq, 1);
        assert(mrc_req_notify_cq(cq, 0) == 0);
        assert(count < EVENT_CAPACITY);
        int received = mrc_ctl_poll_ev_event(cq, EVENT_CAPACITY - count, events + count);
        assert(received >= 0 && received <= EVENT_CAPACITY - count);
        count += received;
    }
    assert(mrc_destroy_cq(cq) == 0);
    assert(mrc_destroy_comp_channel(channel) == 0);

    fputc('[', stdout);
    for (int i = 0; i < count; i++) {
        const struct mrc_ctl_ev_event *event = &events[i];
        assert(event->profile_id == EVENT_PROFILE_ID);
        assert(event->state == MRC_CTL_EV_ASSUMED_BAD || event->state == MRC_CTL_EV_GOOD);
        printf("%s{\"state\":\"%s\",\"port\":%u,\"value\":%" PRIu32 ",\"drop_count\":%" PRIu32 "}",
               i ? "," : "", event->state == MRC_CTL_EV_ASSUMED_BAD ? "bad" : "good",
               event->ev.port, ev_value(&event->ev), event->drop_count);
    }
    puts("]");
}

int main(int argc, char **argv)
{
    bool expect_no_cap = argc == 2 && strcmp(argv[1], "--expect-no-cap") == 0;
    bool seed = argc == 3 && strcmp(argv[1], "--seed-denied") == 0;
    bool unblock = argc == 3 && strcmp(argv[1], "--unblock") == 0;
    bool seed_cc = argc == 3 && strcmp(argv[1], "--seed-cc") == 0;
    bool seed_qa = argc == 3 && strcmp(argv[1], "--seed-qa") == 0;
    bool seed_srv6 = argc == 3 && strcmp(argv[1], "--seed-explicit-srv6") == 0;
    bool unblock_srv6 = argc == 3 && strcmp(argv[1], "--unblock-explicit-srv6") == 0;
    bool seed_srv6_srh = argc == 3 && strcmp(argv[1], "--seed-generated-srv6-srh") == 0;
    bool watch_events = argc == 3 && strcmp(argv[1], "--watch-ev-events") == 0;
    bool watch_bad = argc == 3 && strcmp(argv[1], "--watch-ev-bad") == 0;
    const struct generated_profile *generated = NULL;
    if (argc == 3 && strcmp(argv[1], "--seed-gen") == 0)
        generated = &generated_udp;
    else if (argc == 3 && strcmp(argv[1], "--seed-gen-stev") == 0)
        generated = &generated_stev;
    else if (argc == 3 && strcmp(argv[1], "--seed-gen-custom-stev") == 0)
        generated = &generated_custom_stev;
    bool fixture = seed || unblock || seed_cc || seed_qa || generated || seed_srv6 || unblock_srv6 ||
                   seed_srv6_srh || watch_events || watch_bad;
    if (argc != 1 && !expect_no_cap && !fixture) {
        fprintf(stderr, "Usage: %s [--expect-no-cap | --seed-denied DIR | --unblock DIR | "
                        "--seed-cc DIR | --seed-qa DIR | --seed-gen DIR | --seed-gen-stev DIR | --seed-gen-custom-stev DIR | "
                        "--seed-explicit-srv6 DIR | --unblock-explicit-srv6 DIR | --seed-generated-srv6-srh DIR | "
                        "--watch-ev-events DIR | --watch-ev-bad DIR]\n",
                        argv[0]);
        return EXIT_FAILURE;
    }
    struct ibv_context *verbs = open_supported_device();
    if (!verbs) {
        fputs("The MRC controller ABI test needs an accessible Ethernet verbs device.\n", stderr);
        return EXIT_FAILURE;
    }
    if (fixture) {
        struct mrc_context *controller = open_store(verbs, argv[2]);
        if (watch_events || watch_bad) {
            watch_ev_events(controller, argv[2], watch_bad);
        } else if (seed) {
            seed_denied_profile(controller);
        } else if (unblock) {
            assert(set_profile_state(controller, PROFILE_ID, MRC_CTL_PROFILE_OFFLINE) == -1 && errno == EBUSY);
            set_ev_state(controller, PROFILE_ID, ev(2, SECOND_EV), MRC_CTL_EV_GOOD);
        } else if (seed_srv6) {
            prepare_explicit_srv6(controller, MRC_CTL_EV_DENIED);
            printf("%u\n", (unsigned int)EXP_SRV6_PROFILE_ID);
        } else if (unblock_srv6) {
            unblock_explicit_srv6(controller);
        } else if (seed_srv6_srh) {
            prepare_generated_srv6(controller, true);
            printf("%u\n", (unsigned int)GEN_SRV6_SRH_PROFILE_ID);
        } else if (generated) {
            if (generated == &generated_custom_stev)
                prepare_custom_stev_format(controller);
            assert(configure_generated(controller, *generated) == 0);
            assert(set_profile_state(controller, generated->id, MRC_CTL_PROFILE_ONLINE) == 0);
            printf("%u\n", generated->id);
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
        assert(configure_explicit(controller, PROFILE_ID, MRC_DPDK_EV_FORMAT_UDP) == -1 && errno == EPERM);
        assert(profile_state(controller, PROFILE_ID) == MRC_CTL_PROFILE_INIT);
        assert(configure_generated(controller, generated_udp) == -1 && errno == EPERM);
        assert(profile_state(controller, GEN_UDP_PROFILE_ID) == MRC_CTL_PROFILE_INIT);
        assert(configure_format(controller, CUSTOM_STEV_FORMAT_ID, MRC_CTL_EV_FMT_MODE_STEV) == -1 && errno == EPERM);
        struct mrc_ctl_ev_profile_attr events = { .ev_event_mask = MRC_CTL_EV_GOOD };
        assert(mrc_ctl_modify_ev_profile(controller, EVENT_PROFILE_ID, &events,
                                         MRC_CTL_EV_PROFILE_EVENT_MASK) == -1 && errno == EPERM);
        struct mrc_ctl_ep_req probe = {0};
        struct mrc_ctl_ep_rsp reply;
        int replies = 1;
        assert(mrc_ctl_ep_batch_send_wait(controller, 0, MRC_CTL_EP_OP_EV_PROBE, &probe, 1,
                                          1000000, &reply, &replies) == -1 && errno == EPERM);
        assert(replies == 0);
        struct mrc_ctl_cc_profile_attr cc = {0};
        assert(mrc_ctl_query_cc_profile(controller, CC_PROFILE_ID, &cc,
                                        MRC_CTL_CC_PROFILE_CUR_STATE) == -1 && errno == EPERM);
        assert(configure_cc_algorithm(controller, CC_PROFILE_ID, nscc_algorithm) == -1 && errno == EPERM);
        assert(mrc_destroy_context(controller) == 0);
        puts("Controller operations without CAP_NET_ADMIN were rejected");
    } else {
        test_profile_lifecycle(verbs, controller, directory);
        puts("OCP controller EV profiles, live EV state, persistence, and QP leases passed");
        test_event_masks(verbs, directory);
        puts("OCP controller EV event subscriptions, persistence, and ONLINE immutability passed");
        test_generated_profiles(verbs, directory);
        puts("OCP controller UDP and STEV generation, persistence, and QP leases passed");
        test_srv6_profiles(verbs, directory);
        puts("OCP controller explicit and generated SRv6 EVs, wire byte order, and persistence passed");
        test_custom_udp_format(verbs, directory);
        test_custom_stev_format(verbs, directory);
        test_custom_srv6_format(verbs, directory);
        puts("OCP controller custom UDP, STEV, and SRv6 geometry, lifecycle, generation, and persistence passed");
        test_cc_lifecycle(verbs, directory);
        puts("OCP controller CC profiles, persistence, and QP leases passed");
        test_qa_lifecycle(verbs, directory);
        puts("OCP controller NSCC Quick Adapt configuration and persistence passed");
    }

    remove_test_store(directory);
    assert(ibv_close_device(verbs) == 0);
    return EXIT_SUCCESS;
}
