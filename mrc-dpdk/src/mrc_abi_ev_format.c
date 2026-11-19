#include "mrc_abi.h"

#include <errno.h>
#include <mrc_ctl.h>
#include <string.h>

enum {
    FORMAT_ATTRS = MRC_CTL_EV_FMT_PROFILE_STATE | MRC_CTL_EV_FMT_PROFILE_CUR_STATE |
        MRC_CTL_EV_FMT_PROFILE_MODE | MRC_CTL_EV_FMT_PROFILE_OP,
};

struct format_change {
    const struct mrc_ctl_ev_fmt_profile_attr *attr;
    unsigned int mask;
};

static int format_error(int error)
{
    errno = error;
    return -1;
}

static enum mrc_ctl_ev_fmt_mode controller_mode(enum mrc_ctl_store_format format)
{
    switch (format) {
    case MRC_CTL_STORE_FORMAT_UDP:
        return MRC_CTL_EV_FMT_MODE_UDP;
    case MRC_CTL_STORE_FORMAT_STEV:
        return MRC_CTL_EV_FMT_MODE_STEV;
    case MRC_CTL_STORE_FORMAT_SRV6:
    case MRC_CTL_STORE_FORMAT_SRV6_SRH:
        return MRC_CTL_EV_FMT_MODE_SRV6;
    default:
        return 0;
    }
}

static enum mrc_ctl_store_format store_mode(enum mrc_ctl_ev_fmt_mode mode)
{
    switch (mode) {
    case MRC_CTL_EV_FMT_MODE_UDP:
        return MRC_CTL_STORE_FORMAT_UDP;
    case MRC_CTL_EV_FMT_MODE_STEV:
        return MRC_CTL_STORE_FORMAT_STEV;
    case MRC_CTL_EV_FMT_MODE_SRV6:
        return MRC_CTL_STORE_FORMAT_SRV6;
    default:
        return MRC_CTL_STORE_FORMAT_LEGACY;
    }
}

static int query_fields(const struct mrc_ctl_store_ev_format *format,
                          struct mrc_ctl_ev_fmt_profile_attr *attr)
{
    int capacity = attr->ev_fmt_op.fmt_field_count;
    if (capacity < 0 || attr->ev_fmt_op.op != MRC_CTL_EV_FMT_OP_QUERY_FIELDS)
        return -EINVAL;
    int count = format->field_count;
    attr->ev_fmt_op.fmt_field_count = count;
    if (capacity < count)
        return -E2BIG;
    if (count && !attr->ev_fmt_op.fmt_fields)
        return -EINVAL;
    for (int i = 0; i < count; i++)
        attr->ev_fmt_op.fmt_fields[i] = (struct mrc_ctl_ev_fmt_field){ .width = format->widths[i] };
    return 0;
}

static int modify_fields(struct mrc_ctl_store_ev_format *format,
                           const struct mrc_ctl_ev_fmt_profile_attr *attr)
{
    int count = attr->ev_fmt_op.fmt_field_count;
    if (count < 0 || (count && !attr->ev_fmt_op.fmt_fields) ||
        attr->ev_fmt_op.op != MRC_CTL_EV_FMT_OP_MODIFY_FIELDS)
        return -EINVAL;
    if (count > (int)MRC_CTL_STORE_MAX_EV_FIELDS)
        return -E2BIG;

    uint8_t widths[MRC_CTL_STORE_MAX_EV_FIELDS] = {0};
    unsigned int total = 0;
    for (int i = 0; i < count; i++) {
        widths[i] = attr->ev_fmt_op.fmt_fields[i].width;
        total += widths[i];
    }
    enum mrc_ctl_store_format kind = format->format;
    if (mrc_ctl_store_format_is_srv6(kind))
        kind = total == MRC_CTL_STORE_SRV6_EV_BYTES * 8 ?
            MRC_CTL_STORE_FORMAT_SRV6_SRH : MRC_CTL_STORE_FORMAT_SRV6;
    if (!mrc_ctl_store_format_geometry_valid(kind, widths, (unsigned int)count))
        return -E2BIG;
    format->format = kind;
    format->field_count = (uint8_t)count;
    memcpy(format->widths, widths, sizeof(format->widths));
    return 0;
}

static int edit_format(struct mrc_ctl_store_ev_format *format, void *argument)
{
    const struct format_change *change = argument;
    const struct mrc_ctl_ev_fmt_profile_attr *attr = change->attr;
    unsigned int mask = change->mask;
    enum mrc_ctl_store_state from = format->state;
    enum mrc_ctl_store_state to = from;
    if (mask & MRC_CTL_EV_FMT_PROFILE_STATE) {
        if ((unsigned int)attr->profile_state > MRC_CTL_PROFILE_ONLINE)
            return -EINVAL;
        to = (enum mrc_ctl_store_state)attr->profile_state;
    }
    if ((mask & MRC_CTL_EV_FMT_PROFILE_CUR_STATE) && (int)attr->cur_profile_state != (int)from)
        return -EINVAL;
    if ((from == MRC_CTL_STORE_INIT && to == MRC_CTL_STORE_ONLINE) ||
        (from == MRC_CTL_STORE_ONLINE && to == MRC_CTL_STORE_INIT))
        return -EINVAL;
    const unsigned int geometry = MRC_CTL_EV_FMT_PROFILE_MODE | MRC_CTL_EV_FMT_PROFILE_OP;
    if (from == MRC_CTL_STORE_ONLINE && (mask & geometry))
        return -EBUSY;
    if (to == MRC_CTL_STORE_INIT) {
        if (mask & geometry)
            return -EINVAL;
        *format = (struct mrc_ctl_store_ev_format){0};
        return 0;
    }
    if (from == MRC_CTL_STORE_INIT && !(mask & MRC_CTL_EV_FMT_PROFILE_MODE))
        return -EINVAL;
    if (mask & MRC_CTL_EV_FMT_PROFILE_MODE) {
        enum mrc_ctl_store_format kind = store_mode(attr->ev_fmt_mode);
        if (kind == MRC_CTL_STORE_FORMAT_LEGACY)
            return -EINVAL;
        if (attr->ev_fmt_mode != controller_mode(format->format)) {
            format->format = kind;
            format->field_count = 0;
            memset(format->widths, 0, sizeof(format->widths));
        }
    }
    if (mask & MRC_CTL_EV_FMT_PROFILE_OP) {
        int error = modify_fields(format, attr);
        if (error)
            return error;
    }
    if (to == MRC_CTL_STORE_ONLINE && !format->field_count)
        return -EINVAL;
    format->state = to;
    return 0;
}

MRC_PUBLIC int mrc_ctl_modify_ev_fmt_profile(struct mrc_context *context, uint64_t id,
                                             struct mrc_ctl_ev_fmt_profile_attr *attr, int mask)
{
    if (!context || !id || !attr || ((unsigned int)mask & ~FORMAT_ATTRS))
        return format_error(EINVAL);
    if (!mrc_abi_controller_capable())
        return format_error(EPERM);
    if ((mask & MRC_CTL_EV_FMT_PROFILE_OP) && attr->ev_fmt_op.op == MRC_CTL_EV_FMT_OP_QUERY_FIELDS) {
        if (mask != MRC_CTL_EV_FMT_PROFILE_OP)
            return format_error(EINVAL);
        return mrc_ctl_query_ev_fmt_profile(context, id, attr, mask);
    }
    if (id <= MRC_CTL_STORE_BUILTIN_FORMATS)
        return format_error(EOPNOTSUPP);
    struct mrc_ctl_store store = mrc_abi_profile_store(context);
    if (store.fd < 0)
        return format_error(ENODEV);
    struct format_change change = { .attr = attr, .mask = (unsigned int)mask };
    int error = mrc_ctl_store_edit_format(&store, id, edit_format, &change);
    return error ? format_error(-error) : 0;
}

MRC_PUBLIC int mrc_ctl_query_ev_fmt_profile(struct mrc_context *context, uint64_t id,
                                            struct mrc_ctl_ev_fmt_profile_attr *attr, int mask)
{
    if (!context || !id || !attr || ((unsigned int)mask & ~FORMAT_ATTRS))
        return format_error(EINVAL);
    struct mrc_ctl_store_ev_format format = {0};
    if (id <= MRC_CTL_STORE_BUILTIN_FORMATS) {
        format.format = mrc_abi_builtin_ev_format(id);
        format.state = MRC_CTL_STORE_ONLINE;
        format.field_count = (uint8_t)mrc_abi_builtin_ev_fields(format.format);
        memset(format.widths, 16, format.field_count);
    } else {
        struct mrc_ctl_store store = mrc_abi_profile_store(context);
        if (store.fd < 0)
            return format_error(ENODEV);
        int error = mrc_ctl_store_get_format(&store, id, &format);
        if (error)
            return format_error(-error);
    }
    if (mask & MRC_CTL_EV_FMT_PROFILE_STATE)
        attr->profile_state = (enum mrc_ctl_profile_state)format.state;
    if (mask & MRC_CTL_EV_FMT_PROFILE_CUR_STATE)
        attr->cur_profile_state = (enum mrc_ctl_profile_state)format.state;
    if (mask & MRC_CTL_EV_FMT_PROFILE_MODE)
        attr->ev_fmt_mode = controller_mode(format.format);
    if (mask & MRC_CTL_EV_FMT_PROFILE_OP) {
        int error = query_fields(&format, attr);
        if (error)
            return format_error(-error);
    }
    return 0;
}
