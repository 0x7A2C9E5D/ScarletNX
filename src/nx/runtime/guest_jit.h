/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What runs a pc the game module has no function for (RtAPI.miss, rtapi.c): the module's AOT variant table
   (common/cpu/recompiler/variants.h), linked in at build time from the profile run's cache. There is no runtime
   compiler and no interpreter: a pc the pack has no variant for ends the game */
#ifndef SNX_RUNTIME_GUEST_JIT_H
#define SNX_RUNTIME_GUEST_JIT_H
#include "common/cpu/aarch64/cpu.h"

/* once the game's executables are loaded, the dispatch table built and the variant table stored, before any guest
   thread runs: builds the images the hash reads and registers the invalidation. A module without a table leaves a
   warning, and the first miss ends the game */
void jit_init(void);
/* the AOT function for pc: the dispatch table inside the code range, else the variant table. NULL when the pack has
   no variant whose current bytes match */
GuestFn jit_resolve(u64 pc);
/* jit_resolve, and for a pc in executable memory no image covers (a block the game mapped itself, mem_query), the
   block from pc is registered as an image for the hash. NULL when the pack has no variant for it: a pc in freed
   memory, one that isn't code, or one with every image slot taken */
GuestFn jit_resolve_or_add(u64 pc);
/* the variant table's function for pc after checking the current bytes; NULL when no variant matches */
GuestFn jit_wait(u64 pc);
/* why jit_wait answered NULL: "the AOT pack has no code for it", or why the table could not be set up */
const char* jit_failure(void);
/* Code the game maps while it runs (an NRO, hle/services/ldr.c), text_size bytes of text at base out of size: it is
   registered as an image for the hash. 0, or -1 (no table, or as many such images as it keeps): the code has no pack
   variant, so its first call ends the game */
int jit_code_added(u64 base, u64 text_size, u64 size);
/* before the memory at base is unmapped: forget its image */
void jit_code_removed(u64 base);
/* a mapping, permission or write-back change under [addr, addr+size) (mem_set_code_invalidate): the dispatch slots in
   the range are cleared, and a block the game mapped that is no longer executable loses its image */
void jit_code_invalidate(u64 addr, u64 size);
/* the totals, into the log, at exit */
void jit_report(void);

#endif
