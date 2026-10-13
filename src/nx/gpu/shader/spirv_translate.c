/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Direct Maxwell to SPIR-V translation. Unsupported instructions reject the entire module. */
#include "gpu/shader/spirv_emit.h"
#include "gpu/shader/translator.h"
#include <stdlib.h>

typedef struct {
    SpvEmitter emit;
    u32 u32_type, f32_type, bool_type, zero, true_value, false_value, glsl;
    u32 reg[255];
    u32 pred[7];
    u32 cbuf[18], cbuf_pointer[18];
    u32 scale_buffer, scale_pointer;
    u32 sampler[32], sampled_type[32];
} DirectCtx;

static u32 reg_read(DirectCtx* c, u32 r) {
    return r >= 255 ? c->zero : spv_emit_load(&c->emit, c->u32_type, c->reg[r]);
}

static void reg_write(DirectCtx* c, u32 r, u32 value) {
    if (r < 255) spv_emit_store(&c->emit, c->reg[r], value);
}

static u32 pred_read(DirectCtx* c, u32 p, int negative) {
    u32 value = p == 7 ? c->true_value : spv_emit_load(&c->emit, c->bool_type, c->pred[p]);
    return negative ? spv_emit_bool_not(&c->emit, c->bool_type, value) : value;
}

static void pred_write(DirectCtx* c, u32 p, u32 value) {
    if (p < 7) spv_emit_store(&c->emit, c->pred[p], value);
}

static u32 bool_combine(DirectCtx* c, u32 op, u32 left, u32 right) {
    return op < 3 ? spv_emit_bool_binary(&c->emit, c->bool_type, op, left, right) : left;
}

static u32 float_mod(DirectCtx* c, u32 value, int absolute, int negative) {
    if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FABS, value);
    if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
    return value;
}

static u32 float_imm20(DirectCtx* c, u64 w) {
    return spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
}

static u32 float_imm32(DirectCtx* c, u64 w) { return spv_emit_constant_u32(&c->emit, c->f32_type, B(w, 20, 32)); }

static u32 float_saturate(DirectCtx* c, u32 value, int saturate) {
    if (!saturate) return value;
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
    return spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FCLAMP, value, zero, one);
}

static void alpha_test(DirectCtx* c, u32 push, u32 push_pointer, u32 alpha) {
    u32 index[3];
    for (u32 i = 0; i < 3; i++) index[i] = spv_emit_constant_u32(&c->emit, c->u32_type, i);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[0], c->f32_type);
    u32 function = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[1], c->f32_type);
    u32 reference = spv_emit_push_component_f32(&c->emit, push_pointer, push, index[0], index[2], c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 active = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 pass =
        spv_emit_fcompare(&c->emit, c->bool_type, 2, function, spv_emit_constant_f32(&c->emit, c->f32_type, 7.0f));
    for (u32 mode = 1; mode <= 6; mode++) {
        u32 tag = spv_emit_constant_f32(&c->emit, c->f32_type, (float)mode);
        u32 selected = spv_emit_fcompare(&c->emit, c->bool_type, 2, function, tag);
        u32 comparison = spv_emit_fcompare(&c->emit, c->bool_type, mode, alpha, reference);
        u32 result = spv_emit_bool_binary(&c->emit, c->bool_type, 0, selected, comparison);
        pass = spv_emit_bool_binary(&c->emit, c->bool_type, 1, pass, result);
    }
    u32 fail = spv_emit_bool_not(&c->emit, c->bool_type, pass);
    fail = spv_emit_bool_binary(&c->emit, c->bool_type, 0, active, fail);
    u32 discard = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, fail, discard, merge);
    spv_emit_label(&c->emit, discard);
    spv_emit_kill(&c->emit);
    spv_emit_label(&c->emit, merge);
}

static void vertex_depth_fix(DirectCtx* c, u32 position, u32 push, u32 push_pointer, u32 vec4_type) {
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, 1);
    u32 enabled = spv_emit_push_component_f32(&c->emit, push_pointer, push, member, component, c->f32_type);
    u32 zero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 test = spv_emit_fcompare(&c->emit, c->bool_type, 5, enabled, zero);
    u32 adjust = spv_emit_id(&c->emit), merge = spv_emit_id(&c->emit);
    spv_emit_selection_merge(&c->emit, merge);
    spv_emit_branch_conditional(&c->emit, test, adjust, merge);
    spv_emit_label(&c->emit, adjust);
    u32 value = spv_emit_load(&c->emit, vec4_type, position);
    u32 z = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 2);
    u32 w = spv_emit_vec4_extract(&c->emit, c->f32_type, value, 3);
    u32 sum = spv_emit_fadd(&c->emit, c->f32_type, z, w);
    spv_emit_no_contraction(&c->emit, sum);
    u32 half = spv_emit_constant_f32(&c->emit, c->f32_type, 0.5f);
    u32 depth = spv_emit_fmul(&c->emit, c->f32_type, sum, half);
    spv_emit_no_contraction(&c->emit, depth);
    value = spv_emit_vec4_insert(&c->emit, vec4_type, value, depth, 2);
    spv_emit_store(&c->emit, position, value);
    spv_emit_branch(&c->emit, merge);
    spv_emit_label(&c->emit, merge);
}

static u32 cbuf_read(DirectCtx* c, u64 w) {
    u32 idx = B(w, 34, 5), off = B(w, 20, 14) * 4;
    u32 member = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 element = spv_emit_constant_u32(&c->emit, c->u32_type, off >> 4);
    u32 component = spv_emit_constant_u32(&c->emit, c->u32_type, (off >> 2) & 3);
    return spv_emit_uniform_component_f32(&c->emit, c->cbuf_pointer[idx], c->cbuf[idx], member, element, component,
                                          c->f32_type);
}

static int texs_outputs(u64 w, u32 regs[4], u32 comps[4]) {
    u32 da = B(w, 0, 8), db = B(w, 28, 8), sw = B(w, 50, 3);
    int n = 0;
    if (db == 255) {
        static const int tab[8][2] = {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {0, 1}, {0, 3}, {1, 3}, {2, 3}};
        for (int i = 0; i < 2; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = da == 255 ? 255 : da + (u32)n;
                comps[n++] = (u32)tab[sw][i];
            }
    } else {
        static const int tab[5][4] = {{0, 1, 2, -1}, {0, 1, 3, -1}, {0, 2, 3, -1}, {1, 2, 3, -1}, {0, 1, 2, 3}};
        if (sw > 4) sw = 4;
        for (int i = 0; i < 4; i++)
            if (tab[sw][i] >= 0) {
                regs[n] = n < 2 ? da + (u32)n : db + (u32)(n - 2);
                comps[n++] = (u32)tab[sw][i];
            }
    }
    return n;
}

static u32 next_instruction(u32 index, u32 limit) {
    do { index++; } while (index < limit && !IDX_OK(index));
    return index;
}

static int supported(u64 w, int op) {
    switch (op) {
        case O_EXIT: return B(w, 0, 5) == 15;
        case O_NOP:
        case O_DEPBAR:
        case O_VOTE_VTG: return 1;
        case O_S2R: {
            u32 sr = B(w, 20, 8);
            return sr == 0x1e || sr == 0x1f || sr == 0 || sr == 0x12 || sr == 0x38 || sr == 0x39;
        }
        case O_ALD: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            return B(w, 8, 8) == 255 && !B(w, 32, 1) && addr >= 0x80 && addr + count * 4 <= 0x280 &&
                   B(w, 0, 8) + count <= 255;
        }
        case O_AST: {
            u32 addr = B(w, 20, 10), count = B(w, 47, 2) + 1;
            return B(w, 8, 8) == 255 && B(w, 0, 8) + count <= 255 &&
                   ((addr >= 0x70 && addr + count * 4 <= 0x80) || (addr >= 0x80 && addr + count * 4 <= 0x280));
        }
        case O_KIL: return B(w, 0, 5) == 15;
        case O_BRA: return B(w, 0, 5) == 15;
        case O_SSY:
        case O_PBK:
        case O_PCNT: return 1;
        case O_SYNC:
        case O_BRK:
        case O_CONT: return B(w, 0, 5) == 15;
        case O_MOV32I: return B(w, 12, 4) == 1 || B(w, 12, 4) == 15;
        case O_MOV_R:
        case O_MOV_C: return B(w, 39, 4) == 1 || B(w, 39, 4) == 15;
        case O_FADD_R:
        case O_FADD_C:
        case O_FADD_I: return !B(w, 47, 1);
        case O_FADD32I: return !B(w, 52, 1);
        case O_FMUL_R:
        case O_FMUL_C:
        case O_FMUL_I: return B(w, 44, 2) <= 1 && !B(w, 47, 1) && B(w, 41, 3) < 7;
        case O_FMUL32I: return B(w, 53, 2) <= 1 && !B(w, 52, 1);
        case O_FFMA_R:
        case O_FFMA_RC:
        case O_FFMA_CR:
        case O_FFMA_I: return B(w, 53, 2) <= 1 && !B(w, 47, 1);
        case O_FMNMX_R:
        case O_FMNMX_C:
        case O_FMNMX_I: return !B(w, 47, 1);
        case O_IPA: {
            u32 addr = B(w, 30, 8) * 4;
            return ((addr >= 0x80 && addr < 0x280) || (addr >= 0x70 && addr < 0x80)) && !B(w, 38, 1) && !B(w, 51, 1);
        }
        case O_MUFU:
        case O_RRO_R:
        case O_RRO_C: return 1;
        case O_TEXS: return (B(w, 53, 4) == 1 || B(w, 53, 4) == 4) && B(w, 59, 1);
        case O_FSETP_R:
        case O_FSETP_C:
        case O_FSETP_I: return 1;
        case O_FSET_R:
        case O_FSET_C:
        case O_FSET_I: return !B(w, 47, 1);
        default: return 0;
    }
}

u32* shader_to_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count) {
    if (!code || !si || !word_count || max_bytes < 96 || (stage != ST_FS && stage != ST_VS)) return NULL;
    if ((stage == ST_FS && si->rt_int_mask) || (stage == ST_VS && si->attr_int_mask)) return NULL;
    if (!ops_ready) ops_init();
    u32 len;
    shader_hash(code, max_bytes, &len);
    u32 n = len / 8;
    const u64* insn = (const u64*)code;
    u32 sph[20];
    memcpy(sph, code, sizeof sph);
    u32 target = sph[18];
    if (stage == ST_FS && sph[19]) {
        snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", "depth or sample-mask output");
        si->unsupported = 1;
        return NULL;
    } /* depth and sample-mask outputs follow the colours */
    u8 used[255] = {0};
    u8 pred_used[7] = {0};
    u8 inputs_used[32] = {0};
    u8 outputs_used[32] = {0};
    int position_used = 0;
    u8 cbuf_used[18] = {0};
    u32 cbuf_max[18] = {0};
    ShTex textures[32] = {0};
    int texture_count = 0;
    int needs_fragcoord = 0;
    int needs_scale = 0;
    int needs_glsl = 0;
    int has_flow = 0;
    int has_stack = 0;
    int fp32_flush = 0, fp32_preserve = 0;
    u32 out_reg = 0;
    if (stage == ST_FS)
        for (int rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                if (out_reg > 251) return NULL;
                for (int k = 0; k < 4; k++)
                    if (target & (1u << (rt * 4 + k))) used[out_reg + k] = 1;
                out_reg += 4;
            }
    int has_exit = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(insn[i]);
        if (!op || !supported(insn[i], op->id) || (stage != ST_VS && (op->id == O_ALD || op->id == O_AST)) ||
            (stage != ST_FS && op->id == O_IPA)) {
            snprintf(si->first_unsupported, sizeof si->first_unsupported, "%s", op ? op->name : "unknown opcode");
            si->unsupported = 1;
            return NULL;
        }
        if (op->id == O_BRA) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return NULL;
            has_flow = 1;
        }
        if (op->id == O_SSY || op->id == O_PBK || op->id == O_PCNT) {
            u32 target_index = bra_target(insn[i], i);
            if (target_index >= n || !IDX_OK(target_index)) return NULL;
            has_flow = has_stack = 1;
        }
        if ((op->id >= O_FADD_R && op->id <= O_FFMA_I) && op->id != O_FMNMX_R && op->id != O_FMNMX_C &&
            op->id != O_FMNMX_I) {
            u32 mode = op->id == O_FADD32I                        ? B(insn[i], 55, 1)
                       : op->id >= O_FADD_R && op->id <= O_FADD_I ? B(insn[i], 44, 1)
                       : op->id >= O_FMUL_R && op->id <= O_FMUL_I ? B(insn[i], 44, 2)
                                                                  : B(insn[i], 53, 2);
            if (mode)
                fp32_flush = 1;
            else
                fp32_preserve = 1;
        }
        u32 d = B(insn[i], 0, 8), a = B(insn[i], 8, 8), b = B(insn[i], 20, 8);
        u32 guard = B(insn[i], 16, 3);
        if (guard < 7) pred_used[guard] = 1;
        if (d < 255 && op->id != O_EXIT && op->id != O_KIL && op->id != O_BRA && op->id != O_SSY && op->id != O_PBK &&
            op->id != O_PCNT && op->id != O_SYNC && op->id != O_BRK && op->id != O_CONT && op->id != O_AST &&
            op->id != O_NOP && op->id != O_VOTE_VTG && op->id != O_DEPBAR && op->id != O_FSETP_R &&
            op->id != O_FSETP_C && op->id != O_FSETP_I)
            used[d] = 1;
        if (op->id == O_ALD || op->id == O_AST) {
            u32 addr = B(insn[i], 20, 10), width = B(insn[i], 47, 2) + 1;
            for (u32 k = 0; k < width; k++) {
                used[d + k] = 1;
                u32 location = addr + k * 4;
                if (op->id == O_ALD)
                    inputs_used[(location - 0x80) >> 4] = 1;
                else if (location >= 0x80)
                    outputs_used[(location - 0x80) >> 4] = 1;
                else
                    position_used = 1;
            }
        }
        if (op->id == O_MOV_R || op->id == O_FADD_R || op->id == O_FMUL_R || op->id == O_FFMA_R ||
            op->id == O_FMNMX_R || op->id == O_FSET_R || op->id == O_RRO_R) {
            if (b < 255) used[b] = 1;
        }
        if ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I || op->id == O_FADD32I ||
             op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I || op->id == O_FMUL32I ||
             op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I ||
             op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I || op->id == O_FSET_R ||
             op->id == O_FSET_C || op->id == O_FSET_I || op->id == O_MUFU) &&
            a < 255)
            used[a] = 1;
        if ((op->id == O_FFMA_R || op->id == O_FFMA_RC || op->id == O_FFMA_CR || op->id == O_FFMA_I) &&
            B(insn[i], 39, 8) < 255)
            used[B(insn[i], 39, 8)] = 1;
        if (op->id == O_MUFU || op->id == O_RRO_R || op->id == O_RRO_C || op->id == O_FFMA_R || op->id == O_FFMA_RC ||
            op->id == O_FFMA_CR || op->id == O_FFMA_I || op->id == O_FMNMX_R || op->id == O_FMNMX_C ||
            op->id == O_FMNMX_I ||
            ((op->id == O_FADD_R || op->id == O_FADD_C || op->id == O_FADD_I) &&
             (B(insn[i], 46, 1) || B(insn[i], 49, 1) || B(insn[i], 50, 1))) ||
            (op->id == O_FADD32I && B(insn[i], 54, 1)) ||
            ((op->id == O_FMUL_R || op->id == O_FMUL_C || op->id == O_FMUL_I) && B(insn[i], 50, 1)) ||
            (op->id == O_FMUL32I && B(insn[i], 55, 1)))
            needs_glsl = 1;
        if (op->id == O_MOV_C || op->id == O_FADD_C || op->id == O_FMUL_C || op->id == O_FFMA_RC ||
            op->id == O_FFMA_CR || op->id == O_FMNMX_C || op->id == O_RRO_C || op->id == O_FSETP_C ||
            op->id == O_FSET_C) {
            u32 idx = B(insn[i], 34, 5), off = B(insn[i], 20, 14) * 4;
            if (idx >= 18) return NULL;
            cbuf_used[idx] = 1;
            if ((off | 15u) + 1 > cbuf_max[idx]) cbuf_max[idx] = (off | 15u) + 1;
        }
        if (op->id == O_FSETP_R || op->id == O_FSETP_C || op->id == O_FSETP_I) {
            if (a < 255) used[a] = 1;
            if (op->id == O_FSETP_R && b < 255) used[b] = 1;
            u32 pc = B(insn[i], 39, 3), pa = B(insn[i], 3, 3), pb = B(insn[i], 0, 3);
            if (pc < 7) pred_used[pc] = 1;
            if (pa < 7) pred_used[pa] = 1;
            if (pb < 7) pred_used[pb] = 1;
            needs_glsl = 1;
        }
        if (op->id == O_FMNMX_R || op->id == O_FMNMX_C || op->id == O_FMNMX_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
        }
        if (op->id == O_FSET_R || op->id == O_FSET_C || op->id == O_FSET_I) {
            u32 predicate = B(insn[i], 39, 3);
            if (predicate < 7) pred_used[predicate] = 1;
            if (B(insn[i], 54, 1) || B(insn[i], 44, 1)) needs_glsl = 1;
        }
        if (op->id == O_TEXS) {
            u32 off = B(insn[i], 36, 13);
            int shadow = B(insn[i], 53, 4) == 4;
            int slot = 0;
            for (; slot < texture_count; slot++)
                if (textures[slot].cbuf_off == off && textures[slot].shadow == shadow) break;
            if (slot == texture_count) {
                if (texture_count >= 32) return NULL;
                textures[slot].cbuf_off = off;
                textures[slot].type = 1; /* 2D */
                textures[slot].shadow = (u8)shadow;
                texture_count++;
            }
            if (si->tex_int_mask & (1u << slot)) return NULL;
            if (a < 255) used[a] = 1;
            if (b < 255) used[b] = 1;
            if (shadow && a + 1 < 255) used[a + 1] = 1;
            u32 regs[4], comps[4];
            int outputs = texs_outputs(insn[i], regs, comps);
            for (int k = 0; k < outputs; k++)
                if (regs[k] < 255) used[regs[k]] = 1;
        }
        if (op->id == O_IPA) {
            u32 addr = B(insn[i], 30, 8) * 4;
            if (addr >= 0x80) {
                u32 input = (addr - 0x80) >> 4;
                u32 component = (addr >> 2) & 3;
                u32 interpolation = (sph[6 + input / 4] >> ((input % 4) * 8 + component * 2)) & 3;
                inputs_used[input] = 1;
                if (interpolation == 2) needs_fragcoord = 1;
            } else
                needs_fragcoord = 1;
            if (addr == 0x70 || addr == 0x74) needs_scale = 1;
            if (B(insn[i], 54, 2) == 1 && b < 255) used[b] = 1;
        }
        if (op->id == O_EXIT && flow_always(insn[i])) has_exit = 1;
    }
    if (!has_exit || (fp32_flush && fp32_preserve)) return NULL;
    u32* pop_targets = has_stack ? calloc(n, sizeof *pop_targets) : NULL;
    if (has_stack && (!pop_targets || !shader_resolve_crs(insn, n, pop_targets))) {
        free(pop_targets);
        return NULL;
    }

    DirectCtx* c = calloc(1, sizeof *c);
    if (!c) {
        free(pop_targets);
        return NULL;
    }
    spv_emit_init(&c->emit, (size_t)n * 20 + 128);
    spv_emit_shader_capability(&c->emit);
    if (needs_glsl) c->glsl = spv_emit_glsl450_import(&c->emit);
    u32 fn = spv_emit_id(&c->emit), label = spv_emit_id(&c->emit);
    u32 void_type = spv_emit_type_void(&c->emit);
    c->u32_type = spv_emit_type_u32(&c->emit);
    c->f32_type = spv_emit_type_f32(&c->emit);
    c->bool_type = spv_emit_type_bool(&c->emit);
    c->true_value = spv_emit_constant_bool(&c->emit, c->bool_type, 1);
    c->false_value = spv_emit_constant_bool(&c->emit, c->bool_type, 0);
    u32 vec4 = spv_emit_type_vec4(&c->emit, c->f32_type);
    u32 vec2 = spv_emit_type_vec2(&c->emit, c->f32_type);
    u32 fn_type = spv_emit_type_function_void(&c->emit, void_type);
    u32 reg_ptr = spv_emit_type_function_pointer(&c->emit, c->u32_type);
    c->zero = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
    u32 zero_f = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
    u32 outputs[8], inputs[32] = {0}, output_attrs[32] = {0}, position = 0, point_size = 0, interfaces[96], count = 0;
    if (stage == ST_FS)
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) {
                outputs[count] = spv_emit_output_vec4(&c->emit, vec4, rt);
                interfaces[count] = outputs[count];
                count++;
            }
    if (stage == ST_VS) {
        point_size = spv_emit_point_size_output(&c->emit, c->f32_type);
        interfaces[count++] = point_size;
        if (position_used) {
            position = spv_emit_position_output(&c->emit, vec4);
            interfaces[count++] = position;
        }
        for (u32 i = 0; i < 32; i++)
            if (outputs_used[i]) {
                output_attrs[i] = spv_emit_output_vec4(&c->emit, vec4, i);
                interfaces[count++] = output_attrs[i];
            }
    }
    for (u32 i = 0; i < 32; i++) {
        if (!inputs_used[i]) continue;
        u32 interpolation = 0;
        if (stage == ST_FS) {
            u32 mode_byte = ((const u8*)sph)[0x18 + i];
            u32 k = 0;
            while (k < 3 && !((mode_byte >> (k * 2)) & 3)) k++;
            u32 mode = (mode_byte >> (k * 2)) & 3;
            interpolation = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        }
        inputs[i] = spv_emit_input_vec4(&c->emit, vec4, i, interpolation);
        interfaces[count++] = inputs[i];
    }
    u32 fragcoord = 0;
    if (needs_fragcoord) {
        fragcoord = spv_emit_fragcoord_input(&c->emit, vec4);
        interfaces[count++] = fragcoord;
    }
    if (needs_scale) {
        c->scale_buffer =
            spv_emit_uniform_vec4_array(&c->emit, vec4, c->f32_type, c->u32_type, 1, 74, 1, &c->scale_pointer);
        interfaces[count++] = c->scale_buffer;
    }
    for (u32 i = 0; i < 18; i++) {
        if (!cbuf_used[i]) continue;
        c->cbuf[i] =
            spv_emit_uniform_vec4_array(&c->emit, vec4, c->f32_type, c->u32_type, 1, i, 4096, &c->cbuf_pointer[i]);
        interfaces[count++] = c->cbuf[i];
    }
    for (int i = 0; i < texture_count; i++) {
        c->sampler[i] =
            spv_emit_sampler2d(&c->emit, c->f32_type, 1, 34 + (u32)i, textures[i].shadow, &c->sampled_type[i]);
        interfaces[count++] = c->sampler[i];
    }
    u32 push = 0, push_pointer = 0;
    if ((stage == ST_FS && (target & 15)) || (stage == ST_VS && position_used)) {
        push = spv_emit_push_vec4(&c->emit, vec4, c->f32_type, stage == ST_FS ? 16 : 0, &push_pointer);
        interfaces[count++] = push;
    }
    spv_emit_entry_str(&c->emit, stage == ST_FS ? SPV_STAGE_FRAGMENT : SPV_STAGE_VERTEX, fn, "main", interfaces, count);
    if (stage == ST_FS) spv_emit_fragment_origin_upper_left(&c->emit, fn);
    spv_emit_float_controls(&c->emit, fn, fp32_flush, 1);
    spv_emit_function_void(&c->emit, void_type, fn, fn_type);
    spv_emit_label(&c->emit, label);
    for (u32 r = 0; r < 255; r++)
        if (used[r]) c->reg[r] = spv_emit_function_variable_init(&c->emit, reg_ptr, c->zero);
    u32 pred_ptr = spv_emit_type_function_pointer(&c->emit, c->bool_type);
    for (u32 p = 0; p < 7; p++)
        if (pred_used[p]) c->pred[p] = spv_emit_function_variable_init(&c->emit, pred_ptr, c->false_value);
    u32 pc = 0, loop_header = 0, loop_dispatch = 0, dispatch_merge = 0, loop_continue = 0, loop_merge = 0;
    u32 *case_labels = NULL, *case_literals = NULL;
    size_t case_count = 0;
    if (has_flow) {
        u32 start = spv_emit_constant_u32(&c->emit, c->u32_type, 11);
        pc = spv_emit_function_variable_init(&c->emit, reg_ptr, start);
        loop_header = spv_emit_id(&c->emit);
        loop_dispatch = spv_emit_id(&c->emit);
        dispatch_merge = spv_emit_id(&c->emit);
        loop_continue = spv_emit_id(&c->emit);
        loop_merge = spv_emit_id(&c->emit);
        case_labels = calloc(n, sizeof *case_labels);
        case_literals = calloc(n, sizeof *case_literals);
        if (!case_labels || !case_literals) abort();
        for (u32 i = 11; i < n; i++)
            if (IDX_OK(i)) {
                case_literals[case_count] = i;
                case_labels[case_count++] = spv_emit_id(&c->emit);
            }
        if (stage == ST_VS) {
            u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
            spv_emit_store(&c->emit, point_size, one_f);
            u32 components[4] = {zero_f, zero_f, zero_f, one_f};
            u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
            if (position) spv_emit_store(&c->emit, position, empty);
            for (u32 i = 0; i < 32; i++)
                if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
        }
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_header);
        spv_emit_loop_merge(&c->emit, loop_merge, loop_continue);
        spv_emit_branch(&c->emit, loop_dispatch);
        spv_emit_label(&c->emit, loop_dispatch);
        u32 selector = spv_emit_load(&c->emit, c->u32_type, pc);
        spv_emit_selection_merge(&c->emit, dispatch_merge);
        spv_emit_switch_u32(&c->emit, selector, dispatch_merge, case_literals, case_labels, case_count);
    }
    if (stage == ST_VS && !has_flow) {
        u32 one_f = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
        spv_emit_store(&c->emit, point_size, one_f);
        u32 components[4] = {zero_f, zero_f, zero_f, one_f};
        u32 empty = spv_emit_constant_vec4(&c->emit, vec4, components);
        if (position) spv_emit_store(&c->emit, position, empty);
        for (u32 i = 0; i < 32; i++)
            if (output_attrs[i]) spv_emit_store(&c->emit, output_attrs[i], empty);
    }

    size_t case_index = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        if (has_flow) {
            spv_emit_label(&c->emit, case_labels[case_index++]);
            u32 following = spv_emit_constant_u32(&c->emit, c->u32_type, next_instruction(i, n));
            spv_emit_store(&c->emit, pc, following);
        }
        u64 w = insn[i];
        int op = decode(w)->id;
        u32 d = B(w, 0, 8), a = B(w, 8, 8), b = B(w, 20, 8);
        u32 guard_merge = 0;
        if (B(w, 16, 3) != 7 || B(w, 19, 1)) {
            u32 execute = spv_emit_id(&c->emit);
            guard_merge = spv_emit_id(&c->emit);
            u32 condition = pred_read(c, B(w, 16, 3), B(w, 19, 1));
            spv_emit_selection_merge(&c->emit, guard_merge);
            spv_emit_branch_conditional(&c->emit, condition, execute, guard_merge);
            spv_emit_label(&c->emit, execute);
        }
        if (op == O_BRA || op == O_SYNC || op == O_BRK || op == O_CONT) {
            u32 target_index = op == O_BRA ? bra_target(w, i) : pop_targets ? pop_targets[i] : 0;
            u32 destination = spv_emit_constant_u32(&c->emit, c->u32_type, target_index);
            spv_emit_store(&c->emit, pc, destination);
        } else if (op == O_S2R) {
            u32 sr = B(w, 20, 8);
            u32 value = sr == 0x12   ? spv_emit_constant_u32(&c->emit, c->u32_type, 0x3f800000u)
                        : sr == 0x38 ? spv_emit_constant_u32(&c->emit, c->u32_type, 1)
                                     : c->zero;
            reg_write(c, d, value);
        } else if (op == O_ALD) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
            for (u32 k = 0; k < width; k++) {
                u32 location = addr + k * 4, index = (location - 0x80) >> 4;
                u32 vector = spv_emit_load(&c->emit, vec4, inputs[index]);
                u32 value = spv_emit_vec4_extract(&c->emit, c->f32_type, vector, (location >> 2) & 3);
                reg_write(c, d + k, spv_emit_bitcast(&c->emit, c->u32_type, value));
            }
        } else if (op == O_AST) {
            u32 addr = B(w, 20, 10), width = B(w, 47, 2) + 1;
            for (u32 k = 0; k < width; k++) {
                u32 location = addr + k * 4;
                u32 variable = location < 0x80 ? position : output_attrs[(location - 0x80) >> 4];
                u32 vector = spv_emit_load(&c->emit, vec4, variable);
                u32 value = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, d + k));
                vector = spv_emit_vec4_insert(&c->emit, vec4, vector, value, (location >> 2) & 3);
                spv_emit_store(&c->emit, variable, vector);
            }
        } else if (op == O_MOV32I) {
            u32 value = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 20, 32));
            reg_write(c, d, value);
        } else if (op == O_MOV_R || op == O_MOV_C) {
            reg_write(c, d, op == O_MOV_R ? reg_read(c, b) : spv_emit_bitcast(&c->emit, c->u32_type, cbuf_read(c, w)));
        } else if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I || op == O_FMUL_R ||
                   op == O_FMUL_C || op == O_FMUL_I || op == O_FMUL32I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FADD_C || op == O_FMUL_C     ? cbuf_read(c, w)
                        : op == O_FADD_I || op == O_FMUL_I   ? float_imm20(c, w)
                        : op == O_FADD32I || op == O_FMUL32I ? float_imm32(c, w)
                                                             : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            if (op == O_FADD_R || op == O_FADD_C || op == O_FADD_I) {
                left = float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
                right = float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            } else if (op == O_FADD32I) {
                left = float_mod(c, left, B(w, 54, 1), B(w, 56, 1));
                right = float_mod(c, right, B(w, 57, 1), B(w, 53, 1));
            } else if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
                right = float_mod(c, right, 0, B(w, 48, 1));
            }
            u32 result = op == O_FADD_R || op == O_FADD_C || op == O_FADD_I || op == O_FADD32I
                             ? spv_emit_fadd(&c->emit, c->f32_type, left, right)
                             : spv_emit_fmul(&c->emit, c->f32_type, left, right);
            spv_emit_no_contraction(&c->emit, result);
            if (op == O_FMUL_R || op == O_FMUL_C || op == O_FMUL_I) {
                static const float scale[] = {1.0f, 0.5f, 0.25f, 0.125f, 8.0f, 4.0f, 2.0f};
                u32 sc = B(w, 41, 3);
                if (sc) {
                    u32 factor = spv_emit_constant_f32(&c->emit, c->f32_type, scale[sc]);
                    result = spv_emit_fmul(&c->emit, c->f32_type, result, factor);
                    spv_emit_no_contraction(&c->emit, result);
                }
            }
            int sat = op == O_FADD32I ? 0 : op == O_FMUL32I ? B(w, 55, 1) : B(w, 50, 1);
            result = float_saturate(c, result, sat);
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, result));
        } else if (op == O_FFMA_R || op == O_FFMA_RC || op == O_FFMA_CR || op == O_FFMA_I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FFMA_CR ? cbuf_read(c, w)
                        : op == O_FFMA_I
                            ? float_imm20(c, w)
                            : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, op == O_FFMA_RC ? B(w, 39, 8) : b));
            u32 addend =
                op == O_FFMA_RC ? cbuf_read(c, w) : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, B(w, 39, 8)));
            if (B(w, 48, 1)) right = spv_emit_fnegate(&c->emit, c->f32_type, right);
            if (B(w, 49, 1)) addend = spv_emit_fnegate(&c->emit, c->f32_type, addend);
            u32 result = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMA, left, right, addend);
            result = float_saturate(c, result, B(w, 50, 1));
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, result));
        } else if (op == O_FMNMX_R || op == O_FMNMX_C || op == O_FMNMX_I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FMNMX_C   ? cbuf_read(c, w)
                        : op == O_FMNMX_I ? float_imm20(c, w)
                                          : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            left = float_mod(c, left, B(w, 46, 1), B(w, 48, 1));
            right = float_mod(c, right, B(w, 49, 1), B(w, 45, 1));
            u32 is_left_nan = spv_emit_isnan(&c->emit, c->bool_type, left);
            u32 is_right_nan = spv_emit_isnan(&c->emit, c->bool_type, right);
            u32 minimum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMIN, left, right);
            u32 maximum = spv_emit_glsl_binary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FMAX, left, right);
            u32 choose_min = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 value = spv_emit_select(&c->emit, c->f32_type, choose_min, minimum, maximum);
            value = spv_emit_select(&c->emit, c->f32_type, is_right_nan, left, value);
            value = spv_emit_select(&c->emit, c->f32_type, is_left_nan, right, value);
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, value));
        } else if (op == O_FSETP_R || op == O_FSETP_C || op == O_FSETP_I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            left = float_mod(c, left, B(w, 7, 1), B(w, 43, 1));
            u32 right;
            if (op == O_FSETP_C)
                right = cbuf_read(c, w);
            else if (op == O_FSETP_I)
                right = spv_emit_constant_u32(&c->emit, c->f32_type, (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31));
            else
                right = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            right = float_mod(c, right, B(w, 44, 1), B(w, 6, 1));
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            u32 op_bool = B(w, 45, 2);
            u32 yes = bool_combine(c, op_bool, test, pc);
            u32 inverse = spv_emit_bool_not(&c->emit, c->bool_type, test);
            u32 no = bool_combine(c, op_bool, inverse, pc);
            pred_write(c, B(w, 3, 3), yes);
            pred_write(c, B(w, 0, 3), no);
        } else if (op == O_FSET_R || op == O_FSET_C || op == O_FSET_I) {
            u32 left = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 right = op == O_FSET_C   ? cbuf_read(c, w)
                        : op == O_FSET_I ? float_imm20(c, w)
                                         : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
            left = float_mod(c, left, B(w, 54, 1), B(w, 43, 1));
            right = float_mod(c, right, B(w, 44, 1), B(w, 53, 1));
            u32 test = spv_emit_fcompare(&c->emit, c->bool_type, B(w, 48, 4), left, right);
            u32 pc = pred_read(c, B(w, 39, 3), B(w, 42, 1));
            test = bool_combine(c, B(w, 45, 2), test, pc);
            u32 truth = spv_emit_constant_u32(&c->emit, c->u32_type, B(w, 52, 1) ? 0x3f800000u : 0xffffffffu);
            reg_write(c, d, spv_emit_select(&c->emit, c->u32_type, test, truth, c->zero));
        } else if (op == O_IPA) {
            u32 addr = B(w, 30, 8) * 4;
            u32 component = (addr >> 2) & 3;
            u32 value;
            u32 interpolation = 0;
            if (addr >= 0x80) {
                u32 input = (addr - 0x80) >> 4;
                value = spv_emit_load(&c->emit, vec4, inputs[input]);
                value = spv_emit_vec4_extract(&c->emit, c->f32_type, value, component);
                interpolation = (sph[6 + input / 4] >> ((input % 4) * 8 + component * 2)) & 3;
            }
            if (addr >= 0x70 && addr < 0x80) {
                u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
                value = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, component);
                if (addr == 0x7c) {
                    u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                    value = spv_emit_fdiv(&c->emit, c->f32_type, one, value);
                } else if (needs_scale && component < 2) {
                    u32 zero_index = spv_emit_constant_u32(&c->emit, c->u32_type, 0);
                    u32 scale = spv_emit_uniform_component_f32(&c->emit, c->scale_pointer, c->scale_buffer, zero_index,
                                                               zero_index, zero_index, c->f32_type);
                    value = spv_emit_fmul(&c->emit, c->f32_type, value, scale);
                    spv_emit_no_contraction(&c->emit, value);
                }
            } else if (interpolation == 2) {
                u32 coord = spv_emit_load(&c->emit, vec4, fragcoord);
                u32 wcoord = spv_emit_vec4_extract(&c->emit, c->f32_type, coord, 3);
                u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                u32 reciprocal = spv_emit_fdiv(&c->emit, c->f32_type, one, wcoord);
                value = spv_emit_fmul(&c->emit, c->f32_type, value, reciprocal);
                spv_emit_no_contraction(&c->emit, value);
            }
            if (B(w, 54, 2) == 1) {
                u32 rhs = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b));
                value = spv_emit_fmul(&c->emit, c->f32_type, value, rhs);
                spv_emit_no_contraction(&c->emit, value);
            }
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, value));
        } else if (op == O_MUFU || op == O_RRO_R || op == O_RRO_C) {
            u32 source = op == O_MUFU ? a : b;
            u32 value = op == O_RRO_C ? cbuf_read(c, w) : spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, source));
            u32 absolute = op == O_MUFU ? B(w, 46, 1) : B(w, 49, 1);
            u32 negative = op == O_MUFU ? B(w, 48, 1) : B(w, 45, 1);
            if (absolute) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FABS, value);
            if (negative) value = spv_emit_fnegate(&c->emit, c->f32_type, value);
            if (op == O_MUFU) {
                SpvGlsl450Op fn = 0;
                switch (B(w, 20, 4)) {
                    case 0: fn = SPV_GLSL_COS; break;
                    case 1: fn = SPV_GLSL_SIN; break;
                    case 2: fn = SPV_GLSL_EXP2; break;
                    case 3: fn = SPV_GLSL_LOG2; break;
                    case 5: fn = SPV_GLSL_INVERSE_SQRT; break;
                    case 8: fn = SPV_GLSL_SQRT; break;
                    case 4: {
                        u32 one = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                        value = spv_emit_fdiv(&c->emit, c->f32_type, one, value);
                        break;
                    }
                    default: break;
                }
                if (fn) value = spv_emit_glsl_unary(&c->emit, c->f32_type, c->glsl, fn, value);
                if (B(w, 50, 1)) {
                    u32 fzero = spv_emit_constant_f32(&c->emit, c->f32_type, 0.0f);
                    u32 fone = spv_emit_constant_f32(&c->emit, c->f32_type, 1.0f);
                    value = spv_emit_glsl_ternary(&c->emit, c->f32_type, c->glsl, SPV_GLSL_FCLAMP, value, fzero, fone);
                }
            }
            reg_write(c, d, spv_emit_bitcast(&c->emit, c->u32_type, value));
        } else if (op == O_TEXS) {
            u32 off = B(w, 36, 13);
            int shadow = B(w, 53, 4) == 4;
            int slot = 0;
            while (slot < texture_count && (textures[slot].cbuf_off != off || textures[slot].shadow != shadow)) slot++;
            u32 x = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, a));
            u32 yreg = shadow ? a + 1 : b;
            u32 y = spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, yreg));
            u32 uv = spv_emit_vec2_construct(&c->emit, vec2, x, y);
            u32 dref = shadow ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, b)) : 0;
            u32 sample = spv_emit_sample2d(&c->emit, shadow ? c->f32_type : vec4, c->sampled_type[slot],
                                           c->sampler[slot], uv, dref);
            u32 regs[4], comps[4];
            int outputs = texs_outputs(w, regs, comps);
            for (int k = 0; k < outputs; k++) {
                if (regs[k] >= 255) continue;
                u32 component = shadow ? sample : spv_emit_vec4_extract(&c->emit, c->f32_type, sample, comps[k]);
                reg_write(c, regs[k], spv_emit_bitcast(&c->emit, c->u32_type, component));
            }
        } else if (op == O_KIL) {
            spv_emit_kill(&c->emit);
        } else if (op == O_EXIT) {
            if (stage == ST_VS) {
                if (position) vertex_depth_fix(c, position, push, push_pointer, vec4);
            } else {
                u32 reg = 0, output = 0, alpha = zero_f;
                for (u32 rt = 0; rt < 8; rt++) {
                    u32 mask = (target >> (rt * 4)) & 15;
                    if (!mask) continue;
                    u32 components[4];
                    for (u32 k = 0; k < 4; k++, reg++)
                        components[k] =
                            (mask & (1u << k)) ? spv_emit_bitcast(&c->emit, c->f32_type, reg_read(c, reg)) : zero_f;
                    u32 color = spv_emit_vec4_construct(&c->emit, vec4, components);
                    if (rt == 0) alpha = components[3];
                    spv_emit_store(&c->emit, outputs[output++], color);
                }
                if (push) alpha_test(c, push, push_pointer, alpha);
            }
            spv_emit_return(&c->emit);
        }
        if (guard_merge) {
            if (op != O_EXIT && op != O_KIL) spv_emit_branch(&c->emit, guard_merge);
            spv_emit_label(&c->emit, guard_merge);
        } else if ((op == O_EXIT || op == O_KIL) && !has_flow)
            break;
        if (has_flow && (guard_merge || (op != O_EXIT && op != O_KIL))) spv_emit_branch(&c->emit, dispatch_merge);
    }
    if (has_flow) {
        spv_emit_label(&c->emit, dispatch_merge);
        spv_emit_branch(&c->emit, loop_continue);
        spv_emit_label(&c->emit, loop_continue);
        spv_emit_branch(&c->emit, loop_header);
        spv_emit_label(&c->emit, loop_merge);
        spv_emit_return(&c->emit);
    }
    spv_emit_function_end(&c->emit);
    u32* result = spv_emit_finish(&c->emit, word_count);
    spv_emit_destroy(&c->emit);
    free(c);
    free(case_labels);
    free(case_literals);
    free(pop_targets);
    ShaderInfo next = {0};
    next.stage = stage;
    next.attr_int_mask = si->attr_int_mask;
    next.attr_sint_mask = si->attr_sint_mask;
    next.clip_enable = si->clip_enable;
    next.rt_int_mask = si->rt_int_mask;
    next.rt_sint_mask = si->rt_sint_mask;
    next.tex_int_mask = si->tex_int_mask;
    next.tex_sint_mask = si->tex_sint_mask;
    next.ntex = texture_count;
    next.fp32_flush = fp32_flush;
    next.fp32_preserve = fp32_preserve;
    next.uses_position_in = needs_fragcoord;
    memcpy(next.tex, textures, (size_t)texture_count * sizeof textures[0]);
    for (u32 i = 0; i < 18; i++)
        if (cbuf_used[i]) {
            next.cbuf_mask |= 1u << i;
            next.cbuf_max[i] = cbuf_max[i];
        }
    for (u32 i = 0; i < 32; i++)
        if (inputs_used[i]) next.in_mask |= 1u << i;
    memcpy(next.sph, sph, sizeof sph);
    *si = next;
    return result;
}
