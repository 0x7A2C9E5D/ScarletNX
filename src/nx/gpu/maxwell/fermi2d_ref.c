/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Named after Eden (src/video_core/engines/sw_blitter/blitter.cpp, its SoftwareBlitEngine and NearestNeighbor):
   SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project */
/* The CPU reference of a 2D engine blit from memory, as Eden's software blitter does it: done in guest memory, pixel
   by pixel, nearest neighbor, from the source columns and rows the engine worked out (maxwell/fermi2d.c). The tests
   hold the renderer's blit_mem to it (tests/fermi2d_blit.c, tests/vulkan_headless.c). The runtime blits on the GPU
   (vk/blit.c, docs/GPU_OWNERSHIP.md), so this file isn't built into it */
#include "nx/gpu/gpu.h"

/* the largest pixel a blit copies, in bytes (a 128-bit format) */
#define BLIT_MAX_BPP 16u

/* the byte offset of pixel (x, y) in a plane: through the GOBs when block linear, else rows pitch apart */
static u64 plane_pixel_offset(const GpuDmaSide* s, u32 bpp, u32 x, u32 y) {
    return s->bl ? bl_offset(x * bpp, y, s->z, s->wb, s->h, s->bh, s->bd) : (u64)y * s->pitch + (u64)x * bpp;
}
/* each destination pixel of the blit's rectangle from the source pixel its column and row tables name */
void blit_mem_ref(const GpuBlit* b) {
    u32 x, y;
    u8 px[BLIT_MAX_BPP];
    if (b->bpp > BLIT_MAX_BPP) return;
    for (y = b->y0; y < b->y1; y++)
        for (x = b->x0; x < b->x1; x++) {
            u64 from = plane_pixel_offset(&b->src, b->bpp, b->scol[x - b->x0], b->srow[y - b->y0]);
            gmem_read_raw(b->src.addr + from, px, b->bpp);
            gmem_write_raw(b->dst.addr + plane_pixel_offset(&b->dst, b->bpp, x, y), px, b->bpp);
        }
}
