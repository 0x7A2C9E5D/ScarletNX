/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GDB's JIT interface for the JIT loader (code_loader.c): each object a load placed in the arena is handed to an
   attached GDB, which then names the functions in it (and has the object's .eh_frame to unwind with). Off unless
   asked (snx_gdb_jit_enable: the NX runtime's gdb_jit with the GDB stub): GDB stops the process at every
   registration to read the object */
#ifndef SNX_RECOMPILER_LLVM_GDB_JIT_H
#define SNX_RECOMPILER_LLVM_GDB_JIT_H
#include "common/cpu/recompiler/llvm/elf_object.h"

/* Registrations from here on, until snx_gdb_jit_enable(0); before any loader loads */
void snx_gdb_jit_enable(int on);
#if !defined(_WIN32)
/* Registers a copy of o, a load's object, with its loaded sections at the addresses the load gave them, for owner (a
   loader) to remove with snx_gdb_jit_remove. Nothing when registration is off or out of memory. ELF objects only:
   Windows' loader takes COFF (code_loader_coff.c), and elf_object.h has nothing there */
void snx_gdb_jit_register(const void* owner, const SnxElfObject* o);
#endif
/* Removes every object owner registered: before its code goes */
void snx_gdb_jit_remove(const void* owner);
#endif
