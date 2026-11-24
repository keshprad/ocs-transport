#ifndef ROCE_APP_H
#define ROCE_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct roce_doorbell;
struct roce_lab_options;

struct roce_app_memory {
    int socket;
    uint8_t *buffer;
    size_t length;
    struct roce_doorbell *doorbells;
};

bool roce_app_attach(const char *path, const struct roce_lab_options *options,
                     bool dma_writable, bool in_band_cm, struct roce_app_memory *memory);
bool roce_app_connected(struct roce_app_memory *memory, const struct roce_lab_options *options);
void roce_app_detach(struct roce_app_memory *memory);
bool roce_app_take_writes(struct roce_doorbell *doorbell, uint32_t first, uint32_t count, uint32_t writes);
bool roce_app_write(const char *path, const char *registration_device);

#endif
