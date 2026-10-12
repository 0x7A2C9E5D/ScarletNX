/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/texture/astc.h"
#include <stdio.h>
#include <string.h>

static int check(const u8* block, const u8 expected[4]) {
    u8 pixels[4 * 4 * 4];
    astc_decode_image(block, 4, 4, 4, 4, pixels, 0);
    for (int i = 0; i < 16; i++)
        if (memcmp(pixels + i * 4, expected, 4)) return 0;
    return 1;
}

int main(void) {
    /* LDR void extent: mode bits 0..8 = 0x1fc, bit 9 = 0, reserved bits 10..11 = 11.
       RGBA16 endpoints occupy the last eight bytes. The zero alpha masks the solid color. */
    u8 block[16] = {0xfc, 0x0d, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0x00, 0x00};
    const u8 transparent[4] = {0, 128, 255, 0};
    const u8 invalid[4] = {255, 0, 255, 255};
    if (!check(block, transparent)) {
        fputs("ASTC LDR void extent did not preserve transparent color\n", stderr);
        return 1;
    }
    block[1] &= ~(1u << 2); /* reserved bit 10 must be one */
    if (!check(block, invalid)) {
        fputs("ASTC void extent with reserved bit 10 clear was accepted\n", stderr);
        return 1;
    }
    block[1] |= 1u << 2;
    block[1] &= ~(1u << 3); /* reserved bit 11 must be one */
    if (!check(block, invalid)) {
        fputs("ASTC void extent with reserved bit 11 clear was accepted\n", stderr);
        return 1;
    }
    block[1] |= 1u << 3;
    block[1] |= 1u << 1; /* bit 9 selects unsupported HDR endpoints */
    if (!check(block, invalid)) {
        fputs("ASTC HDR void extent was accepted as LDR\n", stderr);
        return 1;
    }
    puts("PASS ASTC void extent color and invalid modes");
    return 0;
}
