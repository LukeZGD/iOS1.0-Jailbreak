//
//  file_payload.c
//  ios1-jb
//
//  Created by Ethan Arbuckle on 3/6/26.
//

#include "file_payload.h"
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <stdio.h>
#include <stdlib.h>


int load_embedded_file(const char *name, payload_t *payload) {
    unsigned long size = 0;
    const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
    uint8_t *data = getsectiondata(header, "__DATA", name, &size);
    if (data == NULL) {
        return 5;
    }

    payload->data = malloc(size);
    if (payload->data == NULL) {
        return 3;
    }

    memcpy(payload->data, data, size);
    payload->size = size;
    return 0;
}

int load_file_from_path(const char *path, payload_t *payload) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "Failed to open file at %s\n", path);
        return 5;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fprintf(stderr, "Failed to get size of file at %s\n", path);
        fclose(f);
        return 5;
    }

    payload->data = malloc((size_t)size);
    if (payload->data == NULL) {
        fprintf(stderr, "Failed to allocate memory for file at %s\n", path);
        fclose(f);
        return 3;
    }

    fread(payload->data, 1, (size_t)size, f);
    fclose(f);
    payload->size = (size_t)size;
    return 0;
}

void payload_free(payload_t *payload) {
    if (payload->data != NULL) {
        free(payload->data);
        payload->data = NULL;
    }

    payload->size = 0;
}

