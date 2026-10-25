/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/fermi_2d.cpp, src/video_core/engines/fermi_2d.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 2D engine (FERMI_TWOD_A, 0x902D): a blit from one surface to another, scaled, started by the last word of the
   source origin (FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0_INT). gpu.c's puller hands it its methods (fermi2d_method). The
   blit's configuration is worked out from the registers as Eden's Fermi2D::Blit does (F2dConfig), and the renderer
   does the blit on the GPU: as an image blit when the source is a render target (r3d_blit), else from memory
   (blit_mem, vk/blit.c), with the source pixels the blit samples worked out here, nearest sampled. Register names
   are fermi2d_regs.h's */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/fermi2d_regs.h"
#include "nx/gpu/maxwell/gob.h"

/* A blit's configuration from the pixels_from_memory registers (Eden's Fermi2D::Config): the destination rectangle,
   and the source's origin and its step per destination pixel, in source pixels */
typedef struct {
    int dst_x0, dst_y0, dst_width, dst_height;
    double src_x0, src_y0, du_dx, dv_dy;
} F2dConfig;

/* a Surface's registers from b (FERMI2D_DST or FERMI2D_SRC) */
static void f2d_surface(const u32* r, u32 b, Surf2D* s) {
    s->format = r[b + FERMI2D_SURFACE_FORMAT];
    s->linear = r[b + FERMI2D_SURFACE_LINEAR] & 1;
    s->bh = fermi2d_block_height(r[b + FERMI2D_SURFACE_BLOCK_SIZE]);
    s->depth = r[b + FERMI2D_SURFACE_DEPTH];
    s->layer = r[b + FERMI2D_SURFACE_LAYER];
    s->pitch = r[b + FERMI2D_SURFACE_PITCH];
    s->width = r[b + FERMI2D_SURFACE_WIDTH];
    s->height = r[b + FERMI2D_SURFACE_HEIGHT];
    s->addr = gpu_packed_va(r[b + FERMI2D_SURFACE_ADDR_UPPER], r[b + FERMI2D_SURFACE_ADDR_LOWER]);
}
/* A surface's slice as a plane: a 3D surface's layer is its z, with the block depth of its block size register
   (blk); a 2D array's layer is a whole number of blocks further: its row bytes rounded up to a GOB's width, times its
   rows rounded up to a block's height (8 << bh rows), per layer */
static GpuDmaSide f2d_plane(const Surf2D* s, u32 blk, u32 bpp) {
    GpuDmaSide pl;
    memset(&pl, 0, sizeof pl);
    pl.addr = s->addr;
    pl.bl = s->linear != FERMI2D_MEMORY_LAYOUT_PITCH;
    pl.pitch = s->pitch;
    pl.wb = s->width * bpp; /* a row's bytes; pitch linear rows are pitch apart */
    pl.h = s->height;
    pl.bh = s->bh;
    if (pl.bl) {
        if (s->depth > 1) {
            pl.z = s->layer;
            pl.bd = fermi2d_block_depth(blk);
        } else if (s->layer) {
            u32 block_shift = MAXWELL_GOB_SIZE_Y_SHIFT + s->bh;
            u64 row_bytes = ((u64)pl.wb + MAXWELL_GOB_SIZE_X - 1) & ~(u64)(MAXWELL_GOB_SIZE_X - 1);
            u64 rows = (((u64)s->height + (1u << block_shift) - 1) >> block_shift) << block_shift;
            pl.addr += row_bytes * rows * s->layer;
        }
    }
    return pl;
}
/* the source pixel a destination pixel's center falls on, clamped to the surface's n pixels */
static u32 f2d_sample(double at, u32 n) { return at <= 0 ? 0 : at >= (double)n ? n - 1 : (u32)at; }

/* ---- blits between float formats of different texel sizes, converted on the CPU (as Eden's software blitter does) */
/* a float format's components: their count, and 4 or 2 bytes each. 0 when it isn't one. The X formats' fourth
   component is padding */
static int f2d_float_format(u32 f, u32* n, u32* size) {
    *n = f == 0xC0 || f == 0xC3 || f == 0xCA || f == 0xCE ? 4
         : f == 0xCB || f == 0xDE                         ? 2
         : f == 0xE5 || f == 0xF2                         ? 1
                                                          : 0;
    *size = f == 0xC0 || f == 0xC3 || f == 0xCB || f == 0xE5 ? 4 : 2;
    return *n != 0;
}
static float f2d_from_half(u16 h) {
    u32 sign = (u32)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, x;
    float f;
    if (e == 31)
        x = sign | 0x7F800000 | m << 13;
    else if (e)
        x = sign | (e + 112) << 23 | m << 13;
    else if (!m)
        x = sign;
    else { /* a subnormal, normalized */
        for (e = 113; !(m & 0x400); e--) m <<= 1;
        x = sign | e << 23 | (m & 0x3FF) << 13;
    }
    memcpy(&f, &x, 4);
    return f;
}
/* the half nearest a float, ties to even: too large for one an infinity, NaNs kept */
static u16 f2d_to_half(float f) {
    u32 x, sign, e, m, h, rem, half;
    i32 he;
    memcpy(&x, &f, 4);
    sign = (x >> 16) & 0x8000;
    e = (x >> 23) & 0xFF;
    m = x & 0x7FFFFF;
    if (e == 0xFF) return (u16)(sign | 0x7C00 | (m ? 0x200 | m >> 13 : 0));
    he = (i32)e - 112;
    if (he >= 31) return (u16)(sign | 0x7C00);
    if (he <= 0) { /* a subnormal half, or zero */
        if (he < -10) return (u16)sign;
        m |= 0x800000;
        h = m >> (14 - he);
        rem = m & ((1u << (14 - he)) - 1);
        half = 1u << (13 - he);
    } else {
        h = (u32)he << 10 | m >> 13;
        rem = m & 0x1FFF;
        half = 0x1000;
    }
    if (rem > half || (rem == half && (h & 1))) h++; /* a carry into the exponent rounds up to it, or to infinity */
    return (u16)(sign | h);
}
/* the byte offset of byte column xb, row y in a plane (f2d_plane's): pitch linear rows, or the 2D block linear
   layout, GOBs of 64 bytes by 8 rows in blocks 1 << bh GOBs high, one row of blocks after another (Eden's
   GetSwizzleOffset) */
static u64 f2d_offset(const GpuDmaSide* p, u32 xb, u32 y) {
    u32 gobs_x = (p->wb + MAXWELL_GOB_SIZE_X - 1) >> MAXWELL_GOB_SIZE_X_SHIFT, block_rows = MAXWELL_GOB_SIZE_Y << p->bh;
    u64 block;
    if (!p->bl) return (u64)y * p->pitch + xb;
    block = ((u64)(y / block_rows) * gobs_x + (xb >> MAXWELL_GOB_SIZE_X_SHIFT)) << (MAXWELL_GOB_SIZE_SHIFT + p->bh);
    return block + ((u64)((y % block_rows) >> MAXWELL_GOB_SIZE_Y_SHIFT) << MAXWELL_GOB_SIZE_SHIFT) +
           (xb % 64 / 32) * 256 + (y % 8 / 2) * 64 + (xb % 32 / 16) * 32 + (y % 2) * 16 + xb % 16;
}
/* a plane's bytes from its address */
static u64 f2d_span(const GpuDmaSide* p) {
    u32 block_rows = MAXWELL_GOB_SIZE_Y << p->bh;
    if (!p->bl) return (u64)p->pitch * (p->h ? p->h - 1 : 0) + p->wb;
    return ((u64)((p->wb + MAXWELL_GOB_SIZE_X - 1) >> MAXWELL_GOB_SIZE_X_SHIFT) *
            ((p->h + block_rows - 1) / block_rows))
           << (MAXWELL_GOB_SIZE_SHIFT + p->bh);
}
/* b between float formats sf and df of different texel sizes, which the renderer's blit from memory doesn't convert:
   each destination pixel of the rectangle the source pixel the tables pick, its components converted, a missing one
   0 and a missing alpha 1. Through gmem, so what the GPU holds of either surface lands first and what is built from the
   destination takes the write. One game converts the RGBA32F tables its CPU fills into the RGBA16F texture its lighting
   reads, which stayed zero, at each level's start. 0 for a 3D surface, or one past 64 MB */
static int f2d_blit_convert(const GpuBlit* b, u32 sf, u32 df) {
    u32 sn, ss, dn, ds, bs, bd, x, y, k;
    u64 sspan = f2d_span(&b->src), dspan = f2d_span(&b->dst);
    u8 *sp, *dp;
    f2d_float_format(sf, &sn, &ss);
    f2d_float_format(df, &dn, &ds);
    bs = sn * ss;
    bd = dn * ds;
    if (b->src.bd || b->dst.bd || b->src.z || b->dst.z || sspan > (64u << 20) || dspan > (64u << 20)) return 0;
    sp = (u8*)malloc(sspan);
    dp = (u8*)malloc(dspan);
    if (!sp || !dp) {
        free(sp);
        free(dp);
        return 0;
    }
    gmem_read(b->src.addr, sp, sspan); /* an unmapped page reads as zero, and takes no write */
    gmem_read(b->dst.addr, dp, dspan); /* the bytes around the rectangle stay */
    for (y = b->y0; y < b->y1; y++)
        for (x = b->x0; x < b->x1; x++) {
            const u8* in = sp + f2d_offset(&b->src, b->scol[x - b->x0] * bs, b->srow[y - b->y0]);
            u8* out = dp + f2d_offset(&b->dst, x * bd, y);
            float v[4] = {0, 0, 0, 1};
            for (k = 0; k < sn; k++) {
                if (ss == 2) {
                    v[k] = f2d_from_half(snx_ld_le16(in + 2 * k));
                    continue;
                }
                u32 w = snx_ld_le32(in + 4 * k);
                memcpy(&v[k], &w, 4);
            }
            if (sf == 0xC3 || sf == 0xCE) v[3] = 1;
            for (k = 0; k < dn; k++) {
                if (ds == 2) {
                    snx_st_le16(out + 2 * k, f2d_to_half(v[k]));
                    continue;
                }
                u32 w;
                memcpy(&w, &v[k], 4);
                snx_st_le32(out + 4 * k, w);
            }
        }
    gmem_write(b->dst.addr, dp, dspan);
    free(sp);
    free(dp);
    return 1;
}

/* The blit from memory, which the renderer does on the GPU (its destination kept there): which source column and row
   each destination column and row takes, nearest sampled, worked out here. The same bytes per pixel copy (the bytes as
   they are when the formats differ), float formats of different sizes are converted on the CPU (f2d_blit_convert) */
static void f2d_blit_memory(const u32* r, const Surf2D* s, const Surf2D* d, const F2dConfig* c) {
    u32 bs = gpu_color_bpp(s->format), bd = gpu_color_bpp(d->format), n, i, y0, y1, x0, x1, cn, cs;
    int convert = bs != bd;
    GpuBlit b;
    u32* tab;
    if (convert && (!f2d_float_format(s->format, &cn, &cs) || !f2d_float_format(d->format, &cn, &cs))) {
        WARN_LIMIT(4, "gpu: a 2D blit from format %x to %x isn't done\n", s->format, d->format);
        return;
    }
    if (!convert && s->format != d->format)
        LOG_LIMIT(4, "gpu: a 2D blit from format %x to %x copies the bytes\n", s->format, d->format);
    if (!s->width || !s->height) return;
    /* the destination rectangle clipped to the destination surface */
    x0 = c->dst_x0 > 0 ? (u32)c->dst_x0 : 0;
    y0 = c->dst_y0 > 0 ? (u32)c->dst_y0 : 0;
    x1 = (i64)c->dst_x0 + c->dst_width < (i64)d->width ? (u32)((i64)c->dst_x0 + c->dst_width) : d->width;
    y1 = (i64)c->dst_y0 + c->dst_height < (i64)d->height ? (u32)((i64)c->dst_y0 + c->dst_height) : d->height;
    if (x0 >= x1 || y0 >= y1) return;
    n = x1 - x0;
    tab = (u32*)malloc(sizeof *tab * ((size_t)n + (y1 - y0)));
    if (!tab) {
        WARN_LIMIT(1, "gpu: no memory for a 2D blit %u pixels wide\n", n);
        return;
    }
    for (i = 0; i < n; i++)
        tab[i] = f2d_sample(c->src_x0 + ((double)(x0 + i - c->dst_x0) + 0.5) * c->du_dx, s->width);
    for (i = y0; i < y1; i++)
        tab[n + i - y0] = f2d_sample(c->src_y0 + ((double)(i - c->dst_y0) + 0.5) * c->dv_dy, s->height);
    b.src = f2d_plane(s, r[FERMI2D_SRC + FERMI2D_SURFACE_BLOCK_SIZE], bs);
    b.dst = f2d_plane(d, r[FERMI2D_DST + FERMI2D_SURFACE_BLOCK_SIZE], bd);
    b.bpp = bs;
    b.x0 = x0, b.x1 = x1, b.y0 = y0, b.y1 = y1;
    b.scol = tab;
    b.srow = tab + n;
    if (convert) {
        if (g_renderer_up && g_renderer->rt_flush) g_renderer->rt_flush(s->addr);
        if (!f2d_blit_convert(&b, s->format, d->format))
            WARN_LIMIT(4, "gpu: a 2D blit from format %x to %x of a 3D or very large surface isn't done\n", s->format,
                       d->format);
        else if (g_renderer_up && g_renderer->rt_reload)
            g_renderer->rt_reload(d->addr);
    } else if (!g_renderer_up || !g_renderer->blit_mem)
        WARN_LIMIT(4, "gpu: a 2D blit before the renderer started, skipped\n");
    else if (!g_renderer->blit_mem(&b))
        WARN_LIMIT(4, "gpu: the GPU could not do a 2D blit, skipped\n");
    else if (g_renderer->rt_reload)
        g_renderer->rt_reload(d->addr);
    free(tab);
}
/* a signed 32.32 fixed point value from its two registers, the fraction's word first */
static double f2d_fixed(const u32* r, u32 lo) {
    return (double)(i64)(((u64)r[lo + 1] << 32) | r[lo]) / FERMI2D_FIXED_ONE;
}
/* The blit's configuration (Eden's Fermi2D::Blit). With the CORNER origin a destination pixel samples the start of
   its step and with CENTER its middle, as the renderer samples: a corner's start is half a step back (Eden takes off
   the step's integer half, (du_dx >> 33) << 32; the exact half is taken here) */
static void f2d_config(const u32* r, F2dConfig* c) {
    c->dst_x0 = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_X0];
    c->dst_y0 = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_Y0];
    c->dst_width = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_WIDTH];
    c->dst_height = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_HEIGHT];
    c->du_dx = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_DU_DX);
    c->dv_dy = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_DV_DY);
    c->src_x0 = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_SRC_X0);
    c->src_y0 = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0);
    if (fermi2d_sample_mode_origin(r[FERMI2D_PIXELS_FROM_MEMORY_SAMPLE_MODE]) == FERMI2D_ORIGIN_CORNER) {
        c->src_x0 -= c->du_dx / 2;
        c->src_y0 -= c->dv_dy / 2;
    }
}
/* The blit (Eden's Fermi2D::Blit): the renderer's image blit when it takes it, else the blit from memory. Every
   operation is done as SRC_COPY */
static void f2d_blit(GpuChannel* ch) {
    const u32* r = ch->r2d;
    u32 op = r[FERMI2D_OPERATION];
    Surf2D s, d;
    F2dConfig c;
    f2d_config(r, &c);
    f2d_surface(r, FERMI2D_DST, &d);
    f2d_surface(r, FERMI2D_SRC, &s);
    if (c.dst_width <= 0 || c.dst_height <= 0) return;
    if (op != FERMI2D_OPERATION_SRC_COPY && op != FERMI2D_OPERATION_SRC_COPY_AND)
        LOG_LIMIT(4, "gpu: a 2D blit's operation %u is done as a copy\n", op);
    if (r3d_blit(&s, &d, c.dst_x0, c.dst_y0, c.dst_width, c.dst_height, c.src_x0, c.src_y0, c.du_dx, c.dv_dy)) return;
    f2d_blit_memory(r, &s, &d, &c);
}
/* a 2D engine method (Eden's Fermi2D::CallMethod): stored, and the blit started by its last register */
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= FERMI2D_NUM_REGS) return;
    ch->r2d[m] = v;
    if (m == FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0_INT) f2d_blit(ch);
}
