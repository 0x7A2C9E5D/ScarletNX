/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/objects_pack.h"
#include <stdlib.h>
#include <string.h>

static uint64_t word(const unsigned char* p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

unsigned char* snx_objects_pack(const SnxLinkInput* inputs, size_t count, size_t* size) {
    size_t i, total = 16, at;
    unsigned char* data;
    for (i = 0; i < count; ++i) total += 8 + inputs[i].size;
    data = (unsigned char*)malloc(total);
    if (!data) return NULL;
    memcpy(data, "snx-objs", 8);
    snx_link_put64(data + 8, count);
    for (i = 0, at = 16; i < count; ++i) {
        snx_link_put64(data + at, inputs[i].size);
        memcpy(data + at + 8, inputs[i].data, inputs[i].size);
        at += 8 + inputs[i].size;
    }
    *size = total;
    return data;
}

SnxLinkInput* snx_objects_unpack(const unsigned char* data, size_t size, const char* name, size_t* count) {
    size_t n, i, at = 16;
    SnxLinkInput* inputs;
    if (size < 16 || memcmp(data, "snx-objs", 8)) return NULL;
    n = (size_t)word(data + 8);
    if (!n || n > size / 8) return NULL;
    inputs = (SnxLinkInput*)calloc(n, sizeof(*inputs));
    if (!inputs) return NULL;
    for (i = 0; i < n; ++i) {
        uint64_t length;
        if (size - at < 8 || (length = word(data + at)) > size - at - 8) {
            free(inputs);
            return NULL;
        }
        inputs[i].data = data + at + 8;
        inputs[i].size = (size_t)length;
        inputs[i].name = name;
        at += 8 + (size_t)length;
    }
    *count = n;
    return inputs;
}
