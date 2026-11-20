#define _GNU_SOURCE
#include "mrc_ctl_store.h"

#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    HEADER_SIZE = 16,
    LEGACY_VERSION = 1,
    TYPED_VERSION = 2,
    GENERATED_FIELDS_VERSION = 3,
    SRV6_VERSION = 4,
    CUSTOM_FORMAT_VERSION = 5,
    EV_EVENT_VERSION = 6,
    EV_PORT_VERSION = 7,
    LEGACY_ENTRY_HEADER_SIZE = 20,
    ENTRY_HEADER_SIZE = 24,
    EV_EVENT_MASK_OFFSET = 22,
    ENTROPY_SIZE = 4 * MRC_EV_MAX_PATHS,
    ENTRY_SIZE = ENTRY_HEADER_SIZE + ENTROPY_SIZE,
    EV_FIELD_SIZE = 20,
    GENERATED_FIELDS_SLOTS = 2,
    GENERATED_FIELDS_ENTRY_SIZE = ENTRY_SIZE + GENERATED_FIELDS_SLOTS * EV_FIELD_SIZE,
    SRV6_VALUES_OFFSET = ENTRY_SIZE + MRC_CTL_STORE_MAX_EV_FIELDS * EV_FIELD_SIZE,
    SRV6_ENTRY_SIZE = SRV6_VALUES_OFFSET + MRC_EV_MAX_PATHS * MRC_CTL_STORE_SRV6_EV_BYTES,
    CUSTOM_FORMAT_ENTRY_SIZE = SRV6_ENTRY_SIZE + 32,
    EV_PORT_ENTRY_SIZE = CUSTOM_FORMAT_ENTRY_SIZE + MRC_EV_MAX_PATHS,
    SNAPSHOT_MAX_SIZE = HEADER_SIZE + MRC_CTL_STORE_MAX_PROFILES * EV_PORT_ENTRY_SIZE,
};

/* Use the existing filename and lock inode when migrating the wire version. */
static const char snapshot_name[] = "profiles.v1";
static const char lock_name[] = "profiles.lock";
static const uint8_t magic[] = { 'M', 'R', 'C', '-', 'E', 'V', 0 };

enum {
    FORMAT_ENTRY_HEADER_SIZE = 16,
    FORMAT_ENTRY_SIZE = FORMAT_ENTRY_HEADER_SIZE + MRC_CTL_STORE_MAX_EV_FIELDS,
    FORMAT_SNAPSHOT_SIZE = HEADER_SIZE + MRC_CTL_STORE_MAX_CUSTOM_FORMATS * FORMAT_ENTRY_SIZE,
};

static const char format_snapshot_name[] = "ev-formats.v1";
static const uint8_t format_magic[] = { 'M', 'R', 'C', '-', 'E', 'F', 0, 1 };

enum {
    CC_CONFIG_SIZE = 48,
    CC_ENTRY_HEADER_SIZE = 16,
    CC_ENTRY_SIZE = CC_ENTRY_HEADER_SIZE + CC_CONFIG_SIZE,
    CC_SNAPSHOT_SIZE = HEADER_SIZE + MRC_CTL_STORE_MAX_CC_PROFILES * CC_ENTRY_SIZE,
};

static const char cc_snapshot_name[] = "cc-profiles.v1";
static const uint8_t cc_magic[] = { 'M', 'R', 'C', '-', 'C', 'C', 0, 1 };

_Static_assert(sizeof(float) == sizeof(uint32_t) && FLT_RADIX == 2 &&
               FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "Persistent CC profiles require IEEE binary32 floats");

struct transaction {
    int fd;
    struct mrc_ctl_store_profile profiles[MRC_CTL_STORE_MAX_PROFILES];
};

static uint32_t path_mask(unsigned int count)
{
    return count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (bytes[i])
            return false;
    }
    return true;
}

bool mrc_ctl_store_srv6_ev_valid(enum mrc_ctl_store_format format,
                                const uint8_t value[MRC_CTL_STORE_SRV6_EV_BYTES])
{
    if (!value || !mrc_ctl_store_format_is_srv6(format) || value[0] == 0xff ||
        bytes_are_zero(value, MRC_CTL_STORE_SRV6_SID_BYTES))
        return false;
    const uint8_t *segment = value + MRC_CTL_STORE_SRV6_SID_BYTES;
    bool empty = bytes_are_zero(segment, MRC_CTL_STORE_SRV6_SID_BYTES);
    return format == MRC_CTL_STORE_FORMAT_SRV6 ? empty : !empty && segment[0] != 0xff;
}

bool mrc_ctl_store_ev_field_valid(const struct mrc_ctl_store_ev_field *field)
{
    if (!field || !field->width || field->width > 32)
        return false;
    uint32_t width_mask = path_mask(field->width);
    uint32_t compact_mask = path_mask(__builtin_popcount(field->mask));
    return !((field->init_val | field->mask) & ~width_mask) &&
           field->min_val <= field->max_val && field->max_val <= compact_mask;
}

bool mrc_ctl_store_format_geometry_valid(enum mrc_ctl_store_format format,
                                         const uint8_t *widths, unsigned int count)
{
    if (!widths || !count || count > MRC_CTL_STORE_MAX_EV_FIELDS)
        return false;
    unsigned int total;
    switch (format) {
    case MRC_CTL_STORE_FORMAT_UDP:
        total = 16;
        break;
    case MRC_CTL_STORE_FORMAT_STEV:
        total = 32;
        break;
    case MRC_CTL_STORE_FORMAT_SRV6:
        total = MRC_CTL_STORE_SRV6_SID_BYTES * 8;
        break;
    case MRC_CTL_STORE_FORMAT_SRV6_SRH:
        total = MRC_CTL_STORE_SRV6_EV_BYTES * 8;
        break;
    default:
        return false;
    }

    const unsigned int sid_bits = MRC_CTL_STORE_SRV6_SID_BYTES * 8;
    unsigned int bits = 0;
    for (unsigned int i = 0; i < count; i++) {
        unsigned int width = widths[i];
        if (!width || width > 32 || bits + width > total ||
            (format == MRC_CTL_STORE_FORMAT_SRV6_SRH && bits < sid_bits && bits + width > sid_bits))
            return false;
        bits += width;
    }
    return bits == total;
}

static bool format_binding_valid(const struct mrc_ctl_store_config *config)
{
    if (!config->format_id)
        return !config->format_field_count && bytes_are_zero(config->format_widths, sizeof(config->format_widths));
    unsigned int count = config->format_field_count;
    return config->format_id > MRC_CTL_STORE_BUILTIN_FORMATS && config->mode != MRC_CTL_STORE_AUTO &&
           mrc_ctl_store_format_geometry_valid(config->format, config->format_widths, count) &&
           bytes_are_zero(config->format_widths + count, sizeof(config->format_widths) - count);
}

static bool ev_fields_valid(const struct mrc_ctl_store_config *config, bool srv6)
{
    if (!config->field_count)
        return (!srv6 && !config->format_id) || config->mode != MRC_CTL_STORE_GENERATED || !config->populated;
    if (config->mode != MRC_CTL_STORE_GENERATED)
        return false;
    if (config->format_id) {
        if (config->field_count > config->format_field_count)
            return false;
        for (unsigned int i = 0; i < config->field_count; i++) {
            if (config->fields[i].width > config->format_widths[i] ||
                !mrc_ctl_store_ev_field_valid(&config->fields[i]))
                return false;
        }
        return true;
    }
    unsigned int count;
    switch (config->format) {
    case MRC_CTL_STORE_FORMAT_UDP:
        count = 1;
        break;
    case MRC_CTL_STORE_FORMAT_STEV:
        count = 2;
        break;
    case MRC_CTL_STORE_FORMAT_SRV6:
        count = MRC_CTL_STORE_SRV6_SID_BYTES / 2;
        break;
    case MRC_CTL_STORE_FORMAT_SRV6_SRH:
        count = MRC_CTL_STORE_SRV6_EV_BYTES / 2;
        break;
    default:
        return false;
    }
    if (config->field_count != count)
        return false;
    for (unsigned int i = 0; i < count; i++) {
        if (config->fields[i].width > 16 || !mrc_ctl_store_ev_field_valid(&config->fields[i]))
            return false;
    }
    return true;
}

static bool config_valid(const struct mrc_ctl_store_config *config)
{
    bool srv6 = mrc_ctl_store_format_is_srv6(config->format);
    if (config->mode < MRC_CTL_STORE_AUTO || config->mode > MRC_CTL_STORE_GENERATED ||
        (unsigned int)config->format > MRC_CTL_STORE_FORMAT_SRV6_SRH ||
        (srv6 && config->mode == MRC_CTL_STORE_AUTO) ||
        !config->count || config->count > MRC_EV_MAX_PATHS || config->min_active > config->count ||
        (config->event_mask & ~(MRC_CTL_STORE_EV_EVENT_GOOD | MRC_CTL_STORE_EV_EVENT_ASSUMED_BAD)) ||
        !format_binding_valid(config) || !ev_fields_valid(config, srv6))
        return false;

    uint32_t paths = path_mask(config->count);
    if ((config->denied | config->populated) & ~paths ||
        (config->mode == MRC_CTL_STORE_AUTO && config->populated))
        return false;

    for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++) {
        bool populated = config->populated & (UINT32_C(1) << i);
        if (config->ports[i] > MRC_EV_MAX_PATHS || (i >= config->count && config->ports[i]) ||
            (config->mode == MRC_CTL_STORE_EXPLICIT && !populated && config->ports[i]))
            return false;
        if ((srv6 || !populated) && config->entropy[i])
            return false;
        if (config->format_id && populated && !srv6 &&
            (!(uint16_t)config->entropy[i] ||
             (config->format == MRC_CTL_STORE_FORMAT_UDP && config->entropy[i] > UINT16_MAX)))
            return false;
        if (srv6 && populated) {
            if (!mrc_ctl_store_srv6_ev_valid(config->format, config->srv6[i]))
                return false;
        } else if (!bytes_are_zero(config->srv6[i], sizeof(config->srv6[i]))) {
            return false;
        }
    }
    return true;
}

static bool config_ready(const struct mrc_ctl_store_config *config)
{
    return config->mode == MRC_CTL_STORE_AUTO || config->populated == path_mask(config->count);
}

static bool profile_valid(const struct mrc_ctl_store_profile *profile)
{
    const struct mrc_ctl_store_config *config = &profile->config;
    switch (profile->state) {
    case MRC_CTL_STORE_INIT:
        if (config->mode || config->format || config->format_id || config->count || config->min_active ||
            config->event_mask ||
            config->populated || config->denied || config->field_count || !format_binding_valid(config))
            return false;
        for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++) {
            if (config->ports[i] || config->entropy[i] || !bytes_are_zero(config->srv6[i], sizeof(config->srv6[i])))
                return false;
        }
        return true;
    case MRC_CTL_STORE_OFFLINE:
        return config_valid(config);
    case MRC_CTL_STORE_ONLINE:
        return config_valid(config) && config_ready(config);
    default:
        return false;
    }
}

static bool same_fields(const struct mrc_ctl_store_config *previous,
                        const struct mrc_ctl_store_config *updated)
{
    if (previous->field_count != updated->field_count)
        return false;
    for (unsigned int i = 0; i < previous->field_count; i++) {
        const struct mrc_ctl_store_ev_field *a = &previous->fields[i], *b = &updated->fields[i];
        if (a->width != b->width || a->init_val != b->init_val || a->min_val != b->min_val ||
            a->max_val != b->max_val || a->mask != b->mask)
            return false;
    }
    return true;
}

static bool same_format_binding(const struct mrc_ctl_store_config *previous,
                                 const struct mrc_ctl_store_config *updated)
{
    return previous->format == updated->format && previous->format_id == updated->format_id &&
           previous->format_field_count == updated->format_field_count &&
           memcmp(previous->format_widths, updated->format_widths, sizeof(previous->format_widths)) == 0;
}

static bool same_layout(const struct mrc_ctl_store_config *previous,
                        const struct mrc_ctl_store_config *updated)
{
    return previous->mode == updated->mode && same_format_binding(previous, updated) &&
           previous->count == updated->count && previous->min_active == updated->min_active &&
           previous->event_mask == updated->event_mask &&
           previous->populated == updated->populated && same_fields(previous, updated) &&
           memcmp(previous->ports, updated->ports, sizeof(previous->ports)) == 0 &&
           memcmp(previous->entropy, updated->entropy, sizeof(previous->entropy)) == 0 &&
           memcmp(previous->srv6, updated->srv6, sizeof(previous->srv6)) == 0;
}

static uint32_t read32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void write32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint64_t read64(const uint8_t *bytes)
{
    return (uint64_t)read32(bytes) << 32 | read32(bytes + 4);
}

static void write64(uint8_t *bytes, uint64_t value)
{
    write32(bytes, (uint32_t)(value >> 32));
    write32(bytes + 4, (uint32_t)value);
}

static uint32_t checksum(const uint8_t *bytes, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

static unsigned int ev_entry_size(uint8_t version)
{
    switch (version) {
    case LEGACY_VERSION:
        return LEGACY_ENTRY_HEADER_SIZE + ENTROPY_SIZE;
    case TYPED_VERSION:
        return ENTRY_SIZE;
    case GENERATED_FIELDS_VERSION:
        return GENERATED_FIELDS_ENTRY_SIZE;
    case SRV6_VERSION:
        return SRV6_ENTRY_SIZE;
    case CUSTOM_FORMAT_VERSION:
    case EV_EVENT_VERSION:
        return CUSTOM_FORMAT_ENTRY_SIZE;
    case EV_PORT_VERSION:
        return EV_PORT_ENTRY_SIZE;
    default:
        return 0;
    }
}

static bool decode_ev_fields(const uint8_t *bytes, unsigned int slots, struct mrc_ctl_store_config *config)
{
    if (config->field_count > slots)
        return false;
    for (unsigned int i = 0; i < config->field_count; i++, bytes += EV_FIELD_SIZE) {
        config->fields[i] = (struct mrc_ctl_store_ev_field){
            .width = read32(bytes), .init_val = read32(bytes + 4),
            .min_val = read32(bytes + 8), .max_val = read32(bytes + 12), .mask = read32(bytes + 16),
        };
    }

    unsigned int unused = (slots - config->field_count) * EV_FIELD_SIZE;
    return bytes_are_zero(bytes, unused);
}

static void encode_ev_fields(uint8_t *bytes, const struct mrc_ctl_store_config *config)
{
    for (unsigned int i = 0; i < config->field_count; i++, bytes += EV_FIELD_SIZE) {
        const struct mrc_ctl_store_ev_field *field = &config->fields[i];
        write32(bytes, field->width);
        write32(bytes + 4, field->init_val);
        write32(bytes + 8, field->min_val);
        write32(bytes + 12, field->max_val);
        write32(bytes + 16, field->mask);
    }
}

static bool decode_profile(const uint8_t *bytes, uint8_t version,
                           struct mrc_ctl_store_profile *profile)
{
    uint64_t id = read64(bytes);
    if (!id || bytes[8] == MRC_CTL_STORE_INIT) {
        /* Empty slots and INIT profiles never contain configuration. */
        for (unsigned int i = 8; i < ev_entry_size(version); i++) {
            if (bytes[i])
                return false;
        }
        *profile = (struct mrc_ctl_store_profile){ .id = id };
        return true;
    }

    bool typed = version != LEGACY_VERSION;
    bool srv6 = version >= SRV6_VERSION;
    bool fields = version >= GENERATED_FIELDS_VERSION;
    unsigned int header_size = typed ? ENTRY_HEADER_SIZE : LEGACY_ENTRY_HEADER_SIZE;
    bool events = version >= EV_EVENT_VERSION;
    if (typed && ((!fields && bytes[21]) || (!events && bytes[EV_EVENT_MASK_OFFSET]) || bytes[23]))
        return false;
    *profile = (struct mrc_ctl_store_profile){
        .id = id, .state = bytes[8],
        .config = {
            .mode = bytes[9], .count = bytes[10], .min_active = bytes[11],
            .event_mask = events ? bytes[EV_EVENT_MASK_OFFSET] : 0,
            .format = typed ? bytes[20] : MRC_CTL_STORE_FORMAT_LEGACY,
            .field_count = fields ? bytes[21] : 0,
            .populated = read32(bytes + 12), .denied = read32(bytes + 16),
        },
    };
    for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++)
        profile->config.entropy[i] = read32(bytes + header_size + 4 * i);
    if (srv6)
        memcpy(profile->config.srv6, bytes + SRV6_VALUES_OFFSET, sizeof(profile->config.srv6));
    else if (mrc_ctl_store_format_is_srv6(profile->config.format))
        return false;
    if (version >= CUSTOM_FORMAT_VERSION) {
        const uint8_t *binding = bytes + SRV6_ENTRY_SIZE;
        const size_t widths = sizeof(profile->config.format_widths);
        size_t used = 9 + widths;
        if (!bytes_are_zero(binding + used, CUSTOM_FORMAT_ENTRY_SIZE - SRV6_ENTRY_SIZE - used))
            return false;
        profile->config.format_id = read64(binding);
        profile->config.format_field_count = binding[8];
        memcpy(profile->config.format_widths, binding + 9, widths);
    }
    if (version >= EV_PORT_VERSION)
        memcpy(profile->config.ports, bytes + CUSTOM_FORMAT_ENTRY_SIZE, sizeof(profile->config.ports));
    unsigned int slots = srv6 ? MRC_CTL_STORE_MAX_EV_FIELDS : GENERATED_FIELDS_SLOTS;
    return (!fields || decode_ev_fields(bytes + ENTRY_SIZE, slots, &profile->config)) && profile_valid(profile);
}

static void encode_profile(uint8_t *bytes, uint8_t version, const struct mrc_ctl_store_profile *profile)
{
    const struct mrc_ctl_store_config *config = &profile->config;
    write64(bytes, profile->id);
    bytes[8] = (uint8_t)profile->state;
    bytes[9] = (uint8_t)config->mode;
    bytes[10] = config->count;
    bytes[11] = config->min_active;
    write32(bytes + 12, config->populated);
    write32(bytes + 16, config->denied);
    bytes[20] = (uint8_t)config->format;
    for (unsigned int i = 0; i < MRC_EV_MAX_PATHS; i++)
        write32(bytes + ENTRY_HEADER_SIZE + 4 * i, config->entropy[i]);
    if (version >= GENERATED_FIELDS_VERSION) {
        bytes[21] = config->field_count;
        encode_ev_fields(bytes + ENTRY_SIZE, config);
    }
    if (version >= EV_EVENT_VERSION)
        bytes[EV_EVENT_MASK_OFFSET] = config->event_mask;
    if (version >= SRV6_VERSION)
        memcpy(bytes + SRV6_VALUES_OFFSET, config->srv6, sizeof(config->srv6));
    if (version >= CUSTOM_FORMAT_VERSION) {
        uint8_t *binding = bytes + SRV6_ENTRY_SIZE;
        write64(binding, config->format_id);
        binding[8] = config->format_field_count;
        memcpy(binding + 9, config->format_widths, sizeof(config->format_widths));
    }
    if (version >= EV_PORT_VERSION)
        memcpy(bytes + CUSTOM_FORMAT_ENTRY_SIZE, config->ports, sizeof(config->ports));
}

static int read_all(int fd, uint8_t *bytes, size_t length)
{
    while (length) {
        ssize_t n = read(fd, bytes, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -errno;
        if (!n)
            return -EBADMSG;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const uint8_t *bytes, size_t length)
{
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -errno;
        if (!n)
            return -EIO;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static int sync_file(int fd)
{
    int result;
    do {
        result = fsync(fd);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : 0;
}

static int read_snapshot_image(const struct mrc_ctl_store *store, const char *name,
                                uint8_t *bytes, size_t capacity, size_t *length)
{
    *length = 0;
    int fd = openat(store->fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -errno;

    struct stat status;
    int result;
    if (fstat(fd, &status) < 0) {
        result = -errno;
    } else if (!S_ISREG(status.st_mode) || status.st_size < HEADER_SIZE ||
               status.st_size > (off_t)capacity) {
        result = -EBADMSG;
    } else {
        *length = (size_t)status.st_size;
        result = read_all(fd, bytes, *length);
    }
    close(fd);
    return result;
}

static bool ev_format_valid(const struct mrc_ctl_store_ev_format *format)
{
    if (!format->id || format->state == MRC_CTL_STORE_INIT)
        return format->state == MRC_CTL_STORE_INIT && !format->format && !format->field_count &&
               (!format->id || format->id > MRC_CTL_STORE_BUILTIN_FORMATS) &&
               bytes_are_zero(format->widths, sizeof(format->widths));
    if (format->id <= MRC_CTL_STORE_BUILTIN_FORMATS ||
        (format->state != MRC_CTL_STORE_OFFLINE && format->state != MRC_CTL_STORE_ONLINE) ||
        format->format < MRC_CTL_STORE_FORMAT_UDP || format->format > MRC_CTL_STORE_FORMAT_SRV6_SRH)
        return false;
    unsigned int count = format->field_count;
    if (!count)
        return format->state == MRC_CTL_STORE_OFFLINE && bytes_are_zero(format->widths, sizeof(format->widths));
    return mrc_ctl_store_format_geometry_valid(format->format, format->widths, count) &&
           bytes_are_zero(format->widths + count, sizeof(format->widths) - count);
}

static int find_format(const struct mrc_ctl_store_ev_format *formats, uint64_t id)
{
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_CUSTOM_FORMATS; i++) {
        if (formats[i].id == id)
            return (int)i;
    }
    return -ENOENT;
}

static int load_format_snapshot(const struct mrc_ctl_store *store, struct mrc_ctl_store_ev_format *formats)
{
    uint8_t bytes[FORMAT_SNAPSHOT_SIZE];
    size_t length;
    int result = read_snapshot_image(store, format_snapshot_name, bytes, sizeof(bytes), &length);
    if (result || !length)
        return result;
    if (length != sizeof(bytes) || memcmp(bytes, format_magic, sizeof(format_magic)) ||
        read32(bytes + 12) || read32(bytes + 8) != checksum(bytes + HEADER_SIZE, length - HEADER_SIZE))
        return -EBADMSG;

    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_CUSTOM_FORMATS; i++) {
        const uint8_t *entry = bytes + HEADER_SIZE + i * FORMAT_ENTRY_SIZE;
        struct mrc_ctl_store_ev_format *format = &formats[i];
        *format = (struct mrc_ctl_store_ev_format){
            .id = read64(entry), .state = entry[8], .format = entry[9], .field_count = entry[10],
        };
        memcpy(format->widths, entry + FORMAT_ENTRY_HEADER_SIZE, sizeof(format->widths));
        if (!bytes_are_zero(entry + 11, FORMAT_ENTRY_HEADER_SIZE - 11) || !ev_format_valid(format))
            return -EBADMSG;
        if (format->id && find_format(formats, format->id) != (int)i)
            return -EBADMSG;
    }
    return 0;
}

int mrc_ctl_store_get_format(const struct mrc_ctl_store *store, uint64_t id,
                             struct mrc_ctl_store_ev_format *format)
{
    if (!store || store->fd < 0 || id <= MRC_CTL_STORE_BUILTIN_FORMATS || !format)
        return -EINVAL;
    struct mrc_ctl_store_ev_format formats[MRC_CTL_STORE_MAX_CUSTOM_FORMATS] = {0};
    int result = load_format_snapshot(store, formats);
    if (result)
        return result;
    int slot = find_format(formats, id);
    if (slot < 0)
        return slot;
    *format = formats[slot];
    return 0;
}

/* The caller holds the metadata lock so the format cannot change between
 * matching its geometry and publishing an ONLINE EV profile. */
static int check_bound_format(const struct mrc_ctl_store *store, const struct mrc_ctl_store_config *config)
{
    if (!config->format_id)
        return 0;
    struct mrc_ctl_store_ev_format format;
    int result = mrc_ctl_store_get_format(store, config->format_id, &format);
    if (result)
        return result;
    if (format.state != MRC_CTL_STORE_ONLINE)
        return -EAGAIN;
    if (format.format != config->format || format.field_count != config->format_field_count ||
        memcmp(format.widths, config->format_widths, sizeof(format.widths)))
        return -EINVAL;
    return 0;
}

static int load_snapshot(const struct mrc_ctl_store *store, struct transaction *txn)
{
    uint8_t bytes[SNAPSHOT_MAX_SIZE];
    size_t length;
    int result = read_snapshot_image(store, snapshot_name, bytes, sizeof(bytes), &length);
    if (result || !length)
        return result;

    uint8_t version = bytes[sizeof(magic)];
    unsigned int entry_size = ev_entry_size(version);
    if (memcmp(bytes, magic, sizeof(magic)) || !entry_size ||
        length != HEADER_SIZE + MRC_CTL_STORE_MAX_PROFILES * entry_size || read32(bytes + 12) ||
        read32(bytes + 8) != checksum(bytes + HEADER_SIZE, length - HEADER_SIZE))
        return -EBADMSG;

    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        struct mrc_ctl_store_profile *profile = &txn->profiles[i];
        if (!decode_profile(bytes + HEADER_SIZE + i * entry_size, version, profile))
            return -EBADMSG;
        for (unsigned int j = 0; profile->id && j < i; j++) {
            if (profile->id == txn->profiles[j].id)
                return -EBADMSG;
        }
    }
    return 0;
}

static int create_temp_file(const struct mrc_ctl_store *store, char *name, size_t length)
{
    for (unsigned int attempt = 0; attempt < 64; attempt++) {
        snprintf(name, length, ".mrc-profile.tmp.%ld.%u", (long)getpid(), attempt);
        int fd = openat(store->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd >= 0 ? fd : -errno;
    }
    return -EEXIST;
}

static int set_file_permissions(const struct mrc_ctl_store *store, int lock_fd, mode_t *snapshot_mode)
{
    struct stat directory, lock;
    if (fstat(store->fd, &directory) < 0 || fstat(lock_fd, &lock) < 0)
        return -errno;

    const mode_t shared = S_ISGID | S_IRGRP | S_IXGRP;
    bool share = (directory.st_mode & shared) == shared;
    *snapshot_mode = share ? 0640 : 0600;
    /* A legacy lock may predate the directory's group assignment. Do not
     * make it readable by that other group. Preserve the stable lock inode. */
    mode_t lock_mode = share && lock.st_gid == directory.st_gid ? 0640 : 0600;
    if ((lock.st_mode & 07777) != lock_mode && fchmod(lock_fd, lock_mode) < 0)
        return -errno;
    return 0;
}

static int publish_snapshot(const struct mrc_ctl_store *store, int lock_fd,
                             const char *name, uint8_t *bytes, size_t length)
{
    mode_t mode = 0600;
    int result = set_file_permissions(store, lock_fd, &mode);
    if (result)
        return result;
    write32(bytes + 8, checksum(bytes + HEADER_SIZE, length - HEADER_SIZE));

    char temp_name[64];
    int fd = create_temp_file(store, temp_name, sizeof(temp_name));
    if (fd < 0)
        return fd;

    result = write_all(fd, bytes, length);
    if (!result && fchmod(fd, mode) < 0)
        result = -errno;
    if (!result)
        result = sync_file(fd);
    if (close(fd) < 0 && !result)
        result = -errno;
    if (!result && renameat(store->fd, temp_name, store->fd, name) < 0)
        result = -errno;
    if (result) {
        unlinkat(store->fd, temp_name, 0);
        return result;
    }

    /* After rename, an fsync error means the commit's durability is uncertain. */
    return sync_file(store->fd);
}

static int save_format_snapshot(const struct mrc_ctl_store *store, int lock_fd,
                                const struct mrc_ctl_store_ev_format *formats)
{
    uint8_t bytes[FORMAT_SNAPSHOT_SIZE] = {0};
    memcpy(bytes, format_magic, sizeof(format_magic));
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_CUSTOM_FORMATS; i++) {
        const struct mrc_ctl_store_ev_format *format = &formats[i];
        uint8_t *entry = bytes + HEADER_SIZE + i * FORMAT_ENTRY_SIZE;
        write64(entry, format->id);
        entry[8] = (uint8_t)format->state;
        entry[9] = (uint8_t)format->format;
        entry[10] = format->field_count;
        memcpy(entry + FORMAT_ENTRY_HEADER_SIZE, format->widths, sizeof(format->widths));
    }
    return publish_snapshot(store, lock_fd, format_snapshot_name, bytes, sizeof(bytes));
}

static int save_snapshot(const struct mrc_ctl_store *store, const struct transaction *txn)
{
    uint8_t version = TYPED_VERSION;
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        const struct mrc_ctl_store_config *config = &txn->profiles[i].config;
        if (!bytes_are_zero(config->ports, sizeof(config->ports))) {
            version = EV_PORT_VERSION;
            break;
        }
        if (config->event_mask)
            version = EV_EVENT_VERSION;
        else if (config->format_id && version < CUSTOM_FORMAT_VERSION)
            version = CUSTOM_FORMAT_VERSION;
        else if (mrc_ctl_store_format_is_srv6(config->format) && version < SRV6_VERSION)
            version = SRV6_VERSION;
        else if (config->field_count && version < GENERATED_FIELDS_VERSION)
            version = GENERATED_FIELDS_VERSION;
    }

    uint8_t bytes[SNAPSHOT_MAX_SIZE] = {0};
    memcpy(bytes, magic, sizeof(magic));
    bytes[sizeof(magic)] = version;
    unsigned int entry_size = ev_entry_size(version);
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++)
        encode_profile(bytes + HEADER_SIZE + i * entry_size, version, &txn->profiles[i]);
    size_t length = HEADER_SIZE + MRC_CTL_STORE_MAX_PROFILES * entry_size;
    return publish_snapshot(store, txn->fd, snapshot_name, bytes, length);
}

static int lock_file(int fd, int operation)
{
    int result;
    do {
        result = flock(fd, operation);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : 0;
}

static int open_metadata_lock(const struct mrc_ctl_store *store, bool write)
{
    if (!store || store->fd < 0)
        return -EINVAL;

    int flags = write ? O_RDWR | O_CREAT : O_RDONLY;
    int fd = openat(store->fd, lock_name, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
        return -errno;

    struct stat status;
    int result;
    if (fstat(fd, &status) < 0)
        result = -errno;
    else if (!S_ISREG(status.st_mode))
        result = -EBADMSG;
    else
        result = lock_file(fd, write ? LOCK_EX : LOCK_SH);
    if (result) {
        close(fd);
        return result;
    }
    return fd;
}

static int begin(const struct mrc_ctl_store *store, struct transaction *txn, bool write)
{
    *txn = (struct transaction){ .fd = -1 };
    int fd = open_metadata_lock(store, write);
    if (fd < 0)
        return fd;
    int result = load_snapshot(store, txn);
    if (result)
        close(fd);
    else
        txn->fd = fd;
    return result;
}

static int finish(const struct mrc_ctl_store *store, const struct transaction *txn,
                  int result, bool save)
{
    if (!result && save)
        result = save_snapshot(store, txn);
    close(txn->fd);
    return result;
}

static int find_profile(const struct transaction *txn, uint64_t id)
{
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        if (txn->profiles[i].id == id)
            return (int)i;
    }
    return -ENOENT;
}

static int begin_profile(const struct mrc_ctl_store *store, struct transaction *txn,
                          uint64_t id, bool write)
{
    if (!id)
        return -EINVAL;
    int result = begin(store, txn, write);
    if (result)
        return result;
    int slot = find_profile(txn, id);
    if (slot < 0)
        return finish(store, txn, slot, false);
    return slot;
}

/* The metadata lock serializes slot reuse with acquisition of a lease.
 * Each lease is owned by its open file description, independent of the
 * store handle, and the kernel drops it when its last descriptor closes. */
static int lock_reference(int fd, int slot, short type)
{
    struct flock lock = {
        .l_type = type, .l_whence = SEEK_SET, .l_start = slot, .l_len = 1,
    };
    int result;
    do {
        result = fcntl(fd, F_OFD_SETLK, &lock);
    } while (result < 0 && errno == EINTR);
    if (!result)
        return 0;
    return errno == EACCES || errno == EAGAIN ? -EBUSY : -errno;
}

int mrc_ctl_store_open(struct mrc_ctl_store *store, const char *directory)
{
    if (!store || !directory || !*directory)
        return -EINVAL;
    if (store->fd >= 0)
        return -EBUSY;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -errno;
    store->fd = fd;
    return 0;
}

void mrc_ctl_store_close(struct mrc_ctl_store *store)
{
    if (store && store->fd >= 0) {
        close(store->fd);
        store->fd = -1;
    }
}

static bool ev_format_equal(const struct mrc_ctl_store_ev_format *a, const struct mrc_ctl_store_ev_format *b)
{
    return a->id == b->id && a->state == b->state && a->format == b->format &&
           a->field_count == b->field_count && memcmp(a->widths, b->widths, sizeof(a->widths)) == 0;
}

int mrc_ctl_store_edit_format(const struct mrc_ctl_store *store, uint64_t id,
                              int (*edit)(struct mrc_ctl_store_ev_format *, void *), void *arg)
{
    if (id <= MRC_CTL_STORE_BUILTIN_FORMATS || !edit)
        return -EINVAL;
    struct transaction txn;
    int result = begin(store, &txn, true);
    if (result)
        return result;

    struct mrc_ctl_store_ev_format formats[MRC_CTL_STORE_MAX_CUSTOM_FORMATS] = {0};
    result = load_format_snapshot(store, formats);
    if (result)
        return finish(store, &txn, result, false);
    int slot = find_format(formats, id);
    struct mrc_ctl_store_ev_format updated = slot >= 0 ? formats[slot] :
        (struct mrc_ctl_store_ev_format){ .id = id };
    result = edit(&updated, arg);
    if (result)
        return finish(store, &txn, result, false);

    if (!updated.id) {
        if (slot < 0)
            return finish(store, &txn, 0, false);
        updated = (struct mrc_ctl_store_ev_format){0};
    } else if (updated.id != id || !ev_format_valid(&updated)) {
        return finish(store, &txn, -EINVAL, false);
    }
    if (slot >= 0 && ev_format_equal(&updated, &formats[slot]))
        return finish(store, &txn, 0, false);
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        const struct mrc_ctl_store_profile *profile = &txn.profiles[i];
        if (profile->state == MRC_CTL_STORE_ONLINE && profile->config.format_id == id)
            return finish(store, &txn, -EBUSY, false);
    }
    if (slot < 0) {
        slot = find_format(formats, 0);
        if (slot < 0)
            return finish(store, &txn, -ENOSPC, false);
    }
    formats[slot] = updated;
    result = save_format_snapshot(store, txn.fd, formats);
    return finish(store, &txn, result, false);
}

int mrc_ctl_store_create(const struct mrc_ctl_store *store, uint64_t id)
{
    if (!id)
        return -EINVAL;
    struct transaction txn;
    int result = begin(store, &txn, true);
    if (result)
        return result;
    int slot = find_profile(&txn, id);
    if (slot >= 0)
        return finish(store, &txn, -EEXIST, false);
    slot = find_profile(&txn, 0);
    if (slot < 0)
        return finish(store, &txn, -ENOSPC, false);
    txn.profiles[slot] = (struct mrc_ctl_store_profile){ .id = id };
    return finish(store, &txn, 0, true);
}

int mrc_ctl_store_get(const struct mrc_ctl_store *store, uint64_t id,
                      struct mrc_ctl_store_profile *profile)
{
    if (!store || store->fd < 0 || !id || !profile)
        return -EINVAL;

    /* Writers publish a new inode by rename, so an open snapshot remains
     * stable while a writer persists or publishes its replacement. */
    struct transaction txn = {0};
    int result = load_snapshot(store, &txn);
    if (result)
        return result;
    int slot = find_profile(&txn, id);
    if (slot < 0)
        return slot;
    *profile = txn.profiles[slot];
    return 0;
}

int mrc_ctl_store_list(const struct mrc_ctl_store *store, struct mrc_ctl_store_profile *profiles,
                       size_t capacity, size_t *count)
{
    if (!store || store->fd < 0 || !count || (capacity && !profiles))
        return -EINVAL;
    struct transaction txn = {0};
    int result = load_snapshot(store, &txn);
    if (result)
        return result;

    size_t required = 0;
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++)
        required += txn.profiles[i].id != 0;
    *count = required;
    if (required > capacity)
        return -ENOSPC;

    size_t next = 0;
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_PROFILES; i++) {
        if (txn.profiles[i].id)
            profiles[next++] = txn.profiles[i];
    }
    return 0;
}

int mrc_ctl_store_configure(const struct mrc_ctl_store *store, uint64_t id,
                            const struct mrc_ctl_store_config *config)
{
    if (!config || !config_valid(config))
        return -EINVAL;
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    struct mrc_ctl_store_profile *profile = &txn.profiles[slot];
    if (profile->state == MRC_CTL_STORE_ONLINE)
        return finish(store, &txn, -EBUSY, false);
    int result = check_bound_format(store, config);
    if (!result)
        result = lock_reference(txn.fd, slot, F_WRLCK);
    if (!result) {
        profile->config = *config;
        profile->state = MRC_CTL_STORE_OFFLINE;
    }
    return finish(store, &txn, result, true);
}

int mrc_ctl_store_set_state(const struct mrc_ctl_store *store, uint64_t id,
                            enum mrc_ctl_store_state state)
{
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    struct mrc_ctl_store_profile *profile = &txn.profiles[slot];
    if (state == profile->state)
        return finish(store, &txn, 0, false);
    bool valid = (profile->state == MRC_CTL_STORE_ONLINE && state == MRC_CTL_STORE_OFFLINE) ||
                 (profile->state == MRC_CTL_STORE_OFFLINE &&
                  (state == MRC_CTL_STORE_INIT ||
                   (state == MRC_CTL_STORE_ONLINE && config_ready(&profile->config))));
    if (!valid)
        return finish(store, &txn, -EINVAL, false);
    int result = state == MRC_CTL_STORE_ONLINE ? check_bound_format(store, &profile->config) : 0;
    if (!result)
        result = lock_reference(txn.fd, slot, F_WRLCK);
    if (!result) {
        profile->state = state;
        if (state == MRC_CTL_STORE_INIT)
            profile->config = (struct mrc_ctl_store_config){0};
    }
    return finish(store, &txn, result, true);
}

int mrc_ctl_store_remove(const struct mrc_ctl_store *store, uint64_t id)
{
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    if (txn.profiles[slot].state == MRC_CTL_STORE_ONLINE)
        return finish(store, &txn, -EBUSY, false);
    int result = lock_reference(txn.fd, slot, F_WRLCK);
    if (!result)
        txn.profiles[slot] = (struct mrc_ctl_store_profile){0};
    return finish(store, &txn, result, true);
}

int mrc_ctl_store_edit(const struct mrc_ctl_store *store, uint64_t id,
                       int (*edit)(struct mrc_ctl_store_profile *, void *), void *arg)
{
    if (!id || !edit)
        return -EINVAL;
    struct transaction txn;
    int result = begin(store, &txn, true);
    if (result)
        return result;

    int slot = find_profile(&txn, id);
    struct mrc_ctl_store_profile previous = slot >= 0 ? txn.profiles[slot] :
        (struct mrc_ctl_store_profile){ .id = id };
    struct mrc_ctl_store_profile updated = previous;
    result = edit(&updated, arg);
    if (result)
        return finish(store, &txn, result, false);

    if (!updated.id) {
        if (slot < 0)
            return finish(store, &txn, 0, false);
        updated = (struct mrc_ctl_store_profile){0};
    } else if (updated.id != id || !profile_valid(&updated)) {
        return finish(store, &txn, -EINVAL, false);
    }

    bool same = same_layout(&previous.config, &updated.config);
    if (slot >= 0 && updated.id && same && updated.state == previous.state &&
        updated.config.denied == previous.config.denied)
        return finish(store, &txn, 0, false);
    if (updated.state == MRC_CTL_STORE_ONLINE || !same_format_binding(&previous.config, &updated.config)) {
        result = check_bound_format(store, &updated.config);
        if (result)
            return finish(store, &txn, result, false);
    }
    if (slot < 0) {
        slot = find_profile(&txn, 0);
        if (slot < 0)
            return finish(store, &txn, -ENOSPC, false);
    }

    if (previous.state == MRC_CTL_STORE_ONLINE && updated.state == MRC_CTL_STORE_ONLINE) {
        if (!same)
            return finish(store, &txn, -EBUSY, false);
    } else {
        result = lock_reference(txn.fd, slot, F_WRLCK);
        if (result)
            return finish(store, &txn, result, false);
    }
    txn.profiles[slot] = updated;
    return finish(store, &txn, 0, true);
}

int mrc_ctl_store_update_denied(const struct mrc_ctl_store *store, uint64_t id,
                               uint32_t set, uint32_t clear)
{
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, true);
    if (slot < 0)
        return slot;
    struct mrc_ctl_store_config *config = &txn.profiles[slot].config;
    if (txn.profiles[slot].state == MRC_CTL_STORE_INIT || (set & clear) ||
        ((set | clear) & ~path_mask(config->count)))
        return finish(store, &txn, -EINVAL, false);
    uint32_t updated = (config->denied | set) & ~clear;
    if (updated == config->denied)
        return finish(store, &txn, 0, false);
    config->denied = updated;
    return finish(store, &txn, 0, true);
}

int mrc_ctl_store_acquire(const struct mrc_ctl_store *store, uint64_t id,
                          struct mrc_ctl_store_ref *ref, struct mrc_ctl_store_profile *profile)
{
    if (!ref || !profile)
        return -EINVAL;
    if (ref->fd >= 0)
        return -EBUSY;
    struct transaction txn;
    int slot = begin_profile(store, &txn, id, false);
    if (slot < 0)
        return slot;
    if (txn.profiles[slot].state != MRC_CTL_STORE_ONLINE)
        return finish(store, &txn, -EAGAIN, false);
    int result = check_bound_format(store, &txn.profiles[slot].config);
    if (!result)
        result = lock_reference(txn.fd, slot, F_RDLCK);
    if (!result)
        result = lock_file(txn.fd, LOCK_UN);
    if (result)
        return finish(store, &txn, result, false);
    *profile = txn.profiles[slot];
    ref->fd = txn.fd;
    return 0;
}

void mrc_ctl_store_release(struct mrc_ctl_store_ref *ref)
{
    if (ref && ref->fd >= 0) {
        close(ref->fd);
        ref->fd = -1;
    }
}

static float read_float32(const uint8_t *bytes)
{
    uint32_t bits = read32(bytes);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_float32(uint8_t *bytes, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write32(bytes, bits);
}

static void encode_cc_config(uint8_t bytes[CC_CONFIG_SIZE], const struct mrc_cc_config *config)
{
    bytes[0] = config->disable_qa;
    bytes[1] = config->qa_gate;
    write32(bytes + 4, config->adjust_bytes_threshold);
    write32(bytes + 8, config->adjust_period_threshold);
    write32(bytes + 12, config->base_rtt);
    write_float32(bytes + 16, config->eta);
    write_float32(bytes + 20, config->fi);
    write_float32(bytes + 24, config->fi_scale);
    write_float32(bytes + 28, config->gamma);
    write_float32(bytes + 32, config->max_md_jump);
    write32(bytes + 36, config->max_cwnd);
    write32(bytes + 40, config->qa_threshold);
    write32(bytes + 44, config->target_delay);
}

static bool cc_profile_valid(const struct mrc_ctl_store_cc_profile *profile)
{
    if ((unsigned int)profile->state > MRC_CTL_STORE_ONLINE ||
        (unsigned int)profile->algorithm > MRC_CC_NSCC)
        return false;
    if (!profile->id && profile->state != MRC_CTL_STORE_INIT)
        return false;
    if (profile->state == MRC_CTL_STORE_INIT && profile->algorithm != MRC_CC_DISABLED)
        return false;
    if (profile->configured)
        return profile->algorithm == MRC_CC_NSCC;
    if (profile->state == MRC_CTL_STORE_ONLINE && profile->algorithm == MRC_CC_NSCC)
        return false;

    uint8_t encoded[CC_CONFIG_SIZE] = {0};
    const uint8_t empty[CC_CONFIG_SIZE] = {0};
    encode_cc_config(encoded, &profile->config);
    return memcmp(encoded, empty, sizeof(encoded)) == 0;
}

static bool decode_cc_profile(const uint8_t *bytes, struct mrc_ctl_store_cc_profile *profile)
{
    const uint8_t empty[5] = {0};
    const uint8_t *config = bytes + CC_ENTRY_HEADER_SIZE;
    if (bytes[10] > 1 || memcmp(bytes + 11, empty, sizeof(empty)) || config[2] || config[3])
        return false;
    *profile = (struct mrc_ctl_store_cc_profile){
        .id = (uint64_t)read32(bytes) << 32 | read32(bytes + 4),
        .state = bytes[8], .algorithm = bytes[9], .configured = bytes[10] != 0,
        .config = {
            .disable_qa = config[0], .qa_gate = config[1],
            .adjust_bytes_threshold = read32(config + 4),
            .adjust_period_threshold = read32(config + 8),
            .base_rtt = read32(config + 12),
            .eta = read_float32(config + 16),
            .fi = read_float32(config + 20),
            .fi_scale = read_float32(config + 24),
            .gamma = read_float32(config + 28),
            .max_md_jump = read_float32(config + 32),
            .max_cwnd = read32(config + 36),
            .qa_threshold = read32(config + 40),
            .target_delay = read32(config + 44),
        },
    };
    return cc_profile_valid(profile);
}

static void encode_cc_profile(uint8_t *bytes, const struct mrc_ctl_store_cc_profile *profile)
{
    write32(bytes, (uint32_t)(profile->id >> 32));
    write32(bytes + 4, (uint32_t)profile->id);
    bytes[8] = (uint8_t)profile->state;
    bytes[9] = (uint8_t)profile->algorithm;
    bytes[10] = profile->configured;
    encode_cc_config(bytes + CC_ENTRY_HEADER_SIZE, &profile->config);
}

static int find_cc_profile(const struct mrc_ctl_store_cc_profile *profiles, uint64_t id)
{
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_CC_PROFILES; i++) {
        if (profiles[i].id == id)
            return (int)i;
    }
    return -ENOENT;
}

static int load_cc_snapshot(const struct mrc_ctl_store *store,
                            struct mrc_ctl_store_cc_profile *profiles)
{
    uint8_t bytes[CC_SNAPSHOT_SIZE];
    size_t length;
    int result = read_snapshot_image(store, cc_snapshot_name, bytes, sizeof(bytes), &length);
    if (result || !length)
        return result;
    if (length != sizeof(bytes) || memcmp(bytes, cc_magic, sizeof(cc_magic)) ||
        read32(bytes + 12) || read32(bytes + 8) != checksum(bytes + HEADER_SIZE, length - HEADER_SIZE))
        return -EBADMSG;

    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_CC_PROFILES; i++) {
        struct mrc_ctl_store_cc_profile *profile = &profiles[i];
        if (!decode_cc_profile(bytes + HEADER_SIZE + i * CC_ENTRY_SIZE, profile))
            return -EBADMSG;
        for (unsigned int j = 0; profile->id && j < i; j++) {
            if (profile->id == profiles[j].id)
                return -EBADMSG;
        }
    }
    return 0;
}

static int save_cc_snapshot(const struct mrc_ctl_store *store, int lock_fd,
                            const struct mrc_ctl_store_cc_profile *profiles)
{
    uint8_t bytes[CC_SNAPSHOT_SIZE] = {0};
    memcpy(bytes, cc_magic, sizeof(cc_magic));
    for (unsigned int i = 0; i < MRC_CTL_STORE_MAX_CC_PROFILES; i++)
        encode_cc_profile(bytes + HEADER_SIZE + i * CC_ENTRY_SIZE, &profiles[i]);
    return publish_snapshot(store, lock_fd, cc_snapshot_name, bytes, sizeof(bytes));
}

static bool cc_profiles_equal(const struct mrc_ctl_store_cc_profile *lhs,
                              const struct mrc_ctl_store_cc_profile *rhs)
{
    uint8_t left[CC_ENTRY_SIZE] = {0}, right[CC_ENTRY_SIZE] = {0};
    encode_cc_profile(left, lhs);
    encode_cc_profile(right, rhs);
    return memcmp(left, right, sizeof(left)) == 0;
}

int mrc_ctl_store_get_cc(const struct mrc_ctl_store *store, uint64_t id,
                         struct mrc_ctl_store_cc_profile *profile)
{
    if (!store || store->fd < 0 || !id || !profile)
        return -EINVAL;
    struct mrc_ctl_store_cc_profile profiles[MRC_CTL_STORE_MAX_CC_PROFILES] = {0};
    int result = load_cc_snapshot(store, profiles);
    if (result)
        return result;
    int slot = find_cc_profile(profiles, id);
    if (slot < 0)
        return slot;
    *profile = profiles[slot];
    return 0;
}

int mrc_ctl_store_edit_cc(const struct mrc_ctl_store *store, uint64_t id,
                          int (*edit)(struct mrc_ctl_store_cc_profile *, void *), void *arg)
{
    if (!id || !edit)
        return -EINVAL;
    int fd = open_metadata_lock(store, true);
    if (fd < 0)
        return fd;

    struct mrc_ctl_store_cc_profile profiles[MRC_CTL_STORE_MAX_CC_PROFILES] = {0};
    int result = load_cc_snapshot(store, profiles);
    if (result)
        goto done;
    int slot = find_cc_profile(profiles, id);
    struct mrc_ctl_store_cc_profile updated = slot >= 0 ? profiles[slot] :
        (struct mrc_ctl_store_cc_profile){ .id = id };
    result = edit(&updated, arg);
    if (result)
        goto done;

    if (!updated.id) {
        if (slot < 0)
            goto done;
        updated = (struct mrc_ctl_store_cc_profile){0};
    } else if (updated.id != id || !cc_profile_valid(&updated)) {
        result = -EINVAL;
        goto done;
    }

    if (slot >= 0 && cc_profiles_equal(&profiles[slot], &updated))
        goto done;
    if (slot < 0) {
        slot = find_cc_profile(profiles, 0);
        if (slot < 0) {
            result = -ENOSPC;
            goto done;
        }
    }

    /* CC and EV profiles may have the same IDs and slot indices. Their
     * leases occupy disjoint byte ranges on the existing lock inode. */
    result = lock_reference(fd, MRC_CTL_STORE_MAX_PROFILES + slot, F_WRLCK);
    if (!result) {
        profiles[slot] = updated;
        result = save_cc_snapshot(store, fd, profiles);
    }

done:
    close(fd);
    return result;
}

int mrc_ctl_store_acquire_cc(const struct mrc_ctl_store *store, uint64_t id,
                             struct mrc_ctl_store_ref *ref, struct mrc_ctl_store_cc_profile *profile)
{
    if (!id || !ref || !profile)
        return -EINVAL;
    if (ref->fd >= 0)
        return -EBUSY;
    int fd = open_metadata_lock(store, false);
    if (fd < 0)
        return fd;

    struct mrc_ctl_store_cc_profile profiles[MRC_CTL_STORE_MAX_CC_PROFILES] = {0};
    int result = load_cc_snapshot(store, profiles);
    if (!result) {
        int slot = find_cc_profile(profiles, id);
        if (slot < 0) {
            result = slot;
        } else if (profiles[slot].state != MRC_CTL_STORE_ONLINE) {
            result = -EAGAIN;
        } else {
            result = lock_reference(fd, MRC_CTL_STORE_MAX_PROFILES + slot, F_RDLCK);
            if (!result)
                result = lock_file(fd, LOCK_UN);
            if (!result) {
                *profile = profiles[slot];
                ref->fd = fd;
                return 0;
            }
        }
    }
    close(fd);
    return result;
}
