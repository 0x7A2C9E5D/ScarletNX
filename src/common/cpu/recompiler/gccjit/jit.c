/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "jit.h"
#include "cpu.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- context ---- */

/* -march=native is resolved by GCC's driver, which libgccjit's compiler proper doesn't run through: ask the driver
   once what it means on this host (build == host == target) and pass the CPU by name. Generic x86-64 without one */
static const char* host_march(void) {
    static char option[96];
    static int done;
    if (!done) {
        const char* args[5] = {"gcc", "-march=native", "-Q", "--help=target", NULL};
        char *gcc = snx_find_tool("gcc"), *output = NULL, *p;
        done = 1;
        if (gcc) {
            args[0] = gcc;
            if (!snx_run(args, NULL, NULL, &output) && output && (p = strstr(output, "-march=")) != NULL) {
                char name[64];
                p += 7;
                while (*p == ' ' || *p == '\t') ++p;
                if (sscanf(p, "%63s", name) == 1 && strcmp(name, "native")) sprintf(option, "-march=%s", name);
            }
            free(output);
            free(gcc);
        }
    }
    return option[0] ? option : NULL;
}

SnxJit* snx_jit_new(char opt) {
    SnxJit* jit = (SnxJit*)calloc(1, sizeof(*jit));
    gcc_jit_type* params[2];
    if (!jit) return NULL;
    jit->ctx = gcc_jit_context_acquire();
    if (opt == 's' || opt == 'z') {
        gcc_jit_context_set_int_option(jit->ctx, GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL, 2);
        gcc_jit_context_add_command_line_option(jit->ctx, "-Os");
    } else
        gcc_jit_context_set_int_option(jit->ctx, GCC_JIT_INT_OPTION_OPTIMIZATION_LEVEL, opt - '0');
    /* a shared object for this machine (build == host == target), the flags toolchain.c gives generated C */
    gcc_jit_context_add_command_line_option(jit->ctx, "-fPIC");
    if (host_march()) gcc_jit_context_add_command_line_option(jit->ctx, host_march());
    gcc_jit_context_add_command_line_option(jit->ctx, "-fno-strict-aliasing");
    gcc_jit_context_add_command_line_option(jit->ctx, "-fwrapv");
    gcc_jit_context_add_command_line_option(jit->ctx, "-foptimize-sibling-calls");
    jit->t_void = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_VOID);
    jit->t_ptr = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_VOID_PTR);
    jit->t_char_ptr = gcc_jit_type_get_pointer(gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_CHAR));
    jit->t_bool = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_BOOL);
    jit->t_int = gcc_jit_context_get_type(jit->ctx, GCC_JIT_TYPE_INT);
    jit->t_u8 = gcc_jit_context_get_int_type(jit->ctx, 1, 0);
    jit->t_u16 = gcc_jit_context_get_int_type(jit->ctx, 2, 0);
    jit->t_u32 = gcc_jit_context_get_int_type(jit->ctx, 4, 0);
    jit->t_u64 = gcc_jit_context_get_int_type(jit->ctx, 8, 0);
    params[0] = jit->t_ptr;
    params[1] = jit->t_u64;
    jit->t_guest_ptr = gcc_jit_context_new_function_ptr_type(jit->ctx, NULL, jit->t_void, 2, params, 0);
    return jit;
}

void snx_jit_free(SnxJit* jit) {
    if (!jit) return;
    gcc_jit_context_release(jit->ctx);
    free(jit->addresses);
    free(jit->functions);
    free(jit);
}

const char* snx_jit_error(SnxJit* jit) { return gcc_jit_context_get_first_error(jit->ctx); }

/* ---- values ---- */

gcc_jit_rvalue* jit_u64(SnxJit* jit, SnxU64 v) {
    return gcc_jit_context_new_rvalue_from_long(jit->ctx, jit->t_u64, (long)v);
}
gcc_jit_rvalue* jit_u32(SnxJit* jit, SnxU32 v) {
    return gcc_jit_context_new_rvalue_from_long(jit->ctx, jit->t_u32, (long)v);
}

gcc_jit_lvalue* jit_at(SnxJit* jit, gcc_jit_rvalue* base, size_t offset, gcc_jit_type* type) {
    gcc_jit_rvalue* bytes = gcc_jit_context_new_cast(jit->ctx, NULL, base, jit->t_char_ptr);
    gcc_jit_lvalue* at = gcc_jit_context_new_array_access(
        jit->ctx, NULL, bytes, gcc_jit_context_new_rvalue_from_long(jit->ctx, jit->t_int, (long)offset));
    gcc_jit_rvalue* pointer =
        gcc_jit_context_new_cast(jit->ctx, NULL, gcc_jit_lvalue_get_address(at, NULL), gcc_jit_type_get_pointer(type));
    return gcc_jit_rvalue_dereference(pointer, NULL);
}
static gcc_jit_lvalue* cpu(SnxJit* jit, size_t offset, gcc_jit_type* type) { return jit_at(jit, jit->c, offset, type); }
static gcc_jit_rvalue* load(gcc_jit_lvalue* lvalue) { return gcc_jit_lvalue_as_rvalue(lvalue); }

gcc_jit_block* jit_block(SnxJit* jit) {
    char name[32];
    sprintf(name, "b%lu", jit->names++);
    return gcc_jit_function_new_block(jit->fn, name);
}

static gcc_jit_lvalue* local(SnxJit* jit, gcc_jit_type* type) {
    char name[32];
    sprintf(name, "t%lu", jit->names++);
    return gcc_jit_function_new_local(jit->fn, NULL, type, name);
}
static void assign(SnxJit* jit, gcc_jit_lvalue* lvalue, gcc_jit_rvalue* value) {
    gcc_jit_block_add_assignment(jit->block, NULL, lvalue, value);
}
static void ret(SnxJit* jit) {
    gcc_jit_block_end_with_void_return(jit->block, NULL);
    jit->block = NULL;
}
static void jump(SnxJit* jit, gcc_jit_block* target) {
    gcc_jit_block_end_with_jump(jit->block, NULL, target);
    jit->block = NULL;
}
static gcc_jit_rvalue* compare(SnxJit* jit, enum gcc_jit_comparison op, gcc_jit_rvalue* a, gcc_jit_rvalue* b) {
    return gcc_jit_context_new_comparison(jit->ctx, NULL, op, a, b);
}

/* ---- functions ---- */

gcc_jit_function* snx_jit_guest_function(SnxJit* jit, SnxU64 address, int define) {
    char name[32];
    size_t i;
    gcc_jit_param* params[2];
    gcc_jit_function* f;
    for (i = 0; i < jit->count; ++i)
        if (jit->addresses[i] == address) return jit->functions[i];
    if (address >> 32)
        sprintf(name, "F_%lX%08lX", (unsigned long)(address >> 32), (unsigned long)(address & 0xFFFFFFFFUL));
    else
        sprintf(name, "F_%lX", (unsigned long)address);
    params[0] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_ptr, "c");
    params[1] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_u64, "e");
    f = gcc_jit_context_new_function(jit->ctx, NULL, define ? GCC_JIT_FUNCTION_EXPORTED : GCC_JIT_FUNCTION_IMPORTED,
                                     jit->t_void, name, 2, params, 0);
    if (jit->count == jit->capacity) {
        jit->capacity = jit->capacity ? jit->capacity * 2 : 256;
        jit->addresses = (SnxU64*)realloc(jit->addresses, jit->capacity * sizeof(*jit->addresses));
        jit->functions = (gcc_jit_function**)realloc(jit->functions, jit->capacity * sizeof(*jit->functions));
    }
    jit->addresses[jit->count] = address;
    jit->functions[jit->count++] = f;
    return f;
}

/* snx_step(c, w, pc): the interpreter's step for one instruction, in the runtime library */
static gcc_jit_rvalue* step(SnxJit* jit, SnxU32 word, SnxU64 pc) {
    gcc_jit_rvalue* args[3];
    if (!jit->step) {
        gcc_jit_param* params[3];
        params[0] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_ptr, "c");
        params[1] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_u32, "w");
        params[2] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_u64, "pc");
        jit->step = gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_IMPORTED, jit->t_u64, "snx_step", 3,
                                                 params, 0);
    }
    args[0] = jit->c;
    args[1] = jit_u32(jit, word);
    args[2] = jit_u64(jit, pc);
    return gcc_jit_context_new_call(jit->ctx, NULL, jit->step, 3, args);
}

/* snx_lookup (cpu.h): the guest function at pc, or the interpreter */
static gcc_jit_function* lookup_function(SnxJit* jit) {
    gcc_jit_param* params[2];
    gcc_jit_function* f;
    gcc_jit_block *entry, *table, *found, *none;
    gcc_jit_rvalue *c, *pc, *lo, *o, *inside, *entry_fn;
    gcc_jit_lvalue* fn;
    if (jit->lookup) return jit->lookup;
    params[0] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_ptr, "c");
    params[1] = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_u64, "pc");
    f = gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_ALWAYS_INLINE, jit->t_guest_ptr, "snx_lookup", 2,
                                     params, 0);
    c = gcc_jit_param_as_rvalue(params[0]);
    pc = gcc_jit_param_as_rvalue(params[1]);
    entry = gcc_jit_function_new_block(f, "entry");
    table = gcc_jit_function_new_block(f, "table");
    found = gcc_jit_function_new_block(f, "found");
    none = gcc_jit_function_new_block(f, "none");
    lo = load(jit_at(jit, c, offsetof(CPU, code_lo), jit->t_u64));
    o = gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_MINUS, jit->t_u64, pc, lo);
    inside = gcc_jit_context_new_binary_op(
        jit->ctx, NULL, GCC_JIT_BINARY_OP_LOGICAL_AND, jit->t_bool,
        compare(jit, GCC_JIT_COMPARISON_LT, o,
                gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_MINUS, jit->t_u64,
                                              load(jit_at(jit, c, offsetof(CPU, code_hi), jit->t_u64)), lo)),
        compare(jit, GCC_JIT_COMPARISON_EQ,
                gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_BITWISE_AND, jit->t_u64, pc,
                                              jit_u64(jit, 3)),
                jit_u64(jit, 0)));
    gcc_jit_block_end_with_conditional(entry, NULL, inside, table, none);
    fn = gcc_jit_function_new_local(f, NULL, jit->t_guest_ptr, "fn");
    gcc_jit_block_add_assignment(
        table, NULL, fn,
        load(gcc_jit_context_new_array_access(
            jit->ctx, NULL,
            gcc_jit_context_new_cast(jit->ctx, NULL, load(jit_at(jit, c, offsetof(CPU, disp), jit->t_ptr)),
                                     gcc_jit_type_get_pointer(jit->t_guest_ptr)),
            gcc_jit_context_new_binary_op(jit->ctx, NULL, GCC_JIT_BINARY_OP_RSHIFT, jit->t_u64, o, jit_u64(jit, 2)))));
    gcc_jit_block_end_with_conditional(
        table, NULL, compare(jit, GCC_JIT_COMPARISON_NE, load(fn), gcc_jit_context_null(jit->ctx, jit->t_guest_ptr)),
        found, none);
    gcc_jit_block_end_with_return(found, NULL, load(fn));
    if (!jit->interp_entry)
        jit->interp_entry =
            gcc_jit_context_new_global(jit->ctx, NULL, GCC_JIT_GLOBAL_IMPORTED, jit->t_guest_ptr, "snx_interp_entry");
    entry_fn = load(jit->interp_entry);
    gcc_jit_block_end_with_return(none, NULL, entry_fn);
    return jit->lookup = f;
}

/* CALLDYN(t): snx_lookup(c,t)(c,t) */
static gcc_jit_rvalue* call_dynamic(SnxJit* jit, gcc_jit_rvalue* target) {
    gcc_jit_rvalue *args[2], *fn;
    args[0] = jit->c;
    args[1] = target;
    fn = gcc_jit_context_new_call(jit->ctx, NULL, lookup_function(jit), 2, args);
    return gcc_jit_context_new_call_through_ptr(jit->ctx, NULL, fn, 2, args);
}
/* a call the C follows with return: required to become a jump, so guest tail calls never grow the host stack */
static void tail(SnxJit* jit, gcc_jit_rvalue* call) {
    gcc_jit_rvalue_set_bool_require_tail_call(call, 1);
    gcc_jit_block_add_eval(jit->block, NULL, call);
    ret(jit);
}

static void call(SnxJit* jit, const SnxCodegen* codegen, SnxU64 target, int is_tail) {
    gcc_jit_rvalue *args[2], *site;
    args[0] = jit->c;
    args[1] = jit_u64(jit, target);
    if (snx_codegen_function(codegen, target))
        site = gcc_jit_context_new_call(jit->ctx, NULL, snx_jit_guest_function(jit, target, 0), 2, args);
    else
        site = call_dynamic(jit, args[1]);
    if (is_tail)
        tail(jit, site);
    else
        gcc_jit_block_add_eval(jit->block, NULL, site);
}

static size_t index_of(const SnxFunction* function, SnxU64 address) {
    size_t lo = 0, hi = function->count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (snx_function_address(function, middle) < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo;
}

static void branch(SnxJit* jit, const SnxCodegen* codegen, const SnxFunction* function, gcc_jit_block** labels,
                   SnxU64 target) {
    if (snx_function_contains(function, target))
        jump(jit, labels[index_of(function, target)]);
    else
        call(jit, codegen, target, 1);
}

/* if(c->pc!=pc+4) return; */
static void check_return(SnxJit* jit, SnxU64 next) {
    gcc_jit_block *leave = jit_block(jit), *stay = jit_block(jit);
    gcc_jit_block_end_with_conditional(
        jit->block, NULL,
        compare(jit, GCC_JIT_COMPARISON_NE, load(cpu(jit, offsetof(CPU, pc), jit->t_u64)), jit_u64(jit, next)), leave,
        stay);
    gcc_jit_block_end_with_void_return(leave, NULL);
    jit->block = stay;
}

static void begin(SnxJit* jit, gcc_jit_function* function) {
    jit->fn = function;
    jit->c = gcc_jit_param_as_rvalue(gcc_jit_function_get_param(function, 0));
    jit->block = gcc_jit_function_new_block(function, "entry");
}

void snx_jit_emit(SnxJit* jit, SnxCodegen* codegen, const SnxFunction* function) {
    gcc_jit_function* fn = snx_jit_guest_function(jit, function->start, 1);
    gcc_jit_block **labels, *dispatch_block = NULL, *mismatch, *body;
    gcc_jit_rvalue *e, *t, *next_pc;
    gcc_jit_lvalue* pc = NULL;
    size_t i;
    SnxU64 address, next, target;
    SnxU32 word;
    const SnxA64Case* c;
    int dispatch, terminal;
    labels = (gcc_jit_block**)calloc(function->count ? function->count : 1, sizeof(*labels));
    begin(jit, fn);
    e = gcc_jit_param_as_rvalue(gcc_jit_function_get_param(fn, 1));
    for (i = 0; i < function->count; ++i)
        if (function->leaders[i]) labels[i] = jit_block(jit);
    dispatch = function->leader_count > 1;
    if (dispatch) dispatch_block = jit_block(jit);
    pc = cpu(jit, offsetof(CPU, pc), jit->t_u64);
    /* if(e!=start){ c->pc=e; goto D_; } or { CALLDYN(e); return; } */
    mismatch = jit_block(jit);
    body = jit_block(jit);
    gcc_jit_block_end_with_conditional(
        jit->block, NULL, compare(jit, GCC_JIT_COMPARISON_NE, e, jit_u64(jit, function->start)), mismatch, body);
    jit->block = mismatch;
    if (dispatch) {
        assign(jit, pc, e);
        jump(jit, dispatch_block);
    } else
        tail(jit, call_dynamic(jit, e));
    jit->block = body;
    if (function->count && snx_function_address(function, 0) != function->start)
        jump(jit, labels[index_of(function, function->start)]);
    for (i = 0; i < function->count; ++i) {
        address = snx_function_address(function, i);
        next = address + 4;
        if (function->leaders[i]) {
            if (jit->block) jump(jit, labels[i]);
            jit->block = labels[i];
        }
        if (!jit->block) continue; /* unreachable after a terminator until the next label, as in the C */
        word = 0;
        if (!snx_codegen_word(codegen, address, &word))
            c = snx_a64_decode(0);
        else
            c = snx_a64_decode(word);
        if (c->kind == SNX_FLOW_UNDEF) { /* c->pc=pc; c->rt->undef(c,pc,w); return; (the step makes the call) */
            assign(jit, pc, jit_u64(jit, address));
            gcc_jit_block_add_eval(jit->block, NULL, step(jit, word, address));
            ret(jit);
            continue;
        }
        terminal = 0;
        switch (c->kind) {
            case SNX_FLOW_PLAIN: gcc_jit_block_add_eval(jit->block, NULL, step(jit, word, address)); break;
            case SNX_FLOW_B:
                branch(jit, codegen, function, labels, snx_a64_target(word, address));
                terminal = 1;
                break;
            case SNX_FLOW_BC: { /* the step returns the target when taken, pc+4 otherwise */
                gcc_jit_block *taken = jit_block(jit), *fall = jit_block(jit);
                target = snx_a64_target(word, address);
                next_pc = step(jit, word, address);
                gcc_jit_block_end_with_conditional(
                    jit->block, NULL, compare(jit, GCC_JIT_COMPARISON_EQ, next_pc, jit_u64(jit, target)), taken, fall);
                jit->block = taken;
                branch(jit, codegen, function, labels, target);
                jit->block = fall;
                break;
            }
            case SNX_FLOW_BL:
                assign(jit, cpu(jit, offsetof(CPU, x) + 8 * 30, jit->t_u64), jit_u64(jit, next));
                call(jit, codegen, snx_a64_target(word, address), 0);
                check_return(jit, next);
                break;
            case SNX_FLOW_BLR: { /* the step sets x30 and returns the target */
                gcc_jit_lvalue* target_local = local(jit, jit->t_u64);
                assign(jit, target_local, step(jit, word, address));
                gcc_jit_block_add_eval(jit->block, NULL, call_dynamic(jit, load(target_local)));
                check_return(jit, next);
                break;
            }
            case SNX_FLOW_BR:
                t = step(jit, word, address);
                if (dispatch) {
                    assign(jit, pc, t);
                    jump(jit, dispatch_block);
                } else {
                    gcc_jit_lvalue* target_local = local(jit, jit->t_u64);
                    assign(jit, target_local, t);
                    tail(jit, call_dynamic(jit, load(target_local)));
                }
                terminal = 1;
                break;
            case SNX_FLOW_RET:
                assign(jit, pc, step(jit, word, address));
                ret(jit);
                terminal = 1;
                break;
            case SNX_FLOW_SVC: /* the step sets c->pc=pc+4 and calls c->rt->svc */
                gcc_jit_block_add_eval(jit->block, NULL, step(jit, word, address));
                break;
            case SNX_FLOW_BRK:
                assign(jit, pc, jit_u64(jit, address));
                gcc_jit_block_add_eval(jit->block, NULL, step(jit, word, address));
                ret(jit);
                terminal = 1;
                break;
            case SNX_FLOW_UNDEF: break;
        }
        if (!terminal && jit->block &&
            (!snx_function_contains(function, next) ||
             (i + 1 < function->count && snx_function_address(function, i + 1) != next)))
            call(jit, codegen, next, 1);
    }
    if (jit->block) ret(jit);
    if (dispatch) {
        gcc_jit_case** cases = (gcc_jit_case**)calloc(function->leader_count, sizeof(*cases));
        gcc_jit_block* other = jit_block(jit);
        size_t n = 0;
        for (i = 0; i < function->count; ++i)
            if (function->leaders[i]) {
                gcc_jit_rvalue* value = jit_u64(jit, snx_function_address(function, i));
                cases[n++] = gcc_jit_context_new_case(jit->ctx, value, value, labels[i]);
            }
        gcc_jit_block_end_with_switch(dispatch_block, NULL, load(pc), other, (int)n, cases);
        free(cases);
        jit->block = other;
        tail(jit, call_dynamic(jit, load(pc)));
    }
    free(labels);
}

gcc_jit_function* snx_jit_static_step(SnxJit* jit, SnxU32 word, SnxU64 pc, const char* name) {
    gcc_jit_param* param = gcc_jit_context_new_param(jit->ctx, NULL, jit->t_ptr, "c");
    gcc_jit_function* fn =
        gcc_jit_context_new_function(jit->ctx, NULL, GCC_JIT_FUNCTION_EXPORTED, jit->t_u64, name, 1, &param, 0);
    const SnxA64Case* c = snx_a64_decode(word);
    begin(jit, fn);
    if (c->kind == SNX_FLOW_PLAIN) { /* snx_a64_static_step returns pc+4 even after an UNDEF in the body */
        gcc_jit_block_add_eval(jit->block, NULL, step(jit, word, pc));
        gcc_jit_block_end_with_return(jit->block, NULL, jit_u64(jit, pc + 4));
    } else
        gcc_jit_block_end_with_return(jit->block, NULL, step(jit, word, pc));
    jit->block = NULL;
    return fn;
}
