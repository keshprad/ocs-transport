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
    SRV6_SID_FIELDS = MRC_CTL_STORE_SRV6_SID_BYTES / 2,
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
_Static_assert(MRC_CTL_EV_MAX_BYTES == MRC_CTL_STORE_SRV6_EV_BYTES,
               "Controller and persistent SRv6 EV widths must agree");

static const char nscc_algorithm[] = "uet-1.0-nscc";
static const char *cc_algorithms[] = { nscc_algorithm, NULL };

struct ev_change {
    struct mrc_ctl_ev_profile_attr *attr;
    struct mrc_ctl_store_ev_format format;
    unsigned int mask;
    int format_error;
    int entry_error;
};

static int set_errno(int error)
{
    errno = error;
    return -1;
}

bool mrc_abi_controller_capable(void)
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
            .ev_max_fmt_profiles = MRC_CTL_STORE_BUILTIN_FORMATS + MRC_CTL_STORE_MAX_CUSTOM_FORMATS,
            .ev_max_profiles = MRC_CTL_STORE_MAX_PROFILES,
            .ev_max_count = budget, .ev_free_count = used < budget ? budget - used : 0,
            .ev_max_count_profile = PATH_COUNT, .ev_count_align = 1,
            .ev_max_width = MRC_CTL_STORE_SRV6_EV_BYTES * 8,
            .ev_fmt_mode_mask = MRC_CTL_EV_FMT_MODE_UDP | MRC_CTL_EV_FMT_MODE_STEV | MRC_CTL_EV_FMT_MODE_SRV6,
            .ev_mode_mask = MRC_CTL_EV_MODE_AUTO | MRC_CTL_EV_MODE_EXP | MRC_CTL_EV_MODE_GEN,
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

enum mrc_ctl_store_format mrc_abi_builtin_ev_format(uint64_t id)
{
    if (id == MRC_DPDK_EV_FORMAT_UDP)
        return MRC_CTL_STORE_FORMAT_UDP;
    if (id == MRC_DPDK_EV_FORMAT_STEV)
        return MRC_CTL_STORE_FORMAT_STEV;
    if (id == MRC_DPDK_EV_FORMAT_SRV6)
        return MRC_CTL_STORE_FORMAT_SRV6;
    if (id == MRC_DPDK_EV_FORMAT_SRV6_SRH)
        return MRC_CTL_STORE_FORMAT_SRV6_SRH;
    return MRC_CTL_STORE_FORMAT_LEGACY;
}

static uint64_t format_id(enum mrc_ctl_store_format format)
{
    switch (format) {
    case MRC_CTL_STORE_FORMAT_UDP:
        return MRC_DPDK_EV_FORMAT_UDP;
    case MRC_CTL_STORE_FORMAT_STEV:
        return MRC_DPDK_EV_FORMAT_STEV;
    case MRC_CTL_STORE_FORMAT_SRV6:
        return MRC_DPDK_EV_FORMAT_SRV6;
    case MRC_CTL_STORE_FORMAT_SRV6_SRH:
        return MRC_DPDK_EV_FORMAT_SRV6_SRH;
    default:
        return 0;
    }
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

unsigned int mrc_abi_builtin_ev_fields(enum mrc_ctl_store_format format)
{
    if (format == MRC_CTL_STORE_FORMAT_SRV6)
        return SRV6_SID_FIELDS;
    if (format == MRC_CTL_STORE_FORMAT_SRV6_SRH)
        return 2 * SRV6_SID_FIELDS;
    if (format == MRC_CTL_STORE_FORMAT_STEV)
        return 2;
    return format == MRC_CTL_STORE_FORMAT_UDP ? 1 : 0;
}

static unsigned int profile_field_count(const struct mrc_ctl_store_config *config)
{
    if (!config->mode || (config->mode == MRC_CTL_STORE_AUTO &&
                          config->format == MRC_CTL_STORE_FORMAT_LEGACY))
        return 0;
    return config->format_id ? config->format_field_count : mrc_abi_builtin_ev_fields(configured_format(config));
}

static unsigned int profile_field_width(const struct mrc_ctl_store_config *config, unsigned int index)
{
    return config->format_id ? config->format_widths[index] : 16;
}

static uint32_t bit_mask(unsigned int width)
{
    return (uint32_t)((UINT64_C(1) << width) - 1);
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

static struct mrc_ctl_ev pack_ev(const struct mrc_ctl_store_config *config, unsigned int path)
{
    struct mrc_ctl_ev ev = { .port = (uint8_t)(path + 1) };
    if (mrc_ctl_store_format_is_srv6(config->format))
        memcpy(ev.val, config->srv6[path], sizeof(ev.val));
    else {
        uint32_t value = ev_value(config, path);
        ev.val[0] = (uint8_t)(value >> 24);
        ev.val[1] = (uint8_t)(value >> 16);
        ev.val[2] = (uint8_t)(value >> 8);
        ev.val[3] = (uint8_t)value;
    }
    return ev;
}

static int ev_path(const struct mrc_ctl_store_config *config, const struct mrc_ctl_ev *ev)
{
    if (!ev->port || ev->port > config->count)
        return -EINVAL;
    unsigned int path = ev->port - 1;
    if (mrc_ctl_store_format_is_srv6(config->format)) {
        /* All-zero identifies an empty explicit slot. */
        const mrc_ctl_ev_t empty = {0};
        if (memcmp(ev->val, empty, sizeof(empty)) && !mrc_ctl_store_srv6_ev_valid(config->format, ev->val))
            return -EINVAL;
        return memcmp(config->srv6[path], ev->val, sizeof(ev->val)) ? -ENOENT : (int)path;
    }
    uint32_t value;
    int error = unpack_ev(ev, &value);
    if (error)
        return error;
    return ev_value(config, path) == value ? (int)path : -ENOENT;
}

static enum mrc_ctl_ev_state ev_state(const struct mrc_ctl_store_config *config, unsigned int path)
{
    uint32_t bit = UINT32_C(1) << path;
    if (config->mode != MRC_CTL_STORE_AUTO && !(config->populated & bit))
        return MRC_CTL_EV_UNKNOWN;
    return config->denied & bit ? MRC_CTL_EV_DENIED : MRC_CTL_EV_GOOD;
}

static int replace_ev(struct mrc_ctl_store_config *config, unsigned int path, const struct mrc_ctl_ev *ev)
{
    if (ev->port != path + 1)
        return -EINVAL;
    if (mrc_ctl_store_format_is_srv6(config->format)) {
        if (!mrc_ctl_store_srv6_ev_valid(config->format, ev->val))
            return -EINVAL;
        memcpy(config->srv6[path], ev->val, sizeof(ev->val));
        return 0;
    }

    uint32_t value;
    if (unpack_ev(ev, &value) || !(uint16_t)value ||
        (configured_format(config) == MRC_CTL_STORE_FORMAT_UDP && value > UINT16_MAX))
        return -EINVAL;
    for (unsigned int i = 0; i < config->count; i++) {
        if (i != path && config->entropy[i] == value)
            return -EEXIST;
    }
    config->entropy[path] = value;
    return 0;
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
        int error = replace_ev(config, (unsigned int)path, &entry->ev_new);
        if (error)
            return error;
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

static uint32_t expand_generated_field(const struct mrc_ctl_store_ev_field *field, uint32_t value)
{
    uint32_t expanded = field->init_val & ~field->mask;
    uint32_t mask = field->mask;
    while (mask) {
        uint32_t bit = mask & (0u - mask);
        if (value & 1u)
            expanded |= bit;
        value >>= 1;
        mask &= mask - 1;
    }
    return expanded;
}

struct ev_generator {
    const struct mrc_ctl_store_ev_field *fields;
    const uint8_t *widths;
    unsigned int count;
    uint32_t values[MRC_CTL_STORE_MAX_EV_FIELDS];
};

static void start_generator(struct ev_generator *generator)
{
    for (unsigned int i = 0; i < generator->count; i++)
        generator->values[i] = generator->fields[i].min_val;
}

static bool advance_generator(struct ev_generator *generator)
{
    for (unsigned int i = generator->count; i > 0; i--) {
        const struct mrc_ctl_store_ev_field *field = &generator->fields[i - 1];
        uint32_t *value = &generator->values[i - 1];
        if (*value < field->max_val) {
            (*value)++;
            return true;
        }
        *value = field->min_val;
    }
    return false;
}

static bool can_generate_udp_port(const struct ev_generator *generator)
{
    unsigned int bits = 16;
    for (unsigned int i = generator->count; i > 0 && bits; i--) {
        const struct mrc_ctl_store_ev_field *field = &generator->fields[i - 1];
        unsigned int width = generator->widths[i - 1];
        unsigned int port_bits = width < bits ? width : bits;
        uint32_t port_mask = bit_mask(port_bits);
        if ((expand_generated_field(field, field->min_val) & port_mask) ||
            (field->min_val < field->max_val && (field->mask & port_mask)))
            return true;
        bits -= port_bits;
    }
    return false;
}

static uint32_t encode_generated_entropy(const struct ev_generator *generator)
{
    uint64_t entropy = 0;
    for (unsigned int i = 0; i < generator->count; i++) {
        uint32_t value = expand_generated_field(&generator->fields[i], generator->values[i]);
        entropy = (entropy << generator->widths[i]) | value;
    }
    return (uint32_t)entropy;
}

static int generate_udp_evs(struct mrc_ctl_store_config *config, const uint8_t *widths, unsigned int count)
{
    struct ev_generator generator = { .fields = config->fields, .widths = widths, .count = count };
    if (!can_generate_udp_port(&generator))
        return -EINVAL;
    start_generator(&generator);

    /* The last field changes fastest. Advance in compact-value order and
     * skip the zero UDP suffix without scanning high-bit-only ranges. */
    unsigned int generated = 0;
    do {
        uint32_t entropy = encode_generated_entropy(&generator);
        if ((uint16_t)entropy)
            config->entropy[generated++] = entropy;
    } while (generated < config->count && advance_generator(&generator));

    return generated == config->count ? 0 : -EINVAL;
}

struct generated_sid {
    struct ev_generator generator;
    uint8_t bytes[MRC_CTL_STORE_SRV6_SID_BYTES];
};

static void encode_generated_sid(struct generated_sid *sid)
{
    const struct ev_generator *generator = &sid->generator;
    memset(sid->bytes, 0, sizeof(sid->bytes));
    unsigned int offset = 0;
    for (unsigned int i = 0; i < generator->count; i++) {
        uint32_t value = expand_generated_field(&generator->fields[i], generator->values[i]);
        for (unsigned int bit = generator->widths[i]; bit > 0; bit--, offset++)
            sid->bytes[offset / 8] |= (uint8_t)(((value >> (bit - 1)) & 1u) << (7 - offset % 8));
    }
}

static bool advance_generated_sid(struct generated_sid *sid)
{
    if (!advance_generator(&sid->generator))
        return false;
    encode_generated_sid(sid);
    /* The packed values increase monotonically. A multicast prefix
     * means the remaining range of this container is also multicast. */
    return sid->bytes[0] != 0xff;
}

static bool start_generated_sid(struct generated_sid *sid)
{
    const uint8_t zero[MRC_CTL_STORE_SRV6_SID_BYTES] = {0};
    start_generator(&sid->generator);
    encode_generated_sid(sid);
    if (!memcmp(sid->bytes, zero, sizeof(zero)))
        return advance_generated_sid(sid);
    return sid->bytes[0] != 0xff;
}

static int generate_srv6_evs(struct mrc_ctl_store_config *config, const uint8_t *widths, unsigned int count)
{
    const bool srh = config->format == MRC_CTL_STORE_FORMAT_SRV6_SRH;
    unsigned int initial_count = 0, bits = 0;
    while (initial_count < count && bits < MRC_CTL_STORE_SRV6_SID_BYTES * 8)
        bits += widths[initial_count++];
    struct generated_sid initial = {
        .generator = { .fields = config->fields, .widths = widths, .count = initial_count },
    };
    struct generated_sid segment = {
        .generator = { .fields = config->fields + initial_count, .widths = widths + initial_count,
                       .count = count - initial_count },
    };
    if (!start_generated_sid(&initial) || (srh && !start_generated_sid(&segment)))
        return -EINVAL;

    /* Enumerate valid containers independently. An impossible SRH field set
     * must not require scanning every possible first container. */
    const struct generated_sid first_segment = segment;
    for (unsigned int i = 0; i < config->count; i++) {
        if (i && (!srh || !advance_generated_sid(&segment))) {
            if (!advance_generated_sid(&initial))
                return -EINVAL;
            if (srh)
                segment = first_segment;
        }
        memcpy(config->srv6[i], initial.bytes, sizeof(initial.bytes));
        if (srh)
            memcpy(config->srv6[i] + sizeof(initial.bytes), segment.bytes, sizeof(segment.bytes));
    }
    return 0;
}

static int configure_generated(struct mrc_ctl_store_config *config,
                                const struct mrc_ctl_ev_field *fields, unsigned int count)
{
    if (config->format == MRC_CTL_STORE_FORMAT_LEGACY || config->count != PATH_COUNT || !count)
        return -EINVAL;

    unsigned int slots = profile_field_count(config);
    uint8_t widths[MRC_CTL_STORE_MAX_EV_FIELDS];
    for (unsigned int i = 0; i < slots; i++)
        widths[i] = (uint8_t)profile_field_width(config, i);

    memset(config->fields, 0, sizeof(config->fields));
    for (unsigned int i = 0; i < count; i++) {
        const struct mrc_ctl_ev_field *field = &fields[i];
        struct mrc_ctl_store_ev_field *stored = &config->fields[i];
        *stored = (struct mrc_ctl_store_ev_field){
            .width = field->width, .init_val = field->init_val, .min_val = field->min_val,
            .max_val = field->max_val, .mask = field->mask,
        };
        if (stored->width > widths[i] || !mrc_ctl_store_ev_field_valid(stored))
            return -EINVAL;
    }

    int error = mrc_ctl_store_format_is_srv6(config->format)
                ? generate_srv6_evs(config, widths, slots)
                : generate_udp_evs(config, widths, slots);
    if (error)
        return error;

    config->field_count = (uint8_t)count;
    config->populated = (UINT32_C(1) << config->count) - 1;
    config->denied = 0;
    return 0;
}

static int configure_profile(struct mrc_ctl_store_config *config, const struct ev_change *change)
{
    const struct mrc_ctl_ev_profile_attr *attr = change->attr;
    unsigned int mask = change->mask;
    enum mrc_ctl_store_mode mode = config->mode;
    struct mrc_ctl_store_ev_format layout = {
        .id = config->format_id, .format = config->format, .field_count = config->format_field_count,
    };
    memcpy(layout.widths, config->format_widths, sizeof(layout.widths));
    if (mask & MRC_CTL_EV_PROFILE_MODE) {
        if (attr->ev_mode == MRC_CTL_EV_MODE_AUTO)
            mode = MRC_CTL_STORE_AUTO;
        else if (attr->ev_mode == MRC_CTL_EV_MODE_EXP)
            mode = MRC_CTL_STORE_EXPLICIT;
        else if (attr->ev_mode == MRC_CTL_EV_MODE_GEN)
            mode = MRC_CTL_STORE_GENERATED;
        else
            return -EOPNOTSUPP;
    }
    if (mask & MRC_CTL_EV_PROFILE_FMT_ID) {
        if (change->format_error)
            return change->format_error;
        layout = change->format;
        if (layout.format == MRC_CTL_STORE_FORMAT_LEGACY &&
            (attr->ev_fmt_profile_id || mode != MRC_CTL_STORE_AUTO))
            return -ENOENT;
    } else if (mode != config->mode) {
        if (mode == MRC_CTL_STORE_AUTO)
            layout = (struct mrc_ctl_store_ev_format){0};
        else if (layout.format == MRC_CTL_STORE_FORMAT_LEGACY)
            return -EINVAL;
    }
    if (mode == MRC_CTL_STORE_AUTO && (layout.id || mrc_ctl_store_format_is_srv6(layout.format)))
        return -EINVAL;
    if (mask & MRC_CTL_EV_PROFILE_COUNT) {
        if (attr->ev_count != PATH_COUNT)
            return -EINVAL;
        config->count = (uint8_t)attr->ev_count;
    }
    if (mode != config->mode || layout.format != config->format || layout.id != config->format_id ||
        layout.field_count != config->format_field_count ||
        memcmp(layout.widths, config->format_widths, sizeof(layout.widths))) {
        config->mode = mode;
        config->format = layout.format;
        config->format_id = layout.id;
        config->format_field_count = layout.field_count;
        memcpy(config->format_widths, layout.widths, sizeof(config->format_widths));
        config->populated = 0;
        config->denied = 0;
        memset(config->entropy, 0, sizeof(config->entropy));
        memset(config->srv6, 0, sizeof(config->srv6));
        config->field_count = 0;
        memset(config->fields, 0, sizeof(config->fields));
    }
    if (mask & MRC_CTL_EV_PROFILE_MIN_ACTIVE) {
        if (attr->ev_min_active > config->count)
            return -EINVAL;
        config->min_active = (uint8_t)attr->ev_min_active;
    }
    if ((mask & MRC_CTL_EV_PROFILE_EVENT_MASK) && attr->ev_event_mask)
        return -EOPNOTSUPP;
    if (mask & MRC_CTL_EV_PROFILE_EV_FIELDS_OP) {
        int slots = (int)profile_field_count(config);
        int count = attr->ev_fields_op.field_count;
        if (attr->ev_fields_op.op != MRC_CTL_EV_FIELDS_OP_MODIFY_FIELDS || count < 0)
            return -EINVAL;
        if (count > slots)
            return -E2BIG;
        if ((count != slots && (!config->format_id || mode != MRC_CTL_STORE_GENERATED)) ||
            (count && !attr->ev_fields_op.fields))
            return -EINVAL;
        if (mode == MRC_CTL_STORE_GENERATED)
            return configure_generated(config, attr->ev_fields_op.fields, (unsigned int)count);
        for (int i = 0; i < count; i++) {
            const struct mrc_ctl_ev_field *field = &attr->ev_fields_op.fields[i];
            unsigned int width = profile_field_width(config, (unsigned int)i);
            if (field->width != width || field->init_val || field->min_val || field->max_val ||
                field->mask != bit_mask(width))
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
         ((attr->ev_mode == MRC_CTL_EV_MODE_EXP || attr->ev_mode == MRC_CTL_EV_MODE_GEN) &&
          !(mask & MRC_CTL_EV_PROFILE_FMT_ID))))
        return -EINVAL;
    if (from == MRC_CTL_STORE_INIT && attr->ev_mode == MRC_CTL_EV_MODE_GEN &&
        !(mask & MRC_CTL_EV_PROFILE_EV_FIELDS_OP))
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
    if (!mrc_abi_controller_capable())
        return set_errno(EPERM);
    struct mrc_ctl_store store = mrc_abi_profile_store(context);
    if (store.fd < 0)
        return set_errno(ENODEV);
    struct ev_change change = { .attr = attr, .mask = (unsigned int)mask };
    if (mask & MRC_CTL_EV_PROFILE_FMT_ID) {
        uint64_t format = attr->ev_fmt_profile_id;
        if (format <= MRC_CTL_STORE_BUILTIN_FORMATS) {
            change.format.format = mrc_abi_builtin_ev_format(format);
        } else {
            change.format_error = mrc_ctl_store_get_format(&store, format, &change.format);
            if (!change.format_error && change.format.state != MRC_CTL_STORE_ONLINE)
                change.format_error = -EINVAL;
        }
    }
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
                .ev = pack_ev(config, i), .state = ev_state(config, i),
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
    if (mask & MRC_CTL_EV_PROFILE_FMT_ID) {
        attr->ev_fmt_profile_id = config->format_id;
        if (!attr->ev_fmt_profile_id && profile_field_count(config))
            attr->ev_fmt_profile_id = format_id(format);
    }
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
        if (config->mode == MRC_CTL_STORE_GENERATED && (config->field_count || !config->populated))
            count = config->field_count;
        attr->ev_fields_op.field_count = count;
        if (capacity < count)
            return set_errno(E2BIG);
        if (count && !attr->ev_fields_op.fields)
            return set_errno(EINVAL);
        for (int i = 0; i < count; i++) {
            if (config->mode == MRC_CTL_STORE_GENERATED && config->field_count) {
                const struct mrc_ctl_store_ev_field *field = &config->fields[i];
                attr->ev_fields_op.fields[i] = (struct mrc_ctl_ev_field){
                    .width = field->width, .init_val = field->init_val, .min_val = field->min_val,
                    .max_val = field->max_val, .mask = field->mask,
                };
            } else {
                unsigned int width = profile_field_width(config, (unsigned int)i);
                attr->ev_fields_op.fields[i] = (struct mrc_ctl_ev_field){ .width = width, .mask = bit_mask(width) };
            }
        }
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
    if (!mrc_abi_controller_capable())
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
    if (!mrc_abi_controller_capable())
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
