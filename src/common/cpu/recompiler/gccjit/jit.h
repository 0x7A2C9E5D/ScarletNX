/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The libgccjit backend (--backend=gcc), for hosts LLVM doesn't generate code for (DEC Alpha EV67); elsewhere the
   LLVM backend is the one to use. A first port: the control flow is the LLVM backend's (ir_codegen.c: labels, the D_
   dispatch, required tail calls, CALLDYN, BL's return check, the module descriptor), and every instruction calls the
   interpreter's snx_step (a64_templates.inc's C, in the runtime library), branching on what it
   returns for flow instructions. Inline instruction bodies come later. */
#ifndef SNX_RECOMPILER_GCCJIT_JIT_H
#define SNX_RECOMPILER_GCCJIT_JIT_H

#include "codegen.h"
#include <libgccjit.h>

typedef struct SnxJit {
    gcc_jit_context* ctx;
    gcc_jit_type *t_void, *t_ptr, *t_char_ptr, *t_bool, *t_int, *t_u8, *t_u16, *t_u32, *t_u64, *t_guest, *t_guest_ptr;
    gcc_jit_function* step;   /* u64 snx_step(CPU*, u32 w, u64 pc), in the runtime library */
    gcc_jit_function* lookup; /* snx_lookup */
    gcc_jit_lvalue* interp_entry;
    /* F_<address> declared in this context: defined here (exported) or elsewhere (imported) */
    SnxU64* addresses;
    gcc_jit_function** functions;
    size_t count, capacity;
    /* the function being emitted */
    gcc_jit_function* fn;
    gcc_jit_rvalue* c;
    gcc_jit_block* block; /* NULL after a terminator, until the next label */
    unsigned long names;
} SnxJit;

/* opt: the -O letter */
SnxJit* snx_jit_new(char opt);
void snx_jit_free(SnxJit* jit);
/* NULL, or the context's first error */
const char* snx_jit_error(SnxJit* jit);

/* F_<address>: exported (defined in this context) when define, imported otherwise; declare defined ones first */
gcc_jit_function* snx_jit_guest_function(SnxJit* jit, SnxU64 address, int define);
void snx_jit_emit(SnxJit* jit, SnxCodegen* codegen, const SnxFunction* function);
/* u64 name(CPU* c): one instruction, as snx_a64_static_step returns (tests) */
gcc_jit_function* snx_jit_static_step(SnxJit* jit, SnxU32 word, SnxU64 pc, const char* name);

/* building blocks for the module descriptor */
gcc_jit_rvalue* jit_u64(SnxJit* jit, SnxU64 v);
gcc_jit_rvalue* jit_u32(SnxJit* jit, SnxU32 v);
gcc_jit_lvalue* jit_at(SnxJit* jit, gcc_jit_rvalue* base, size_t offset, gcc_jit_type* type); /* *(type*)(base+off) */
gcc_jit_block* jit_block(SnxJit* jit);

#endif
