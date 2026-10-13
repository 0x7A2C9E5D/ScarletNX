/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED     0x001F8000FC0007E0ull
#define EXIT      0xE30000000007000Full
#define F2I       0x5CB0118000070A00ull
#define FMUL_FTZ  0x5C68100000070000ull
#define FMUL_KEEP 0x5C68000000070000ull
#define FADD_KEEP 0x5C58000000070000ull
#define FMUL_IMM  0x3868003F00070000ull

static int failures;
static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static u32* translate(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words, int flush) {
    u64 code[64] = {0};
    int at = 10;
    for (int i = 0; i <= count; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < count ? instructions[i] : EXIT;
    }
    memset(info, 0, sizeof *info);
    if (stage == ST_CS) info->cs_local[0] = info->cs_local[1] = info->cs_local[2] = 1;
    return shader_to_spirv((const u8*)code, sizeof code, stage, info, words, flush, 1, 1);
}

static int count_opcode(const u32* code, size_t words, u32 opcode, u32 operand) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return -1;
        u32 value_index = op == 17 ? 1 : 2;
        if (op == opcode && (operand == 0xffffffffu || (width > value_index && code[i + value_index] == operand)))
            found++;
        i += width;
    }
    return found;
}

static int has_glsl_abs(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width >= 6 && code[i + 4] == 4) return 1; /* GLSL.std.450 FAbs */
        i += width;
    }
    return 0;
}

static void f2i_modifiers(void) {
    const u64 cases[] = {F2I | (1ull << 45), F2I | (1ull << 49), F2I | (1ull << 45) | (1ull << 49)};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i], 1, ST_FS, &info, &words, 1);
        check(code && !info.unsupported, "F2I modifier translates");
        if (code) {
            check((count_opcode(code, words, 127, 0xffffffffu) > 0) == (i != 1), "F2I bit 45 negates");
            check(has_glsl_abs(code, words) == (i != 0), "F2I bit 49 takes absolute value");
        }
        free(code);
    }
}

static void global_buffers(void) {
    static const struct {
        u64 instructions[3];
        int count;
        u32 offset;
        int write;
    } cases[] = {
        {{0xEF9500007307FF00ull, 0xEED4200000070000ull}, 2, 0x730, 0},
        {{0x4C1080001CC70100ull, 0x4C1008001CD7FF01ull, 0xEEDC200000070002ull}, 3, 0x730, 1},
        {{0x4C98078014470002ull, 0xEED4200000070200ull}, 2, 0x510, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_FS, &info, &words, 1);
        if (!code || info.unsupported || info.ngbuf != 1 || info.gbuf_cbuf_off[0] != cases[i].offset ||
            !!(info.gbuf_wmask & 1) != cases[i].write)
            fprintf(stderr, "global case %zu: %s, buffers %d, offset %x, writes %x\n", i, info.first_unsupported,
                    info.ngbuf, info.gbuf_cbuf_off[0], info.gbuf_wmask);
        check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == cases[i].offset &&
                  !!(info.gbuf_wmask & 1) == cases[i].write,
              "global buffer found at first use");
        free(code);
    }
    const u64 unused[] = {0x4C98078018870002ull};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(unused, 1, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ngbuf == 0, "unused constant is not a global buffer");
    free(code);
}

static void float_controls(void) {
    const u64 cases[][2] = {{FMUL_FTZ, FMUL_FTZ}, {FMUL_FTZ, FADD_KEEP}, {FMUL_KEEP, FMUL_KEEP}};
    for (int i = 0; i < 3; i++) {
        for (int available = 0; available < 2; available++) {
            ShaderInfo info;
            size_t words = 0;
            u32* code = translate(cases[i], 2, ST_FS, &info, &words, available);
            check(code && !info.unsupported, "FP32 mode translates");
            if (code) {
                int flush = i == 0 && available;
                check(info.fp32_flush == (i != 2) && info.fp32_preserve == (i != 0), "FP32 modes recorded");
                check((count_opcode(code, words, 17, 4465) > 0) == flush, "FP32 flush capability");
                int mode = 0;
                for (size_t k = 5; k < words;) {
                    u32 width = code[k] >> 16;
                    if (!width || k + width > words) break;
                    if ((code[k] & 0xffff) == 16 && width >= 4 && code[k + 2] == 4460) mode = 1;
                    k += width;
                }
                check(mode == flush, "FP32 flush execution mode");
            }
            free(code);
        }
    }
}

static void precise_math(void) {
    const u64 math[] = {FMUL_KEEP, FMUL_IMM};
    const int stages[] = {ST_VS, ST_FS, ST_CS};
    for (size_t i = 0; i < sizeof stages / sizeof *stages; i++) {
        int stage = stages[i];
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(math, 2, stage, &info, &words, 1);
        if (!code || info.unsupported) fprintf(stderr, "precise stage %d: %s\n", stage, info.first_unsupported);
        check(code && !info.unsupported, "precise float math translates");
        if (code) check(count_opcode(code, words, 71, 42) >= 2, "float math has NoContraction");
        free(code);
    }
}

int main(void) {
    f2i_modifiers();
    global_buffers();
    float_controls();
    precise_math();
    return failures ? 1 : 0;
}
