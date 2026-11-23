#ifndef ROCE_APP_H
#define ROCE_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct roce_app_memory {
    int socket;
    uint8_t *buffer;
    size_t length;
};

bool roce_app_attach(const char *path, size_t bytes, bool sender, struct roce_app_memory *memory);
void roce_app_detach(struct roce_app_memory *memory);
bool roce_app_write(const char *path, const char *registration_device);

#endif
