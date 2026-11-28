#include "roce_app.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *device = NULL;
    if (argc == 4 && strcmp(argv[2], "--kernel-mr") == 0) {
        device = argv[3];
    } else if (argc != 2) {
        fprintf(stderr, "usage: roce_app_write SOCKET [--kernel-mr DEVICE]\n");
        return 1;
    }
    if (!roce_app_write(argv[1], device)) {
        perror("application WRITE");
        return 1;
    }
    return 0;
}
