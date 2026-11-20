#include "mrc_ctl_store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_number(const char *arg, uint64_t limit, uint64_t *value)
{
    if (*arg < '0' || *arg > '9')
        return false;
    char *end;
    int base = arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X') ? 16 : 10;
    errno = 0;
    uintmax_t parsed = strtoumax(arg, &end, base);
    if (errno || *end || parsed > limit)
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static const char *const states[] = {
    [MRC_CTL_STORE_INIT] = "init",
    [MRC_CTL_STORE_OFFLINE] = "offline",
    [MRC_CTL_STORE_ONLINE] = "online",
};

static const char *const modes[] = {
    [0] = "unconfigured",
    [MRC_CTL_STORE_AUTO] = "auto",
    [MRC_CTL_STORE_EXPLICIT] = "explicit",
    [MRC_CTL_STORE_GENERATED] = "generated",
};

static void show(const struct mrc_ctl_store_profile *profile)
{
    const struct mrc_ctl_store_config *config = &profile->config;
    printf("%" PRIu64 " %s %s count=%u min_active=%u denied=0x%08" PRIx32,
           profile->id, states[profile->state], modes[config->mode],
           (unsigned int)config->count, (unsigned int)config->min_active, config->denied);
    for (unsigned int path = 0; path < config->count; path++) {
        if (!(config->populated & (UINT32_C(1) << path)))
            continue;
        if (mrc_ctl_store_format_is_srv6(config->format)) {
            const uint8_t *value = config->srv6[path];
            char destination[INET6_ADDRSTRLEN], segment[INET6_ADDRSTRLEN];
            printf(" ev[%u]=%s", path, inet_ntop(AF_INET6, value, destination, sizeof(destination)));
            if (config->format == MRC_CTL_STORE_FORMAT_SRV6_SRH)
                printf("/%s", inet_ntop(AF_INET6, value + MRC_CTL_STORE_SRV6_SID_BYTES,
                                        segment, sizeof(segment)));
        } else {
            printf(" ev[%u]=0x%08" PRIx32, path, config->entropy[path]);
        }
    }
    for (unsigned int ev = 0; ev < config->count; ev++) {
        if (config->ports[ev])
            printf(" port[%u]=%u", ev, mrc_ctl_store_ev_port(config, ev));
    }
    putchar('\n');
}

static int configure(const struct mrc_ctl_store *store, uint64_t id, int argc, char **argv)
{
    if (argc < 3)
        return -EINVAL;

    struct mrc_ctl_store_config config = {0};
    uint64_t count, min_active;
    if (!strcmp(argv[0], "auto")) {
        if (argc != 3 || !parse_number(argv[1], MRC_EV_MAX_PATHS, &count) ||
            !parse_number(argv[2], count, &min_active))
            return -EINVAL;
        config.mode = MRC_CTL_STORE_AUTO;
    } else {
        if (!strcmp(argv[0], "explicit"))
            config.mode = MRC_CTL_STORE_EXPLICIT;
        else if (!strcmp(argv[0], "generated"))
            config.mode = MRC_CTL_STORE_GENERATED;
        else
            return -EINVAL;
        count = (uint64_t)(argc - 2);
        if (count > MRC_EV_MAX_PATHS || !parse_number(argv[1], count, &min_active))
            return -EINVAL;
        for (unsigned int path = 0; path < count; path++) {
            uint64_t entropy;
            if (!parse_number(argv[2 + path], UINT32_MAX, &entropy))
                return -EINVAL;
            config.entropy[path] = (uint32_t)entropy;
            config.populated |= UINT32_C(1) << path;
        }
    }
    config.count = (uint8_t)count;
    config.min_active = (uint8_t)min_active;
    return mrc_ctl_store_configure(store, id, &config);
}

static int run_command(const struct mrc_ctl_store *store, int argc, char **argv)
{
    const char *command = argv[0];
    if (argc == 1 && !strcmp(command, "list")) {
        struct mrc_ctl_store_profile profiles[MRC_CTL_STORE_MAX_PROFILES];
        size_t count;
        int result = mrc_ctl_store_list(store, profiles, MRC_CTL_STORE_MAX_PROFILES, &count);
        if (result)
            return result;
        for (size_t i = 0; i < count; i++)
            show(&profiles[i]);
        return 0;
    }

    uint64_t id;
    if (argc < 2 || !parse_number(argv[1], UINT64_MAX, &id) || !id)
        return -EINVAL;
    if (!strcmp(command, "configure"))
        return configure(store, id, argc - 2, argv + 2);
    if (!strcmp(command, "deny") || !strcmp(command, "allow")) {
        uint64_t mask;
        if (argc != 3 || !parse_number(argv[2], UINT32_MAX, &mask))
            return -EINVAL;
        if (!strcmp(command, "deny"))
            return mrc_ctl_store_update_denied(store, id, (uint32_t)mask, 0);
        return mrc_ctl_store_update_denied(store, id, 0, (uint32_t)mask);
    }
    if (argc != 2)
        return -EINVAL;
    if (!strcmp(command, "create"))
        return mrc_ctl_store_create(store, id);
    if (!strcmp(command, "online"))
        return mrc_ctl_store_set_state(store, id, MRC_CTL_STORE_ONLINE);
    if (!strcmp(command, "offline"))
        return mrc_ctl_store_set_state(store, id, MRC_CTL_STORE_OFFLINE);
    if (!strcmp(command, "remove"))
        return mrc_ctl_store_remove(store, id);
    if (!strcmp(command, "show")) {
        struct mrc_ctl_store_profile profile;
        int result = mrc_ctl_store_get(store, id, &profile);
        if (!result)
            show(&profile);
        return result;
    }
    return -EINVAL;
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s DIRECTORY COMMAND [ARGS]\n"
                    "  list\n"
                    "  create|show|online|offline|remove ID\n"
                    "  configure ID auto COUNT MIN_ACTIVE\n"
                    "  configure ID explicit|generated MIN_ACTIVE EV [EV ...]\n"
                    "  deny|allow ID EV_MASK\n", program);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    struct mrc_ctl_store store = MRC_CTL_STORE_INITIALIZER;
    int result = mrc_ctl_store_open(&store, argv[1]);
    if (!result)
        result = run_command(&store, argc - 2, argv + 2);
    mrc_ctl_store_close(&store);
    if (result) {
        fprintf(stderr, "MRC EV profile %s failed: %s\n", argv[2], strerror(-result));
        if (result == -EINVAL)
            usage(argv[0]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
