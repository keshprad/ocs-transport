#include "mrc_abi.h"
#include "mrc_endpoint.h"

#include <errno.h>
#include <linux/capability.h>
#include <mrc_ctl.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
    EV_BATCH_LIMIT = 32,
    MODIFY_ATTRS = MRC_CTL_EV_PROFILE_STATE | MRC_CTL_EV_PROFILE_CUR_STATE |
        MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_FMT_ID | MRC_CTL_EV_PROFILE_COUNT |
        MRC_CTL_EV_PROFILE_MIN_ACTIVE | MRC_CTL_EV_PROFILE_EVENT_MASK |
        MRC_CTL_EV_PROFILE_EV_OP | MRC_CTL_EV_PROFILE_EV_FIELDS_OP,
    EV_GEOMETRY = MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_FMT_ID | MRC_CTL_EV_PROFILE_COUNT,
};

_Static_assert((int)MRC_CTL_PROFILE_INIT == (int)MRC_CTL_STORE_INIT &&
               (int)MRC_CTL_PROFILE_OFFLINE == (int)MRC_CTL_STORE_OFFLINE &&
               (int)MRC_CTL_PROFILE_ONLINE == (int)MRC_CTL_STORE_ONLINE,
               "Controller and persistent profile states must agree");

static const char nscc_algorithm[] = "uet-1.0-nscc";
static const char *cc_algorithms[] = { nscc_algorithm, NULL };

struct ev_change {
    struct mrc_ctl_ev_profile_attr *attr;
    unsigned int mask;
    int entry_error;
};

static int set_errno(int error)
{
    errno = error;
    return -1;
}

static bool controller_capable(void)
{
    struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3] = {{0}};
    if (syscall(SYS_capget, &header, data) != 0)
        return false;
    return (data[CAP_NET_ADMIN / 32].effective & (UINT32_C(1) << (CAP_NET_ADMIN % 32))) != 0;
}

MRC_PUBLIC int mrc_dpdk_set_profile_store(struct mrc_context *context, const char *directory)
{
    if (!context)
        return EINVAL;
    pthread_mutex_lock(&context->mutex);
    int error = -mrc_ctl_store_open(&context->profile_store, directory);
    pthread_mutex_unlock(&context->mutex);
    return error;
}

MRC_PUBLIC int mrc_dpdk_set_ev_store(struct mrc_context *context, const char *directory)
{
    return mrc_dpdk_set_profile_store(context, directory);
}

struct mrc_ctl_store mrc_abi_profile_store(struct mrc_context *context)
{
    pthread_mutex_lock(&context->mutex);
    struct mrc_ctl_store store = context->profile_store;
    pthread_mutex_unlock(&context->mutex);
    return store;
}

MRC_PUBLIC void mrc_ctl_query_version(uint32_t *current, uint32_t *minimum,
                                      uint32_t *vendor, uint32_t *vendor_minimum)
{
    if (current)
        *current = MRC_CTL_API_VER(1, 0, 1);
    if (minimum)
        *minimum = MRC_CTL_API_VER(1, 0, 0);
    if (vendor)
        *vendor = MRC_CTL_API_VER(1, 0, 0);
    if (vendor_minimum)
        *vendor_minimum = MRC_CTL_API_VER(1, 0, 0);
}

MRC_PUBLIC int mrc_ctl_query_device_ex(struct ibv_context *verbs, struct mrc_context *context,
                                       struct mrc_ctl_device_attr *out, int *supported)
{
    if (!out || !supported)
        return set_errno(EINVAL);
    struct mrc_device_attr device;
    if (mrc_query_device_ex(verbs, context, &device, supported))
        return -1;
    *out = (struct mrc_ctl_device_attr){0};
    if (!*supported)
        return 0;
    if (!verbs)
        verbs = context->verbs;

    struct ibv_device_attr limits;
    int error = ibv_query_device(verbs, &limits);
    if (error)
        return error > 0 ? set_errno(error) : -1;
    uint32_t ports = 0;
    for (unsigned int port = 1; port < 32 && port <= limits.phys_port_cnt; port++) {
        struct ibv_port_attr attr;
        if (!ibv_query_port(verbs, (uint8_t)port, &attr) && attr.link_layer == IBV_LINK_LAYER_ETHERNET)
            ports |= UINT32_C(1) << port;
    }

    size_t used = 0;
    if (context) {
        struct mrc_ctl_store store = mrc_abi_profile_store(context);
        if (store.fd >= 0) {
            struct mrc_ctl_store_profile profiles[MRC_CTL_STORE_MAX_PROFILES];
            size_t count;
            error = mrc_ctl_store_list(&store, profiles, MRC_CTL_STORE_MAX_PROFILES, &count);
            if (error)
                return set_errno(-error);
            for (size_t i = 0; i < count; i++)
                used += profiles[i].config.count;
        }
    }
    const unsigned int budget = MRC_CTL_STORE_MAX_PROFILES * PATH_COUNT;
    *out = (struct mrc_ctl_device_attr){
        .mrc_ctl_protocol_version = MRC_CTL_PROTOCOL_VERSION_1,
        .ev = {
            .ev_max_fmt_profiles = 2, .ev_max_profiles = MRC_CTL_STORE_MAX_PROFILES,
            .ev_max_count = budget, .ev_free_count = used < budget ? budget - used : 0,
            .ev_max_count_profile = PATH_COUNT, .ev_count_align = 1, .ev_max_width = 32,
            .ev_fmt_mode_mask = MRC_CTL_EV_FMT_MODE_UDP | MRC_CTL_EV_FMT_MODE_STEV,
            .ev_mode_mask = MRC_CTL_EV_MODE_AUTO | MRC_CTL_EV_MODE_EXP,
            .ev_op_max_entries = EV_BATCH_LIMIT,
        },
        .cc = { .cc_max_profiles = MRC_CTL_STORE_MAX_CC_PROFILES, .algorithms = cc_algorithms },
        .phy_port_mask = ports,
    };
    return 0;
}

MRC_PUBLIC int mrc_ctl_query_device(struct ibv_context *verbs, struct mrc_ctl_device_attr *out)
{
    int supported;
    int result = mrc_ctl_query_device_ex(verbs, NULL, out, &supported);
    return result || supported ? result : set_errno(EOPNOTSUPP);
}

static enum mrc_ctl_store_format format_from_id(uint64_t id)
{
    if (id == MRC_DPDK_EV_FORMAT_UDP)
        return MRC_CTL_STORE_FORMAT_UDP;
    if (id == MRC_DPDK_EV_FORMAT_STEV)
        return MRC_CTL_STORE_FORMAT_STEV;
    return MRC_CTL_STORE_FORMAT_LEGACY;
}

static enum mrc_ctl_store_format configured_format(const struct mrc_ctl_store_config *config)
{
    if (config->format != MRC_CTL_STORE_FORMAT_LEGACY)
        return config->format;
    for (unsigned int i = 0; i < config->count; i++) {
        if (config->entropy[i] > UINT16_MAX)
            return MRC_CTL_STORE_FORMAT_STEV;
    }
    return MRC_CTL_STORE_FORMAT_UDP;
}

static unsigned int format_field_count(enum mrc_ctl_store_format format)
{
    return format == MRC_CTL_STORE_FORMAT_STEV ? 2 : 1;
}

static unsigned int profile_field_count(const struct mrc_ctl_store_config *config)
{
    if (!config->mode || (config->mode == MRC_CTL_STORE_AUTO &&
                          config->format == MRC_CTL_STORE_FORMAT_LEGACY))
        return 0;
    return format_field_count(configured_format(config));
}

MRC_PUBLIC int mrc_ctl_query_ev_fmt_profile(struct mrc_context *context, uint64_t id,
                                            struct mrc_ctl_ev_fmt_profile_attr *attr, int mask)
{
    const unsigned int allowed = MRC_CTL_EV_FMT_PROFILE_STATE | MRC_CTL_EV_FMT_PROFILE_CUR_STATE |
        MRC_CTL_EV_FMT_PROFILE_MODE | MRC_CTL_EV_FMT_PROFILE_OP;
    if (!context || !attr || ((unsigned int)mask & ~allowed))
        return set_errno(EINVAL);
    enum mrc_ctl_store_format format = format_from_id(id);
    if (format == MRC_CTL_STORE_FORMAT_LEGACY)
        return set_errno(ENOENT);
    if (mask & MRC_CTL_EV_FMT_PROFILE_STATE)
        attr->profile_state = MRC_CTL_PROFILE_ONLINE;
    if (mask & MRC_CTL_EV_FMT_PROFILE_CUR_STATE)
        attr->cur_profile_state = MRC_CTL_PROFILE_ONLINE;
    if (mask & MRC_CTL_EV_FMT_PROFILE_MODE)
        attr->ev_fmt_mode = format == MRC_CTL_STORE_FORMAT_STEV ?
            MRC_CTL_EV_FMT_MODE_STEV : MRC_CTL_EV_FMT_MODE_UDP;
    if (mask & MRC_CTL_EV_FMT_PROFILE_OP) {
        if (attr->ev_fmt_op.op != MRC_CTL_EV_FMT_OP_QUERY_FIELDS || attr->ev_fmt_op.fmt_field_count < 0)
            return set_errno(EINVAL);
        int capacity = attr->ev_fmt_op.fmt_field_count;
        int count = (int)format_field_count(format);
        attr->ev_fmt_op.fmt_field_count = count;
        if (capacity < count)
            return set_errno(E2BIG);
        if (!attr->ev_fmt_op.fmt_fields)
            return set_errno(EINVAL);
        for (int i = 0; i < count; i++)
            attr->ev_fmt_op.fmt_fields[i] = (struct mrc_ctl_ev_fmt_field){ .width = 16 };
    }
    return 0;
}

static uint32_t ev_value(const struct mrc_ctl_store_config *config, unsigned int path)
{
    if (config->mode == MRC_CTL_STORE_AUTO)
        return mrc_endpoint_default_entropy(configured_format(config) == MRC_CTL_STORE_FORMAT_STEV, path);
    return config->entropy[path];
}

static int unpack_ev(const struct mrc_ctl_ev *ev, uint32_t *value)
{
    for (unsigned int i = 4; i < sizeof(ev->val); i++) {
        if (ev->val[i])
            return -EINVAL;
    }
    *value = (uint32_t)ev->val[0] << 24 | (uint32_t)ev->val[1] << 16 |
             (uint32_t)ev->val[2] << 8 | ev->val[3];
    return 0;
}

static struct mrc_ctl_ev pack_ev(unsigned int path, uint32_t value)
{
    return (struct mrc_ctl_ev){
        .val = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value },
        .port = (uint8_t)(path + 1),
    };
}

static int ev_path(const struct mrc_ctl_store_config *config, const struct mrc_ctl_ev *ev)
{
    uint32_t value;
    int error = unpack_ev(ev, &value);
    if (error)
        return error;
    if (!ev->port || ev->port > config->count)
        return -EINVAL;
    unsigned int path = ev->port - 1;
    return ev_value(config, path) == value ? (int)path : -ENOENT;
}

static enum mrc_ctl_ev_state ev_state(const struct mrc_ctl_store_config *config, unsigned int path)
{
    uint32_t bit = UINT32_C(1) << path;
    if (config->mode != MRC_CTL_STORE_AUTO && !(config->populated & bit))
        return MRC_CTL_EV_UNKNOWN;
    return config->denied & bit ? MRC_CTL_EV_DENIED : MRC_CTL_EV_GOOD;
}

static int modify_ev(struct mrc_ctl_store_config *config, enum mrc_ctl_ev_op op,
                     const struct mrc_ctl_ev_op_entry *entry)
{
    if ((entry->state != MRC_CTL_EV_GOOD && entry->state != MRC_CTL_EV_DENIED) ||
        (entry->flags & ~MRC_CTL_EV_OP_F_ALL_COPIES))
        return -EINVAL;
    int path = ev_path(config, &entry->ev);
    if (path < 0)
        return path;
    uint32_t bit = UINT32_C(1) << path;
    if (op == MRC_CTL_EV_OP_REPLACE_EV) {
        if (config->mode != MRC_CTL_STORE_EXPLICIT)
            return -EOPNOTSUPP;
        uint32_t value;
        if (entry->ev_new.port != entry->ev.port || unpack_ev(&entry->ev_new, &value) ||
            !(uint16_t)value || (configured_format(config) == MRC_CTL_STORE_FORMAT_UDP && value > UINT16_MAX))
            return -EINVAL;
        for (unsigned int i = 0; i < config->count; i++) {
            if ((int)i != path && config->entropy[i] == value)
                return -EEXIST;
        }
        config->entropy[path] = value;
        config->populated |= bit;
    } else if (config->mode != MRC_CTL_STORE_AUTO && !(config->populated & bit)) {
        return -ENOENT;
    }
    if (entry->state == MRC_CTL_EV_DENIED)
        config->denied |= bit;
    else
        config->denied &= ~bit;
    return 0;
}

static bool valid_transition(enum mrc_ctl_store_state from, enum mrc_ctl_store_state to)
{
    switch (from) {
    case MRC_CTL_STORE_INIT:
        return to == MRC_CTL_STORE_INIT || to == MRC_CTL_STORE_OFFLINE;
    case MRC_CTL_STORE_OFFLINE:
        return to == MRC_CTL_STORE_INIT || to == MRC_CTL_STORE_OFFLINE || to == MRC_CTL_STORE_ONLINE;
    case MRC_CTL_STORE_ONLINE:
        return to == MRC_CTL_STORE_OFFLINE || to == MRC_CTL_STORE_ONLINE;
    }
    return false;
}

static int configure_profile(struct mrc_ctl_store_config *config, const struct ev_change *change)
{
    const struct mrc_ctl_ev_profile_attr *attr = change->attr;
    unsigned int mask = change->mask;
    enum mrc_ctl_store_mode mode = config->mode;
    enum mrc_ctl_store_format format = config->format;
    if (mask & MRC_CTL_EV_PROFILE_MODE) {
        if (attr->ev_mode == MRC_CTL_EV_MODE_AUTO)
            mode = MRC_CTL_STORE_AUTO;
        else if (attr->ev_mode == MRC_CTL_EV_MODE_EXP)
            mode = MRC_CTL_STORE_EXPLICIT;
        else
            return -EOPNOTSUPP;
    }
    if (mask & MRC_CTL_EV_PROFILE_FMT_ID) {
        format = format_from_id(attr->ev_fmt_profile_id);
        if (format == MRC_CTL_STORE_FORMAT_LEGACY && (attr->ev_fmt_profile_id || mode != MRC_CTL_STORE_AUTO))
            return -ENOENT;
    } else if (mode != config->mode) {
        if (mode == MRC_CTL_STORE_AUTO)
            format = MRC_CTL_STORE_FORMAT_LEGACY;
        else if (format == MRC_CTL_STORE_FORMAT_LEGACY)
            return -EINVAL;
    }
    if (mask & MRC_CTL_EV_PROFILE_COUNT) {
        if (attr->ev_count != PATH_COUNT)
            return -EINVAL;
        config->count = (uint8_t)attr->ev_count;
    }
    if (mode != config->mode || format != config->format) {
        config->mode = mode;
        config->format = format;
        config->populated = 0;
        config->denied = 0;
        memset(config->entropy, 0, sizeof(config->entropy));
    }
    if (mask & MRC_CTL_EV_PROFILE_MIN_ACTIVE) {
        if (attr->ev_min_active > config->count)
            return -EINVAL;
        config->min_active = (uint8_t)attr->ev_min_active;
    }
    if ((mask & MRC_CTL_EV_PROFILE_EVENT_MASK) && attr->ev_event_mask)
        return -EOPNOTSUPP;
    if (mask & MRC_CTL_EV_PROFILE_EV_FIELDS_OP) {
        int count = (int)profile_field_count(config);
        if (attr->ev_fields_op.op != MRC_CTL_EV_FIELDS_OP_MODIFY_FIELDS ||
            attr->ev_fields_op.field_count < 0)
            return -EINVAL;
        if (attr->ev_fields_op.field_count > count)
            return -E2BIG;
        if (attr->ev_fields_op.field_count != count || (count && !attr->ev_fields_op.fields))
            return -EINVAL;
        for (int i = 0; i < count; i++) {
            const struct mrc_ctl_ev_field *field = &attr->ev_fields_op.fields[i];
            if (field->width != 16 || field->init_val || field->min_val || field->max_val ||
                field->mask != UINT16_MAX)
                return -EOPNOTSUPP;
        }
    }
    return 0;
}

static int edit_profile(struct mrc_ctl_store_profile *profile, void *argument)
{
    struct ev_change *change = argument;
    struct mrc_ctl_ev_profile_attr *attr = change->attr;
    unsigned int mask = change->mask;
    enum mrc_ctl_store_state from = profile->state;
    enum mrc_ctl_store_state to = from;
    if (mask & MRC_CTL_EV_PROFILE_STATE) {
        if ((unsigned int)attr->profile_state > MRC_CTL_PROFILE_ONLINE)
            return -EINVAL;
        to = (enum mrc_ctl_store_state)attr->profile_state;
    }
    if ((mask & MRC_CTL_EV_PROFILE_CUR_STATE) && (int)attr->cur_profile_state != (int)from)
        return -EINVAL;
    if (!valid_transition(from, to))
        return -EINVAL;
    if (to == MRC_CTL_STORE_INIT) {
        if (mask & ~(MRC_CTL_EV_PROFILE_STATE | MRC_CTL_EV_PROFILE_CUR_STATE))
            return -EINVAL;
        *profile = (struct mrc_ctl_store_profile){0};
        return 0;
    }
    const unsigned int initial = MRC_CTL_EV_PROFILE_MODE | MRC_CTL_EV_PROFILE_COUNT;
    if (from == MRC_CTL_STORE_INIT &&
        ((mask & initial) != initial ||
         (attr->ev_mode == MRC_CTL_EV_MODE_EXP && !(mask & MRC_CTL_EV_PROFILE_FMT_ID))))
        return -EINVAL;
    if (from == MRC_CTL_STORE_ONLINE &&
        (mask & ~(MRC_CTL_EV_PROFILE_STATE | MRC_CTL_EV_PROFILE_CUR_STATE | MRC_CTL_EV_PROFILE_EV_OP)))
        return -EBUSY;

    uint8_t old_min_active = profile->config.min_active;
    int error = configure_profile(&profile->config, change);
    if (error)
        return error;
    if (mask & MRC_CTL_EV_PROFILE_EV_OP) {
        if (attr->ev_op.op != MRC_CTL_EV_OP_MODIFY_EV_STATE && attr->ev_op.op != MRC_CTL_EV_OP_REPLACE_EV)
            return -EINVAL;
        if (attr->ev_op.op == MRC_CTL_EV_OP_REPLACE_EV && from != MRC_CTL_STORE_OFFLINE)
            return -EBUSY;
        if (attr->ev_op.entry_count < 0 || (attr->ev_op.entry_count && !attr->ev_op.entries))
            return -EINVAL;
        if (attr->ev_op.entry_count > EV_BATCH_LIMIT)
            return -E2BIG;
        for (int i = 0; i < attr->ev_op.entry_count; i++) {
            error = modify_ev(&profile->config, attr->ev_op.op, &attr->ev_op.entries[i]);
            if (error) {
                if (!i)
                    return error;
                profile->config.min_active = old_min_active;
                change->entry_error = error;
                return 0;
            }
        }
    }
    unsigned int count = profile->config.count;
    uint32_t required = count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1;
    if (to == MRC_CTL_STORE_ONLINE && profile->config.mode != MRC_CTL_STORE_AUTO &&
        profile->config.populated != required)
        return -EINVAL;
    profile->state = to;
    return 0;
}

static bool valid_mask(unsigned int mask)
{
    const unsigned int ops = MRC_CTL_EV_PROFILE_EV_OP | MRC_CTL_EV_PROFILE_EV_FIELDS_OP;
    return !(mask & ~MODIFY_ATTRS) && (mask & ops) != ops;
}

MRC_PUBLIC int mrc_ctl_modify_ev_profile(struct mrc_context *context, uint64_t id,
                                         struct mrc_ctl_ev_profile_attr *attr, int mask)
{
    if (!context || !id || !attr || !valid_mask(mask) ||
        ((mask & MRC_CTL_EV_PROFILE_EV_OP) && (mask & EV_GEOMETRY)))
        return set_errno(EINVAL);
    if (!controller_capable())
        return set_errno(EPERM);
    struct mrc_ctl_store store = mrc_abi_profile_store(context);
    if (store.fd < 0)
        return set_errno(ENODEV);
    struct ev_change change = { .attr = attr, .mask = (unsigned int)mask };
    int error = mrc_ctl_store_edit(&store, id, edit_profile, &change);
    if (!error)
        error = change.entry_error;
    return error ? set_errno(-error) : 0;
}

static int query_entries(const struct mrc_ctl_store_config *config, struct mrc_ctl_ev_profile_attr *attr)
{
    int count = attr->ev_op.entry_count;
    if (count < 0)
        return -EINVAL;
    switch (attr->ev_op.op) {
    case MRC_CTL_EV_OP_QUERY_EV_ARRAY:
        attr->ev_op.entry_count = config->count;
        if (count < config->count)
            return -E2BIG;
        if (config->count && !attr->ev_op.entries)
            return -EINVAL;
        for (unsigned int i = 0; i < config->count; i++) {
            attr->ev_op.entries[i] = (struct mrc_ctl_ev_op_entry){
                .ev = pack_ev(i, ev_value(config, i)), .state = ev_state(config, i),
            };
        }
        return 0;
    case MRC_CTL_EV_OP_QUERY_EV_STATE:
        if (count && !attr->ev_op.entries)
            return -EINVAL;
        if (count > EV_BATCH_LIMIT)
            return -E2BIG;
        for (int i = 0; i < count; i++) {
            int path = ev_path(config, &attr->ev_op.entries[i].ev);
            if (path < 0)
                return path;
            attr->ev_op.entries[i].state = ev_state(config, (unsigned int)path);
        }
        return 0;
    default:
        return -EINVAL;
    }
}

MRC_PUBLIC int mrc_ctl_query_ev_profile(struct mrc_context *context, uint64_t id,
                                        struct mrc_ctl_ev_profile_attr *attr, int mask)
{
    if (!context || !id || !attr || !valid_mask(mask))
        return set_errno(EINVAL);
    struct mrc_ctl_store store = mrc_abi_profile_store(context);
    if (store.fd < 0)
        return set_errno(ENODEV);
    struct mrc_ctl_store_profile profile = {0};
    int error = mrc_ctl_store_get(&store, id, &profile);
    if (error && error != -ENOENT)
        return set_errno(-error);

    const struct mrc_ctl_store_config *config = &profile.config;
    enum mrc_ctl_store_format format = configured_format(config);
    if (mask & MRC_CTL_EV_PROFILE_STATE)
        attr->profile_state = (enum mrc_ctl_profile_state)profile.state;
    if (mask & MRC_CTL_EV_PROFILE_CUR_STATE)
        attr->cur_profile_state = (enum mrc_ctl_profile_state)profile.state;
    if (mask & MRC_CTL_EV_PROFILE_MODE) {
        switch (config->mode) {
        case MRC_CTL_STORE_AUTO:
            attr->ev_mode = MRC_CTL_EV_MODE_AUTO;
            break;
        case MRC_CTL_STORE_EXPLICIT:
            attr->ev_mode = MRC_CTL_EV_MODE_EXP;
            break;
        case MRC_CTL_STORE_GENERATED:
            attr->ev_mode = MRC_CTL_EV_MODE_GEN;
            break;
        default:
            attr->ev_mode = 0;
            break;
        }
    }
    if (mask & MRC_CTL_EV_PROFILE_FMT_ID)
        attr->ev_fmt_profile_id = !profile_field_count(config) ? 0 :
            format == MRC_CTL_STORE_FORMAT_STEV ? MRC_DPDK_EV_FORMAT_STEV : MRC_DPDK_EV_FORMAT_UDP;
    if (mask & MRC_CTL_EV_PROFILE_COUNT)
        attr->ev_count = config->count;
    if (mask & MRC_CTL_EV_PROFILE_MIN_ACTIVE)
        attr->ev_min_active = config->min_active;
    if (mask & MRC_CTL_EV_PROFILE_EVENT_MASK)
        attr->ev_event_mask = 0;
    if (mask & MRC_CTL_EV_PROFILE_EV_OP)
        error = query_entries(config, attr);
    else
        error = 0;
    if (mask & MRC_CTL_EV_PROFILE_EV_FIELDS_OP) {
        int capacity = attr->ev_fields_op.field_count;
        if (attr->ev_fields_op.op != MRC_CTL_EV_FIELDS_OP_QUERY_FIELDS || capacity < 0)
            return set_errno(EINVAL);
        int count = (int)profile_field_count(config);
        attr->ev_fields_op.field_count = count;
        if (capacity < count)
            return set_errno(E2BIG);
        if (count && !attr->ev_fields_op.fields)
            return set_errno(EINVAL);
        for (int i = 0; i < count; i++)
            attr->ev_fields_op.fields[i] = (struct mrc_ctl_ev_field){ .width = 16, .mask = UINT16_MAX };
    }
    return error ? set_errno(-error) : 0;
}

enum {
    CC_STATE_ATTRS = MRC_CTL_CC_PROFILE_STATE | MRC_CTL_CC_PROFILE_CUR_STATE,
    CC_ATTRS = CC_STATE_ATTRS | MRC_CTL_CC_PROFILE_ALGORITHM | MRC_CTL_CC_PROFILE_CONFIG,
};

struct cc_change {
    const struct mrc_ctl_cc_profile_attr *attr;
    unsigned int mask;
};

/* Keep the two representations independent so the store stays usable
 * without OCP headers, including when the public structures change. */
#define COPY_NSCC_FIELDS(src) \
    .disable_qa = (src)->disable_qa, \
    .adjust_bytes_threshold = (src)->adjust_bytes_threshold, \
    .adjust_period_threshold = (src)->adjust_period_threshold, \
    .base_rtt = (src)->base_rtt, \
    .eta = (src)->eta, .fi = (src)->fi, .fi_scale = (src)->fi_scale, \
    .gamma = (src)->gamma, .max_md_jump = (src)->max_md_jump, \
    .max_cwnd = (src)->max_cwnd, .qa_gate = (src)->qa_gate, \
    .qa_threshold = (src)->qa_threshold, .target_delay = (src)->target_delay

static int configure_cc(struct mrc_ctl_store_cc_profile *profile,
                         const struct mrc_ctl_cc_nscc_cfg *config)
{
    if (profile->algorithm == MRC_CC_DISABLED)
        return config ? -EINVAL : 0;
    if (!config)
        return -EINVAL;
    struct mrc_cc_config checked = { COPY_NSCC_FIELDS(config) };
    if (!mrc_cc_config_valid(&checked, MRC_NOMINAL_MTU))
        return -EINVAL;
    profile->config = checked;
    profile->configured = true;
    return 0;
}

static int edit_cc_profile(struct mrc_ctl_store_cc_profile *profile, void *argument)
{
    const struct cc_change *change = argument;
    const struct mrc_ctl_cc_profile_attr *attr = change->attr;
    const unsigned int mask = change->mask;
    const enum mrc_ctl_store_state from = profile->state;
    enum mrc_ctl_store_state to = from;
    if (mask & MRC_CTL_CC_PROFILE_STATE) {
        if ((unsigned int)attr->profile_state > MRC_CTL_PROFILE_ONLINE)
            return -EINVAL;
        to = (enum mrc_ctl_store_state)attr->profile_state;
    }
    if (!valid_transition(from, to) ||
        ((mask & MRC_CTL_CC_PROFILE_CUR_STATE) && (int)attr->cur_profile_state != (int)from))
        return -EINVAL;
    if (to == MRC_CTL_STORE_INIT) {
        if (mask & ~CC_STATE_ATTRS)
            return -EINVAL;
        *profile = (struct mrc_ctl_store_cc_profile){0};
        return 0;
    }
    if (from == MRC_CTL_STORE_ONLINE && (mask & ~CC_STATE_ATTRS))
        return -EBUSY;
    if (from == MRC_CTL_STORE_INIT && !(mask & MRC_CTL_CC_PROFILE_ALGORITHM))
        return -EINVAL;
    if (from == MRC_CTL_STORE_OFFLINE && to == MRC_CTL_STORE_ONLINE &&
        !(mask & MRC_CTL_CC_PROFILE_CONFIG))
        return -EINVAL;

    if (mask & MRC_CTL_CC_PROFILE_ALGORITHM) {
        enum mrc_cc_algorithm algorithm = MRC_CC_DISABLED;
        if (attr->algorithm) {
            if (strcmp(attr->algorithm, nscc_algorithm))
                return -EINVAL;
            algorithm = MRC_CC_NSCC;
        }
        if (profile->algorithm != algorithm) {
            profile->algorithm = algorithm;
            profile->configured = false;
            profile->config = (struct mrc_cc_config){0};
        }
    }
    if (mask & MRC_CTL_CC_PROFILE_CONFIG) {
        int error = configure_cc(profile, attr->cc_config);
        if (error)
            return error;
    }
    profile->state = to;
    return 0;
}

MRC_PUBLIC int mrc_ctl_modify_cc_profile(struct mrc_context *context, uint64_t id,
                                         struct mrc_ctl_cc_profile_attr *attr, int mask)
{
    if (!context || !id || !attr || ((unsigned int)mask & ~CC_ATTRS))
        return set_errno(EINVAL);
    if (!controller_capable())
        return set_errno(EPERM);
    struct mrc_ctl_store store = mrc_abi_profile_store(context);
    if (store.fd < 0)
        return set_errno(ENODEV);
    struct cc_change change = { .attr = attr, .mask = (unsigned int)mask };
    int error = mrc_ctl_store_edit_cc(&store, id, edit_cc_profile, &change);
    return error ? set_errno(-error) : 0;
}

MRC_PUBLIC int mrc_ctl_query_cc_profile(struct mrc_context *context, uint64_t id,
                                        struct mrc_ctl_cc_profile_attr *attr, int mask)
{
    if (!context || !id || !attr || ((unsigned int)mask & ~CC_ATTRS))
        return set_errno(EINVAL);
    if (!controller_capable())
        return set_errno(EPERM);
    struct mrc_ctl_store store = mrc_abi_profile_store(context);
    if (store.fd < 0)
        return set_errno(ENODEV);
    struct mrc_ctl_store_cc_profile profile = {0};
    int error = mrc_ctl_store_get_cc(&store, id, &profile);
    if (error && error != -ENOENT)
        return set_errno(-error);

    if (mask & MRC_CTL_CC_PROFILE_STATE)
        attr->profile_state = (enum mrc_ctl_profile_state)profile.state;
    if (mask & MRC_CTL_CC_PROFILE_CUR_STATE)
        attr->cur_profile_state = (enum mrc_ctl_profile_state)profile.state;
    if (mask & MRC_CTL_CC_PROFILE_ALGORITHM)
        attr->algorithm = profile.algorithm == MRC_CC_NSCC ? nscc_algorithm : NULL;
    if (mask & MRC_CTL_CC_PROFILE_CONFIG) {
        static _Thread_local struct mrc_ctl_cc_nscc_cfg config;
        const struct mrc_cc_config *stored = &profile.config;
        config = (struct mrc_ctl_cc_nscc_cfg){ COPY_NSCC_FIELDS(stored) };
        attr->cc_config = profile.configured ? &config : NULL;
    }
    return 0;
}
