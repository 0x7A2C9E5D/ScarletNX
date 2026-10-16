/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Tests of the PICA200 command processor (src/3ds/gpu/pica.c): command lists written into the board's FCRAM and
   run from their physical address. The expected values come from the layouts Azahar documents (CommandHeader in
   pica_core.h, PackedAttribute's float24 packing, pica_types.h's float24), worked out by hand here. */
#include "3ds/gpu/pica.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FCRAM 0x20000000u

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/* a command list being written: [value, header] pairs, extra words after their header, 8-byte aligned */
typedef struct {
    uint32_t w[256];
    unsigned n;
} List;
static void cmd(List* l, uint32_t id, uint32_t mask, bool group, const uint32_t* v, unsigned count) {
    unsigned i;
    l->w[l->n++] = v[0];
    l->w[l->n++] = id | (mask << 16) | ((count - 1) << 20) | (group ? 1u << 31 : 0);
    for (i = 1; i < count; ++i) l->w[l->n++] = v[i];
    if (l->n % 2) l->w[l->n++] = 0;
}
static void put(List* l, uint32_t id, uint32_t value) { cmd(l, id, 15, false, &value, 1); }

typedef struct {
    unsigned interrupts, draws, indexed, immediates;
} Host;
static void on_interrupt(void* c) { ++((Host*)c)->interrupts; }
static bool on_draw(void* c, SnxCtrPica* p, bool indexed) {
    (void)p;
    ++((Host*)c)->draws;
    ((Host*)c)->indexed += indexed;
    return true;
}
static bool on_immediate(void* c, SnxCtrPica* p) {
    (void)p;
    ++((Host*)c)->immediates;
    return true;
}

static SnxCtrPicaResult run(SnxCtrPica* p, SnxCtrMemory* m, uint32_t pa, const List* l, const SnxCtrPicaHost* h) {
    if (snx_ctr_phys_write(m, pa, l->w, l->n * 4) != SNX_CTR_MEM_OK) return SNX_CTR_PICA_FAULT;
    return snx_ctr_pica_run(p, m, pa, l->n * 4, h);
}

static void float24(void) {
    check(snx_ctr_f24(0x3f0000) == 1.0f && snx_ctr_f24(0xc00000) == -2.0f && snx_ctr_f24(0x3e0000) == 0.5f,
          "float24 normals: bias 63, mantissa above bit 16");
    check(snx_ctr_f24(0) == 0.0f && signbit(snx_ctr_f24(0x800000)), "float24 zeros keep their sign");
    /* exponent 0, mantissa 0x8000: 0.5 * 2^(1-63), renormalized into a float32 normal */
    check(snx_ctr_f24(0x008000) == ldexpf(1.0f, -63), "float24 denormals are converted exactly");
    check(isinf(snx_ctr_f24(0x7f0000)) && isnan(snx_ctr_f24(0x7f0001)), "an all-ones exponent is inf or NaN");
    check(snx_ctr_pica_mul(INFINITY, 0.0f) == 0.0f && isnan(snx_ctr_pica_mul(NAN, 1.0f)) &&
              snx_ctr_pica_mul(3.0f, -2.0f) == -6.0f,
          "PICA multiplication: inf * 0 is 0, NaN stays NaN");
}

static void writes(SnxCtrMemory* m) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    List l = {{0}, 0};
    const uint32_t seq[3] = {0x11, 0x22, 0x33}, oob = 0xffffffffu, masked = 0xaabbccddu;
    snx_ctr_pica_init(p);
    check(p->regs[SNX_PICA_IRQ_AUTOSTOP] == 1 && p->regs[SNX_PICA_IRQ_MASK] == 0xfffffff0u &&
              p->regs[SNX_PICA_IRQ_COMPARE] == 0x12345678u &&
              p->regs[SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG] == 0xa0000001u,
          "registers start as GSP leaves them");
    p->regs[0x100] = 0x01020304u;
    cmd(&l, 0x100, 5, false, &masked, 1); /* bytes 0 and 2 */
    cmd(&l, 0x101, 15, true, seq, 3);     /* 0x101, 0x102, 0x103 */
    cmd(&l, 0x2ff, 15, true, seq, 3);     /* 0x2ff, then two writes past the register file */
    cmd(&l, 0x300, 15, false, &oob, 1);
    check(run(p, m, FCRAM, &l, NULL) == SNX_CTR_PICA_OK, "a list of plain writes runs");
    check(p->regs[0x100] == 0x01bb03ddu, "the parameter mask enables bytes");
    check(p->regs[0x101] == 0x11 && p->regs[0x102] == 0x22 && p->regs[0x103] == 0x33,
          "grouped extra words go to consecutive registers");
    check(p->regs[0x2ff] == 0x11, "writes past the register file are ignored");
    snx_ctr_pica_free(p);
    free(p);
}

static void uploads(SnxCtrMemory* m) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    List l = {{0}, 0};
    /* x 1.0, y 2.0, z -2.0, w 0.5 packed as PackedAttribute reads it: w:z.hi, z.lo:y.hi, y.lo:x */
    const uint32_t f24[3] = {0x3e0000c0u, 0x00004000u, 0x003f0000u};
    const uint32_t code[3] = {0xa, 0xb, 0xc}, lut[2] = {0x55, 0x66};
    uint32_t f32[4];
    float v[4] = {4.0f, 3.0f, 2.0f, 1.0f}; /* w, z, y, x as the words come */
    unsigned i;
    for (i = 0; i < 4; ++i) memcpy(&f32[i], &v[i], 4);
    snx_ctr_pica_init(p);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_SETUP, 5);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, 15, false, f24, 3);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_SETUP, (1u << 31) | 95);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, 15, false, f32, 4);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, 15, false, f32, 4); /* index 96: dropped */
    put(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD, 15, false, code, 3);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_BOOL, 0x7fff8001u);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_INT + 2, 0x04030201u);
    put(&l, SNX_PICA_LIGHTING_LUT_CONFIG, (3u << 8) | 0x10);
    cmd(&l, SNX_PICA_LIGHTING_LUT_DATA, 15, false, lut, 2);
    put(&l, SNX_PICA_FOG_LUT_OFFSET, 127);
    cmd(&l, SNX_PICA_FOG_LUT_DATA, 15, false, lut, 2);
    check(run(p, m, FCRAM, &l, NULL) == SNX_CTR_PICA_OK, "an upload list runs");
    check(p->vs.f[5][0] == 1.0f && p->vs.f[5][1] == 2.0f && p->vs.f[5][2] == -2.0f && p->vs.f[5][3] == 0.5f,
          "a float24 uniform is three words");
    check(p->vs.f[95][0] == 1.0f && p->vs.f[95][3] == 4.0f, "a float32 uniform is four words, x last");
    check((p->regs[SNX_PICA_VS + SNX_PICA_SH_UNIFORM_SETUP] & 0x7f) == 96, "the uniform index stops past 95");
    check(!memcmp(p->gs.f[5], p->vs.f[5], sizeof p->vs.f[5]), "uniforms reach the shared unit");
    check(p->vs.code[0] == 0xa && p->vs.code[2] == 0xc && p->gs.code[1] == 0xb &&
              p->regs[SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET] == 3,
          "program words are uploaded to both units, the offset advances");
    check(p->vs.b == 0x8001 && p->gs.b == 0x8001, "16 bool uniforms");
    check(p->vs.i[2][0] == 1 && p->vs.i[2][3] == 4 && p->gs.i[2][1] == 2, "int uniforms are x y z w bytes");
    check(p->lighting_lut[3][0x10] == 0x55 && p->lighting_lut[3][0x11] == 0x66 &&
              (p->regs[SNX_PICA_LIGHTING_LUT_CONFIG] & 0xff) == 0x12,
          "lighting LUT writes advance the index");
    check(p->fog_lut[127] == 0x55 && p->fog_lut[0] == 0x66, "the fog LUT index wraps at 128");

    /* unit 3 exclusive: the vertex unit's uploads stay its own */
    l.n = 0;
    put(&l, SNX_PICA_GS_EXCLUSIVE, 1);
    put(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    cmd(&l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD, 15, false, lut, 1);
    check(run(p, m, FCRAM, &l, NULL) == SNX_CTR_PICA_OK && p->vs.code[0] == 0x55 && p->gs.code[0] == 0xa,
          "an exclusive geometry unit keeps its program");
    snx_ctr_pica_free(p);
    free(p);
}

static void control(SnxCtrMemory* m) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    Host count = {0, 0, 0, 0};
    SnxCtrPicaHost host = {&count, on_interrupt, on_draw, on_immediate};
    List l = {{0}, 0}, sub = {{0}, 0};
    const uint32_t attr[3] = {0x3e0000c0u, 0x00004000u, 0x003f0000u};
    snx_ctr_pica_init(p);

    /* P3D: a byte of the request equal to the same byte of the compare value; autostop ends the list */
    put(&l, SNX_PICA_IRQ_REQUEST, 0x00000099u);
    put(&l, 0x110, 1);
    put(&l, SNX_PICA_IRQ_REQUEST, 0x00000078u);
    put(&l, 0x111, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK, "an interrupting list runs");
    check(count.interrupts == 1 && p->regs[0x110] == 1 && p->regs[0x111] == 0,
          "a matching request raises P3D and stops the list");

    /* a jump: the second channel's list replaces the rest of this one */
    l.n = 0;
    put(&sub, 0x112, 7);
    put(&l, SNX_PICA_CMDBUF_SIZE + 1, sub.n * 4 / 8);
    put(&l, SNX_PICA_CMDBUF_ADDR + 1, (FCRAM + 0x1000) / 8);
    put(&l, SNX_PICA_CMDBUF_TRIGGER + 1, 1);
    put(&l, 0x113, 1);
    check(snx_ctr_phys_write(m, FCRAM + 0x1000, sub.w, sub.n * 4) == SNX_CTR_MEM_OK, "the sublist is written");
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK && p->regs[0x112] == 7 && p->regs[0x113] == 0,
          "a command buffer trigger jumps and does not return");
    l.n = 0;
    put(&l, SNX_PICA_CMDBUF_SIZE, 1);
    put(&l, SNX_PICA_CMDBUF_ADDR, 0x08000000u / 8); /* nothing at 0x08000000 on the board */
    put(&l, SNX_PICA_CMDBUF_TRIGGER, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_FAULT, "a jump outside memory faults");

    /* draws go to the host, and stop the list without one */
    l.n = 0;
    put(&l, SNX_PICA_TRIGGER_DRAW, 1);
    put(&l, SNX_PICA_TRIGGER_DRAW_INDEXED, 1);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK && count.draws == 2 && count.indexed == 1,
          "draw triggers reach the host");
    check(run(p, m, FCRAM, &l, NULL) == SNX_CTR_PICA_UNSUPPORTED, "a draw without a host stops the list");

    /* default attributes, then immediate mode with two input attributes */
    l.n = 0;
    put(&l, SNX_PICA_DEFAULT_ATTR_INDEX, 3);
    cmd(&l, SNX_PICA_DEFAULT_ATTR_VALUE, 15, false, attr, 3);
    put(&l, SNX_PICA_MAX_INPUT_ATTR, 1);
    put(&l, SNX_PICA_DEFAULT_ATTR_INDEX, 15);
    cmd(&l, SNX_PICA_DEFAULT_ATTR_VALUE, 15, false, attr, 3);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK && count.immediates == 0, "one attribute is no vertex");
    check(p->default_attributes[3][0] == 1.0f && p->default_attributes[3][3] == 0.5f &&
              p->regs[SNX_PICA_DEFAULT_ATTR_INDEX] == 15,
          "a default attribute is stored and the index advances");
    l.n = 0;
    cmd(&l, SNX_PICA_DEFAULT_ATTR_VALUE, 15, false, attr, 3);
    check(run(p, m, FCRAM, &l, &host) == SNX_CTR_PICA_OK && count.immediates == 1 && p->immediate_vertex[1][1] == 2.0f,
          "the last attribute of an immediate vertex draws it");

    /* GPU::SubmitCmdList: a list outside memory raises P3D and runs nothing */
    count.interrupts = 0;
    check(snx_ctr_pica_run(p, m, 0x08000000u, 16, &host) == SNX_CTR_PICA_OK && count.interrupts == 1,
          "an invalid list raises P3D");
    snx_ctr_pica_free(p);
    free(p);
}

int main(void) {
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    if (!m) return 1;
    float24();
    writes(m);
    uploads(m);
    control(m);
    snx_ctr_memory_free(m);
    if (failures) return 1;
    puts("3ds pica: ok");
    return 0;
}
