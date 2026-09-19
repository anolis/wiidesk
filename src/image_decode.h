/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_IMAGE_DECODE_H
#define WIIDESK_IMAGE_DECODE_H
#include <limits.h>
#define IMAGE_PIXELS (1024u * 1024u)
struct image_result {
    unsigned width, height;
    int ok;
    char path[PATH_MAX], error[192];
    unsigned char rgb[IMAGE_PIXELS * 3];
};
/* Call in a resource-limited worker. direction is -1, 0 or +1. */
void image_decode(const char *path, int direction, struct image_result *out);
#endif
