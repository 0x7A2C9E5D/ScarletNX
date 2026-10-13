/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_crc32 against the bit-at-a-time definition, both polynomials, every width */
#include "cpu.h"
#include <stdio.h>

static int failures;
static u32 reference(u32 acc, u64 val, int bytes, int castagnoli) {
    u32 poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (int i = 0; i < bytes; i++) {
        acc ^= (u8)(val >> (i * 8));
        for (int k = 0; k < 8; k++) acc = (acc >> 1) ^ ((acc & 1) ? poly : 0);
    }
    return acc;
}
static u64 next(u64* s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

int main(void) {
    /* the standard check values of "123456789" */
    static const u32 check[2] = {0xCBF43926u, 0xE3069283u};
    for (int c = 0; c < 2; c++) {
        u32 acc = 0xFFFFFFFFu;
        for (const char* p = "123456789"; *p; p++) acc = snx_crc32(acc, (u8)*p, 1, c);
        if (~acc != check[c]) {
            fprintf(stderr, "%s check value: %08x, expected %08x\n", c ? "CRC32C" : "CRC32", ~acc, check[c]);
            failures++;
        }
    }
    /* bits above the width must not count */
    static const int widths[4] = {1, 2, 4, 8};
    u64 seed = 0x9E3779B97F4A7C15ull;
    unsigned checks = 2;
    for (int i = 0; i < 20000; i++)
        for (int c = 0; c < 2; c++)
            for (int w = 0; w < 4; w++) {
                u32 acc = (u32)next(&seed);
                u64 val = next(&seed);
                u64 used = widths[w] == 8 ? val : val & ((1ull << (8 * widths[w])) - 1);
                u32 got = snx_crc32(acc, val, widths[w], c), want = reference(acc, used, widths[w], c);
                checks++;
                if (got != want && failures++ < 8)
                    fprintf(stderr, "%s, %d bytes, acc %08x, value %016llx: %08x, expected %08x\n",
                            c ? "CRC32C" : "CRC32", widths[w], acc, (unsigned long long)val, got, want);
            }
    printf("snx_crc32: %u checks, %d failed\n", checks, failures);
    return failures != 0;
}
