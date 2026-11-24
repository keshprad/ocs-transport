#include "roce_cm_lab_offer.h"

#include <arpa/inet.h>
#include <string.h>

enum { CM_LAB_MAGIC = 0x52434d31, CM_LAB_WORDS = ROCE_CM_LAB_OFFER_SIZE / sizeof(uint32_t),
       CM_LAB_MULTI_MR = 1u << 1 };

size_t roce_cm_lab_offer_encode(uint8_t *data, size_t capacity, const struct roce_lab_options *options,
                                const struct roce_lab_offer *local, const void *app_data, uint8_t app_length)
{
    bool auxiliary = options->multi_mr_targets && !options->sender;
    size_t base_length = ROCE_CM_LAB_OFFER_SIZE + (app_length ? 1 + app_length : 0);
    size_t length = auxiliary ? ROCE_CM_LAB_REPLY_SIZE : base_length;
    if (app_length > ROCE_CM_LAB_MAX_APP_DATA || (app_length && !app_data) || length > capacity)
        return 0;
    const uint32_t words[CM_LAB_WORDS] = {
        htonl(CM_LAB_MAGIC), htonl(options->sender | (options->multi_mr_targets ? CM_LAB_MULTI_MR : 0)),
        htonl(options->size), htonl(options->writes),
        htonl((uint32_t)(local->region.address >> 32)), htonl((uint32_t)local->region.address),
        htonl(local->region.rkey), htonl(local->pmtu),
    };
    memcpy(data, words, sizeof(words));
    if (app_length) {
        data[sizeof(words)] = app_length;
        memcpy(data + sizeof(words) + 1, app_data, app_length);
    }
    if (auxiliary) {
        memset(data + base_length, 0, ROCE_CM_LAB_PRIVATE_SIZE - base_length);
        if (!roce_lab_encode_auxiliary(data + ROCE_CM_LAB_PRIVATE_SIZE, local, options))
            return 0;
    }
    return length;
}

bool roce_cm_lab_offer_decode(const void *data, size_t length, const struct roce_lab_options *options,
                              struct roce_lab_offer *peer, const uint8_t **app_data, uint8_t *app_length)
{
    uint32_t words[CM_LAB_WORDS];
    if (!data || length < sizeof(words))
        return false;
    memcpy(words, data, sizeof(words));
    for (size_t i = 0; i < CM_LAB_WORDS; ++i)
        words[i] = ntohl(words[i]);

    uint64_t address = (uint64_t)words[4] << 32 | words[5];
    uint64_t bytes = (uint64_t)options->size * options->writes;
    uint32_t role = (uint32_t)!options->sender | (options->multi_mr_targets ? CM_LAB_MULTI_MR : 0);
    if (words[0] != CM_LAB_MAGIC || words[1] != role ||
        words[2] != options->size || words[3] != options->writes || !bytes ||
        bytes - 1 > UINT64_MAX - address || words[7] > ROCE_MAX_PMTU ||
        !roce_valid_pmtu((uint16_t)words[7]))
        return false;

    const uint8_t *extra = (const uint8_t *)data + sizeof(words);
    uint8_t extra_length = length > sizeof(words) ? *extra : 0;
    if (extra_length > ROCE_CM_LAB_MAX_APP_DATA ||
        (extra_length && extra_length > length - sizeof(words) - 1))
        return false;

    peer->pmtu = (uint16_t)words[7];
    peer->region = (struct roce_region) { .address = address, .rkey = words[6], .size = words[2] };
    peer->auxiliary = (struct roce_region){0};
    if (options->multi_mr_targets && options->sender &&
        (length < ROCE_CM_LAB_REPLY_SIZE || !roce_lab_decode_auxiliary((const uint8_t *)data + ROCE_CM_LAB_PRIVATE_SIZE,
                                                                    peer, options)))
        return false;
    if (app_data)
        *app_data = extra_length ? extra + 1 : NULL;
    if (app_length)
        *app_length = extra_length;
    return true;
}
