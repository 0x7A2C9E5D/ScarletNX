// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The texture conversion shader's body (src/nx/gpu/texture/texconv.glsl) run on the CPU through tests/glsl/glsl.hpp,
// compared byte for byte with the CPU's conversions: every 2 byte texel of the three 2 byte formats, every 24 bit
// depth as a float against the CPU's float division, RGB32F, packed depth and stencil in rows not a multiple of the
// four texels an invocation owns, and random ETC2 and EAC blocks of every kind (with the T, H and planar modes made
// often) against src/nx/gpu/texture/etc2.c, in images whose sides aren't multiples of 4, and vi's pixel formats
// against present.c's former CPU conversion. tests/astc_vulkan.c runs the
// SPIR-V build of the same file on a Vulkan device.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "glsl.hpp"
// nx/gpu/texture/etc2.h's functions and kinds (its header brings the recompiler's C helpers, which C++ warns about)
extern "C" void etc2_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
extern "C" uint32_t etc2_block_bytes(int kind);
extern "C" uint32_t etc2_pixel_bytes(int kind);

namespace texconv_shader {
using namespace glsl;
// what texconv.comp declares, under its names
struct Conv : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    const uint* src;
    uint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
} // namespace texconv_shader
using texconv_shader::Conv;

static unsigned failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            if (failures++ < 10) {                                                                                     \
                std::printf("%s:%d: ", __FILE__, __LINE__);                                                            \
                std::printf(__VA_ARGS__);                                                                              \
                std::printf("\n");                                                                                     \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

static uint32_t rng = 0x2545f491;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// vk/texconv.c's shape and push constants for a w x rows x slices conversion from rows of in_pitch bytes; the output's
// row is out_texels long, the stencil's rows follow the depth's
struct Shape {
    uint32_t units, out_texels, out_bpp, out_rows, stencil;
};
static Shape shape(uint32_t mode, uint32_t w) {
    if (mode <= 2 || (mode >= 15 && mode <= 17)) return {(w + 1) / 2, (w + 1) / 2 * 2, 4, 1, 0};
    if (mode >= 18) return {w, w, 4, 1, 0};
    if (mode == 3) return {w, w, 16, 1, 0};
    if (mode <= 7) return {(w + 3) / 4, (w + 3) / 4 * 4, 4, 1, 1};
    return {(w + 3) / 4, (w + 3) / 4 * 4, mode == 11 || mode == 12 ? 2u : 4u, 4, 0};
}
static std::vector<uint32_t> convert(uint32_t mode, uint32_t zfloat, uint32_t w, uint32_t rows, uint32_t slices,
                                     const std::vector<uint32_t>& in, uint32_t in_pitch) {
    Shape s = shape(mode, w);
    uint64_t texels = (uint64_t)s.out_texels * s.out_rows * rows * slices;
    std::vector<uint32_t> out((texels * s.out_bpp + (s.stencil ? texels : 0)) / 4, 0xDEADBEEFu);
    glsl::dispatch<Conv>(glsl::uvec3((s.units + 63) / 64, rows, slices), glsl::uvec3(64, 1, 1), [&](Conv& c) {
        c.mode = mode;
        c.units = s.units;
        c.rows = rows;
        c.in_pitch = in_pitch / 4;
        c.out_pitch = s.out_texels * s.out_bpp / 4;
        c.st_pitch = s.out_texels / 4;
        c.in_bias = 0;
        c.out_bias = 0;
        c.st_bias = (uint32_t)(texels * s.out_bpp / 4);
        c.zfloat = zfloat;
        c.src = in.data();
        c.dst = out.data();
    });
    return out;
}

// texture.c's conversion of a 2 byte texel before it moved to the GPU
static uint32_t ref16(uint32_t mode, uint32_t v) {
    uint32_t r, g, b, a;
    if (mode == 0) {
        r = (v & 15) * 17;
        g = ((v >> 4) & 15) * 17;
        b = ((v >> 8) & 15) * 17;
        a = ((v >> 12) & 15) * 17;
    } else if (mode == 1) {
        r = (v & 31) * 255 / 31;
        g = ((v >> 5) & 31) * 255 / 31;
        b = ((v >> 10) & 31) * 255 / 31;
        a = (v >> 15) ? 255 : 0;
    } else {
        r = (v & 1) ? 255 : 0;
        g = ((v >> 1) & 31) * 255 / 31;
        b = ((v >> 6) & 31) * 255 / 31;
        a = ((v >> 11) & 31) * 255 / 31;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void two_byte_texels() {
    for (uint32_t mode = 0; mode < 3; mode++) {
        Conv c;
        c.mode = mode;
        for (uint32_t v = 0; v < 65536; v++)
            CHECK(c.tc_16to32(v) == ref16(mode, v), "mode %u texel %04x: %08x, not %08x", mode, v, c.tc_16to32(v),
                  ref16(mode, v));
        // an odd row in rows padded to 16 bytes: every texel where it goes, two slices
        uint32_t w = 13, rows = 3, pitch = 32;
        std::vector<uint32_t> in(pitch / 4 * rows * 2);
        for (auto& x : in) x = random_word();
        std::vector<uint32_t> out = convert(mode, 0, w, rows, 2, in, pitch);
        for (uint32_t y = 0; y < rows * 2; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint16_t v;
                std::memcpy(&v, (const uint8_t*)in.data() + y * pitch + x * 2, 2);
                CHECK(out[y * 14 + x] == ref16(mode, v), "mode %u (%u, %u)", mode, x, y);
            }
    }
}

// present.c's conversion of vi's pixel formats before it moved to the GPU
static uint32_t expand(uint32_t v, unsigned bits) { return bits == 4 ? v * 17 : bits == 5 ? v << 3 | v >> 2 : v << 2 | v >> 4; }
static uint32_t to_rgba8(uint32_t fmt, uint32_t p) {
    switch (fmt) {
        case 2: return p | 0xFF000000u;
        case 4: return expand(p >> 11 & 31, 5) | expand(p >> 5 & 63, 6) << 8 | expand(p & 31, 5) << 16 | 0xFF000000u;
        case 5: return (p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16;
        case 6:
            return expand(p >> 11 & 31, 5) | expand(p >> 6 & 31, 5) << 8 | expand(p >> 1 & 31, 5) << 16 |
                   (p & 1 ? 0xFF000000u : 0);
        case 7:
            return expand(p >> 12 & 15, 4) | expand(p >> 8 & 15, 4) << 8 | expand(p >> 4 & 15, 4) << 16 |
                   expand(p & 15, 4) << 24;
    }
    return p;
}
// every 2 byte pixel of the three 2 byte formats, random 4 byte ones, and rows of each through a dispatch
static void vi_frames() {
    const uint32_t fmts[] = {4, 6, 7, 2, 5}, modes[] = {15, 16, 17, 18, 19};
    for (int i = 0; i < 5; i++) {
        uint32_t fmt = fmts[i], mode = modes[i], bpp = i < 3 ? 2 : 4;
        Conv c;
        c.mode = mode;
        if (bpp == 2)
            for (uint32_t v = 0; v < 65536; v++)
                CHECK(c.tc_16to32(v) == to_rgba8(fmt, v), "vi format %u pixel %04x", fmt, v);
        uint32_t w = 15, rows = 4, pitch = 64;
        std::vector<uint32_t> in(pitch / 4 * rows);
        for (auto& x : in) x = random_word();
        std::vector<uint32_t> out = convert(mode, 0, w, rows, 1, in, pitch);
        uint32_t ow = shape(mode, w).out_texels;
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t p = 0;
                std::memcpy(&p, (const uint8_t*)in.data() + y * pitch + x * bpp, bpp);
                CHECK(out[y * ow + x] == to_rgba8(fmt, p), "vi format %u (%u, %u)", fmt, x, y);
            }
    }
}

static void depth_float() {
    Conv c;
    for (uint32_t z = 0; z < 1u << 24; z++) {
        float f = (float)z / 16777215.0f;
        uint32_t want;
        std::memcpy(&want, &f, 4);
        CHECK(c.tc_d24_float(z) == want, "depth %06x: %08x, not %08x", z, c.tc_d24_float(z), want);
    }
}

static void rgb32f() {
    uint32_t w = 7, rows = 5, pitch = 96; // 84 bytes a row, padded to 48
    std::vector<uint32_t> in(pitch / 4 * rows);
    for (auto& x : in) x = random_word();
    std::vector<uint32_t> out = convert(3, 0, w, rows, 1, in, pitch);
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < w; x++)
            for (uint32_t k = 0; k < 4; k++)
                CHECK(out[(y * w + x) * 4 + k] == (k < 3 ? in[y * pitch / 4 + x * 3 + k] : 0x3F800000u), "rgb32f");
}

// format.c's vk_depth_unpack before it moved to the GPU: the texel's depth and stencil
static void ref_depth(uint32_t mode, int zfloat, const uint8_t* t, uint32_t* depth, uint8_t* stencil) {
    uint32_t v;
    std::memcpy(&v, t, 4);
    if (mode == 7) {
        *depth = v;
        *stencil = t[4];
        return;
    }
    uint32_t z = mode == 5 ? v >> 8 : v & 0xFFFFFF;
    *stencil = mode == 6 ? 0 : mode == 5 ? v & 0xFF : v >> 24;
    if (zfloat) {
        float f = (float)z / 16777215.0f;
        std::memcpy(depth, &f, 4);
    } else
        *depth = z;
}
static void depth_stencil() {
    for (uint32_t mode = 4; mode <= 7; mode++)
        for (int zfloat = 0; zfloat < 2; zfloat++) {
            uint32_t bpp = mode == 7 ? 8 : 4, w = 9, rows = 4, slices = 2;
            uint32_t pitch = (w * bpp + 31) / 32 * 32;
            std::vector<uint32_t> in(pitch / 4 * rows * slices);
            for (auto& x : in) x = random_word();
            std::vector<uint32_t> out = convert(mode, mode == 7 ? 0 : (uint32_t)zfloat, w, rows, slices, in, pitch);
            uint32_t ow = 12, st = ow * rows * slices;
            const uint8_t* stencil = (const uint8_t*)(out.data() + st);
            for (uint32_t y = 0; y < rows * slices; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t d;
                    uint8_t s;
                    ref_depth(mode, mode != 7 && zfloat, (const uint8_t*)in.data() + y * pitch + x * bpp, &d, &s);
                    CHECK(out[y * ow + x] == d, "depth mode %u zfloat %d (%u, %u)", mode, zfloat, x, y);
                    CHECK(stencil[y * ow + x] == s, "stencil mode %u (%u, %u)", mode, x, y);
                }
        }
}

// random ETC2 blocks, differential, half of them made to overflow a differential color into the T, H or planar
// mode: a base of 31 plus 3 (0xFB in the color's byte) overflows, a base of 2 plus 0 (0x10) doesn't
static void random_block(uint8_t* b) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)random_word();
    b[3] |= 2;
    uint32_t m = random_word() % 6;
    if (m == 0) b[0] = 0xFB; // red overflows: T
    if (m == 1) b[0] = 0x10, b[1] = 0xFB; // green: H
    if (m == 2) b[0] = 0x10, b[1] = 0x10, b[2] = 0xFB; // blue: planar
}
static void etc2_kinds() {
    const uint32_t sizes[][2] = {{4, 4}, {13, 9}, {1, 1}, {18, 7}};
    for (int kind = 0; kind < 7; kind++)
        for (const auto& sz : sizes) {
            uint32_t w = sz[0], h = sz[1], bw = (w + 3) / 4, bh = (h + 3) / 4, bb = etc2_block_bytes(kind);
            uint32_t pb = etc2_pixel_bytes(kind), pitch = (bw * bb + 15) / 16 * 16;
            std::vector<uint8_t> tight((size_t)bw * bh * bb);
            for (uint32_t i = 0; i < bw * bh * bb / 8; i++) {
                random_block(&tight[i * 8]);
                if (random_word() % 4 == 0) tight[i * 8 + 3] &= ~2; // individual, or punch-through's see-through
            }
            std::vector<uint32_t> in(pitch / 4 * bh);
            for (uint32_t y = 0; y < bh; y++) std::memcpy((uint8_t*)in.data() + y * pitch, &tight[y * bw * bb], bw * bb);
            std::vector<uint8_t> want((size_t)w * h * pb);
            etc2_decode_image(tight.data(), w, h, kind, want.data());
            std::vector<uint32_t> out = convert(8 + kind, 0, w, bh, 1, in, pitch);
            uint32_t ow = bw * 4;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    CHECK(!std::memcmp((const uint8_t*)out.data() + ((size_t)y * ow + x) * pb, &want[((size_t)y * w + x) * pb],
                                       pb),
                          "kind %d %ux%u (%u, %u)", kind, w, h, x, y);
        }
}

int main() {
    two_byte_texels();
    vi_frames();
    depth_float();
    rgb32f();
    depth_stencil();
    for (int i = 0; i < 200; i++) etc2_kinds();
    if (failures) {
        std::printf("texconv_glsl: %u failures\n", failures);
        return 1;
    }
    std::puts("texconv_glsl: the shader's conversions are the CPU's, byte for byte");
    return 0;
}
